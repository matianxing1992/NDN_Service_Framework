#include "UavVideoPipeline.hpp"
#include "config.hpp"

#include <stdexcept>
#include <utility>

#if HAVE_GSTREAMER
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <condition_variable>
#include <limits>
#include <map>
#include <sstream>
#endif

namespace ndnsf::examples::uav {

UavVideoSampleClassSchedule
UavVideoSampleClassSchedule::exactKeyDelta(uint32_t fps,
                                          size_t hardMaxSources,
                                          uint64_t sessionGeneration)
{
  return {UavVideoSampleClassMode::ExactKeyDelta, fps, hardMaxSources,
          sessionGeneration};
}

UavVideoSampleClassSchedule
UavVideoSampleClassSchedule::boundedOpaque(uint32_t fps,
                                           size_t hardMaxSources,
                                           uint64_t sessionGeneration)
{
  return {UavVideoSampleClassMode::BoundedOpaque, fps, hardMaxSources,
          sessionGeneration};
}

UavVideoSampleClassSchedule::UavVideoSampleClassSchedule(
  UavVideoSampleClassMode mode, uint32_t fps, size_t hardMaxSources,
  uint64_t sessionGeneration)
  : m_mode(mode)
  , m_fps(fps)
  , m_hardMaxSources(hardMaxSources)
  , m_sessionGeneration(sessionGeneration)
{
  if (fps == 0 || fps > 60) {
    throw std::invalid_argument("video sample-class FPS must be in [1,60]");
  }
  if (hardMaxSources == 0) {
    throw std::invalid_argument("video sample-class source bound must be positive");
  }
  if (sessionGeneration == 0) {
    throw std::invalid_argument("video sample-class session generation must be positive");
  }
}

UavVideoSampleClassMode
UavVideoSampleClassSchedule::mode() const
{
  return m_mode;
}

uint32_t
UavVideoSampleClassSchedule::fps() const
{
  return m_fps;
}

size_t
UavVideoSampleClassSchedule::hardMaxSources() const
{
  return m_hardMaxSources;
}

uint64_t
UavVideoSampleClassSchedule::sessionGeneration() const
{
  return m_sessionGeneration;
}

bool
UavVideoSampleClassSchedule::hasExactFrameClass() const
{
  return m_mode == UavVideoSampleClassMode::ExactKeyDelta;
}

std::string
UavVideoSampleClassSchedule::classFor(uint64_t sampleId) const
{
  if (!hasExactFrameClass()) {
    return "opaque";
  }
  return sampleId % m_fps == 0 ? "key" : "delta";
}

bool
UavVideoSampleClassSchedule::matchesActual(uint64_t sampleId,
                                           bool keyFrame) const
{
  if (!hasExactFrameClass()) {
    return false;
  }
  return (classFor(sampleId) == "key") == keyFrame;
}

BoundedLatestFrameQueue::BoundedLatestFrameQueue(size_t maxFrames,
                                                 size_t maxBytes)
  : m_maxFrames(maxFrames)
  , m_maxBytes(maxBytes)
{
  if (maxFrames == 0 || maxBytes == 0) {
    throw std::invalid_argument("video queue bounds must be positive");
  }
}

bool
BoundedLatestFrameQueue::push(UavVideoFrame frame)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (frame.bytes.size() > m_maxBytes) {
    ++m_droppedFrames;
    m_lastDropReason = "frame-exceeds-byte-capacity";
    return false;
  }
  while (!m_frames.empty() &&
         (m_frames.size() >= m_maxFrames ||
          m_queuedBytes + frame.bytes.size() > m_maxBytes)) {
    dropOldest("superseded-by-newer-frame");
  }
  m_queuedBytes += frame.bytes.size();
  m_frames.push_back(std::move(frame));
  return true;
}

std::optional<UavVideoFrame>
BoundedLatestFrameQueue::popLatest()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_frames.empty()) {
    return std::nullopt;
  }
  UavVideoFrame latest = std::move(m_frames.back());
  if (m_frames.size() > 1) {
    m_droppedFrames += m_frames.size() - 1;
    m_lastDropReason = "superseded-by-newer-frame";
  }
  m_frames.clear();
  m_queuedBytes = 0;
  return latest;
}

void
BoundedLatestFrameQueue::clear(const std::string& reason)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_frames.empty()) {
    m_droppedFrames += m_frames.size();
    m_lastDropReason = reason;
  }
  m_frames.clear();
  m_queuedBytes = 0;
}

UavVideoQueueSnapshot
BoundedLatestFrameQueue::snapshot() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return {m_frames.size(), m_queuedBytes, m_droppedFrames, m_lastDropReason};
}

void
BoundedLatestFrameQueue::dropOldest(const std::string& reason)
{
  m_queuedBytes -= m_frames.front().bytes.size();
  m_frames.pop_front();
  ++m_droppedFrames;
  m_lastDropReason = reason;
}

LegacyPipeVideoPipeline::LegacyPipeVideoPipeline(bool headless)
  : m_headless(headless)
{
}

UavVideoPipelineCapabilities
LegacyPipeVideoPipeline::probeCapabilities() const
{
  return {
    "legacy-pipe",
    true,
    false,
    true,
    m_headless,
    false,
    "legacy byte path; codec output PTS association unavailable",
  };
}

void
LegacyPipeVideoPipeline::startDecode(FrameCallback callback)
{
  if (!callback) {
    throw std::invalid_argument("video decode callback is required");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == UavVideoPipelineState::Running) {
    throw std::logic_error("video pipeline is already running");
  }
  m_callback = std::move(callback);
  m_state = UavVideoPipelineState::Running;
}

bool
LegacyPipeVideoPipeline::submitAccessUnit(const UavVideoFrame& frame)
{
  FrameCallback callback;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state != UavVideoPipelineState::Running || !m_callback) {
      return false;
    }
    callback = m_callback;
  }
  callback(frame);
  return true;
}

void
LegacyPipeVideoPipeline::stop()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_callback = {};
  m_state = UavVideoPipelineState::Stopped;
}

UavVideoPipelineState
LegacyPipeVideoPipeline::state() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_state;
}

bool
LegacyPipeVideoPipeline::isHeadless() const
{
  return m_headless;
}

class GStreamerVideoPipeline::Impl
{
public:
  enum class Mode { None, Capture, Decode };

#if HAVE_GSTREAMER
  template<typename Handler>
  static GstFlowReturn
  invokeSampleCallback(Impl* self, const char* direction,
                       Handler&& handler) noexcept
  {
    try {
      return handler();
    }
    catch (const std::exception& error) {
      self->recordFailure(
        direction, std::string(direction) + "-callback-exception",
        error.what() == nullptr ? "std-exception" : error.what());
    }
    catch (...) {
      self->recordFailure(
        direction, std::string(direction) + "-callback-nonstandard-exception",
        "non-standard-exception");
    }
    return GST_FLOW_ERROR;
  }

  static GstFlowReturn
  onCaptureSample(GstAppSink* sink, gpointer userData) noexcept
  {
    auto* self = static_cast<Impl*>(userData);
    return invokeSampleCallback(self, "capture", [self, sink] {
      return self->handleCaptureSample(sink);
    });
  }

  static GstFlowReturn
  onDecodeSample(GstAppSink* sink, gpointer userData) noexcept
  {
    auto* self = static_cast<Impl*>(userData);
    return invokeSampleCallback(self, "decode", [self, sink] {
      return self->handleDecodeSample(sink);
    });
  }

  static GstPadProbeReturn onRawFrame(GstPad*, GstPadProbeInfo* info,
                                      gpointer userData) noexcept
  {
    auto* self = static_cast<Impl*>(userData);
    try {
      auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
      if (buffer == nullptr) {
        return GST_PAD_PROBE_OK;
      }
      std::lock_guard<std::mutex> lock(self->mutex);
      const auto pts = GST_BUFFER_PTS(buffer);
      self->rawTimings[pts] = {
        self->nextSourceFrameId++,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count())};
      while (self->rawTimings.size() > 64) {
        self->rawTimings.erase(self->rawTimings.begin());
      }
      return GST_PAD_PROBE_OK;
    }
    catch (const std::exception& error) {
      self->recordFailure(
        "capture-probe", "capture-probe-exception",
        error.what() == nullptr ? "std-exception" : error.what());
    }
    catch (...) {
      self->recordFailure(
        "capture-probe", "capture-probe-nonstandard-exception",
        "non-standard-exception");
    }
    return GST_PAD_PROBE_DROP;
  }

  GstFlowReturn handleCaptureSample(GstAppSink* sink)
  {
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
      return GST_FLOW_EOS;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      gst_sample_unref(sample);
      return GST_FLOW_ERROR;
    }
    UavVideoFrame frame;
    FrameCallback emit;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (rawTimings.empty() || state != UavVideoPipelineState::Running ||
          !GST_BUFFER_PTS_IS_VALID(buffer)) {
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
      }
      const auto encodedPts = GST_BUFFER_PTS(buffer);
      if (!capturePtsOffset) {
        const auto firstRawPts = rawTimings.begin()->first;
        if (encodedPts < firstRawPts) {
          gst_buffer_unmap(buffer, &map);
          gst_sample_unref(sample);
          return GST_FLOW_OK;
        }
        capturePtsOffset = encodedPts - firstRawPts;
      }
      if (encodedPts < *capturePtsOffset) {
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
      }
      const auto rawPts = encodedPts - *capturePtsOffset;
      const auto timing = rawTimings.find(rawPts);
      if (timing == rawTimings.end()) {
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
      }
      frame.sourceFrameId = timing->second.first;
      frame.captureOriginNs = timing->second.second;
      rawTimings.erase(timing);
      frame.codecPts = static_cast<int64_t>(encodedPts);
      frame.codecConfigEpoch = codecConfigEpoch;
      frame.keyFrame = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
      frame.bytes.assign(map.data, map.data + map.size);
      emit = callback;
    }
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    if (emit) {
      emit(frame);
    }
    return GST_FLOW_OK;
  }

  GstFlowReturn handleDecodeSample(GstAppSink* sink)
  {
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
      return GST_FLOW_EOS;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      gst_sample_unref(sample);
      return GST_FLOW_ERROR;
    }
    FrameCallback emit;
    std::optional<UavVideoFrame> frame;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto found = decodeBindings.find(GST_BUFFER_PTS(buffer));
      if (found != decodeBindings.end() && state == UavVideoPipelineState::Running) {
        frame = found->second;
        frame->bytes.assign(map.data, map.data + map.size);
        decodeBindings.erase(found);
        emit = callback;
      }
    }
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    if (frame && emit) {
      emit(*frame);
    }
    return GST_FLOW_OK;
  }
#endif

  void
  recordFailure(const std::string& direction, const std::string& code,
                const std::string& reason) noexcept
  {
    try {
      std::lock_guard<std::mutex> lock(mutex);
      if (!failure) {
        UavVideoPipelineFailure value;
        value.direction = direction.substr(0, 32);
        value.code = code.substr(0, 96);
        value.reason = reason.substr(0, 256);
        failure = std::move(value);
      }
      callback = {};
      if (state == UavVideoPipelineState::Running) {
        state = UavVideoPipelineState::Failed;
      }
    }
    catch (...) {
      // This function is the final C-ABI exception boundary. Never propagate a
      // secondary allocation or synchronization failure into GStreamer.
    }
  }

  mutable std::mutex mutex;
  Mode mode = Mode::None;
  UavVideoPipelineState state = UavVideoPipelineState::Idle;
  FrameCallback callback;
  std::optional<UavVideoPipelineFailure> failure;
  uint64_t nextSourceFrameId = 1;
  uint64_t codecConfigEpoch = 1;
#if HAVE_GSTREAMER
  GstElement* pipeline = nullptr;
  GstAppSrc* appsrc = nullptr;
  std::map<GstClockTime, std::pair<uint64_t, uint64_t>> rawTimings;
  std::optional<GstClockTime> capturePtsOffset;
  std::map<GstClockTime, UavVideoFrame> decodeBindings;
  std::optional<GstClockTime> lastDecodePts;
#endif
};

GStreamerVideoPipeline::GStreamerVideoPipeline()
  : m_impl(std::make_unique<Impl>())
{
#if HAVE_GSTREAMER
  static std::once_flag once;
  std::call_once(once, [] { gst_init(nullptr, nullptr); });
#endif
}

GStreamerVideoPipeline::~GStreamerVideoPipeline()
{
  stop();
}

UavVideoPipelineCapabilities
GStreamerVideoPipeline::probeCapabilities() const
{
#if HAVE_GSTREAMER
  const auto has = [] (const char* name) {
    auto* factory = gst_element_factory_find(name);
    if (factory == nullptr) return false;
    gst_object_unref(factory);
    return true;
  };
  const bool available = has("appsrc") && has("appsink") && has("x264enc") &&
                         has("h264parse") && has("avdec_h264") && has("jpegenc");
  return {"gstreamer", available, available, available, available,
          has("gtkglsink") || has("glimagesink"),
          available ? "PTS-preserving access-unit pipeline" :
                      "required GStreamer plugin unavailable"};
#else
  return {"gstreamer", false, false, false, false, false,
          "built without GStreamer"};
#endif
}

void
GStreamerVideoPipeline::startCapture(const UavVideoCaptureConfig& config,
                                     FrameCallback callback)
{
#if HAVE_GSTREAMER
  if (!callback || config.width == 0 || config.height == 0 || config.fps == 0 ||
      config.bitrateKbps == 0 || config.keyFrameInterval == 0) {
    throw std::invalid_argument("invalid GStreamer capture configuration");
  }
  stop();
  std::ostringstream description;
  if (config.source == "videotestsrc") {
    description << "videotestsrc is-live=true pattern=ball";
  }
  else {
    description << "filesrc location=\"" << config.source << "\" ! decodebin";
  }
  description << " ! videoconvert ! videoscale ! videorate "
              << "! video/x-raw,format=I420,width=" << config.width
              << ",height=" << config.height << ",framerate=" << config.fps << "/1 "
              << "! identity name=capture_identity "
              << "! x264enc tune=zerolatency speed-preset=ultrafast bframes=0 "
              << "bitrate=" << config.bitrateKbps
              << " key-int-max=" << config.keyFrameInterval
              << " option-string=scenecut=0 byte-stream=true "
              << "! h264parse ! video/x-h264,stream-format=byte-stream,alignment=au "
              << "! appsink name=sink emit-signals=true sync=true max-buffers=4 drop=true";
  GError* error = nullptr;
  auto* pipeline = gst_parse_launch(description.str().c_str(), &error);
  if (pipeline == nullptr) {
    const std::string reason = error == nullptr ? "unknown" : error->message;
    if (error != nullptr) g_error_free(error);
    throw std::runtime_error("GStreamer capture pipeline failed: " + reason);
  }
  auto* identity = gst_bin_get_by_name(GST_BIN(pipeline), "capture_identity");
  auto* sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "sink"));
  auto* pad = gst_element_get_static_pad(identity, "src");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &Impl::onRawFrame,
                    m_impl.get(), nullptr);
  gst_object_unref(pad);
  gst_object_unref(identity);
  gst_app_sink_set_emit_signals(sink, true);
  g_signal_connect(sink, "new-sample", G_CALLBACK(Impl::onCaptureSample), m_impl.get());
  {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->pipeline = pipeline;
    m_impl->mode = Impl::Mode::Capture;
    m_impl->callback = std::move(callback);
    m_impl->failure.reset();
    m_impl->lastDecodePts.reset();
    m_impl->state = UavVideoPipelineState::Running;
    ++m_impl->codecConfigEpoch;
  }
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  gst_object_unref(sink);
#else
  (void)config;
  (void)callback;
  throw std::runtime_error("GStreamer support is unavailable");
#endif
}

void
GStreamerVideoPipeline::startDecode(FrameCallback callback)
{
#if HAVE_GSTREAMER
  if (!callback) throw std::invalid_argument("decode callback is required");
  stop();
  GError* error = nullptr;
  auto* pipeline = gst_parse_launch(
    "appsrc name=source is-live=false format=time block=true "
    "caps=video/x-h264,stream-format=byte-stream,alignment=au "
    "! h264parse ! avdec_h264 max-threads=1 ! videoconvert ! jpegenc "
    "! appsink name=sink emit-signals=true sync=false max-buffers=2 drop=true",
    &error);
  if (pipeline == nullptr) {
    const std::string reason = error == nullptr ? "unknown" : error->message;
    if (error != nullptr) g_error_free(error);
    throw std::runtime_error("GStreamer decode pipeline failed: " + reason);
  }
  auto* source = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "source"));
  auto* sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "sink"));
  gst_app_sink_set_emit_signals(sink, true);
  g_signal_connect(sink, "new-sample", G_CALLBACK(Impl::onDecodeSample), m_impl.get());
  {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->pipeline = pipeline;
    m_impl->appsrc = source;
    m_impl->mode = Impl::Mode::Decode;
    m_impl->callback = std::move(callback);
    m_impl->failure.reset();
    m_impl->state = UavVideoPipelineState::Running;
  }
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  gst_object_unref(sink);
#else
  (void)callback;
  throw std::runtime_error("GStreamer support is unavailable");
#endif
}

bool
GStreamerVideoPipeline::submitAccessUnit(const UavVideoFrame& frame)
{
#if HAVE_GSTREAMER
  GstAppSrc* source = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->state != UavVideoPipelineState::Running ||
        m_impl->mode != Impl::Mode::Decode || m_impl->appsrc == nullptr ||
        frame.bytes.empty()) {
      return false;
    }
    const auto pts = static_cast<GstClockTime>(frame.codecPts);
    if (frame.codecPts < 0 ||
        (m_impl->lastDecodePts && pts <= *m_impl->lastDecodePts)) {
      return false;
    }
    source = GST_APP_SRC(gst_object_ref(m_impl->appsrc));
    m_impl->decodeBindings.emplace(pts, frame);
    m_impl->lastDecodePts = pts;
  }
  auto* buffer = gst_buffer_new_allocate(nullptr, frame.bytes.size(), nullptr);
  gst_buffer_fill(buffer, 0, frame.bytes.data(), frame.bytes.size());
  GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(frame.codecPts);
  GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
  const auto result = gst_app_src_push_buffer(source, buffer);
  gst_object_unref(source);
  if (result != GST_FLOW_OK) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->decodeBindings.erase(static_cast<GstClockTime>(frame.codecPts));
    return false;
  }
  return true;
#else
  (void)frame;
  return false;
#endif
}

void
GStreamerVideoPipeline::stop()
{
#if HAVE_GSTREAMER
  GstElement* pipeline = nullptr;
  GstAppSrc* source = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    pipeline = m_impl->pipeline;
    source = m_impl->appsrc;
    m_impl->pipeline = nullptr;
    m_impl->appsrc = nullptr;
    m_impl->callback = {};
    m_impl->rawTimings.clear();
    m_impl->capturePtsOffset.reset();
    m_impl->decodeBindings.clear();
    m_impl->lastDecodePts.reset();
    m_impl->mode = Impl::Mode::None;
    m_impl->state = UavVideoPipelineState::Stopped;
  }
  if (pipeline != nullptr) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }
  if (source != nullptr) gst_object_unref(source);
#else
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  m_impl->state = UavVideoPipelineState::Stopped;
#endif
}

UavVideoPipelineState
GStreamerVideoPipeline::state() const
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  return m_impl->state;
}

std::optional<UavVideoPipelineFailure>
GStreamerVideoPipeline::failure() const
{
  std::lock_guard<std::mutex> lock(m_impl->mutex);
  return m_impl->failure;
}

} // namespace ndnsf::examples::uav
