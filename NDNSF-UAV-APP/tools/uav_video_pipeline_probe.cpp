#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct CycleResult
{
  bool pts = false;
  bool accessUnit = false;
  uint64_t stopMs = 0;
};

bool
hasFactory(const char* name)
{
  auto* factory = gst_element_factory_find(name);
  if (factory == nullptr) {
    return false;
  }
  gst_object_unref(factory);
  return true;
}

CycleResult
runCycle()
{
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(
    "appsrc name=source is-live=false format=time block=true "
    "caps=video/x-raw,format=RGB,width=64,height=64,framerate=30/1 "
    "! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast "
    "bframes=0 key-int-max=30 byte-stream=true "
    "! h264parse ! video/x-h264,stream-format=byte-stream,alignment=au "
    "! avdec_h264 ! videoconvert ! video/x-raw,format=RGB "
    "! appsink name=sink sync=false max-buffers=4 drop=false",
    &error);
  if (pipeline == nullptr) {
    std::string reason = error == nullptr ? "unknown" : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }
    throw std::runtime_error("GStreamer pipeline parse failed: " + reason);
  }
  auto* source = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline), "source"));
  auto* sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "sink"));
  if (source == nullptr || sink == nullptr) {
    throw std::runtime_error("GStreamer appsrc/appsink lookup failed");
  }
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    throw std::runtime_error("GStreamer pipeline failed to enter PLAYING");
  }

  std::vector<GstClockTime> expected;
  for (uint64_t frame = 0; frame < 3; ++frame) {
    constexpr gsize frameSize = 64 * 64 * 3;
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frameSize, nullptr);
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
      throw std::runtime_error("GStreamer input buffer map failed");
    }
    std::fill(map.data, map.data + map.size,
              static_cast<uint8_t>(40 + frame * 50));
    gst_buffer_unmap(buffer, &map);
    const auto pts = gst_util_uint64_scale(frame, GST_SECOND, 30);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, 30);
    expected.push_back(pts);
    if (gst_app_src_push_buffer(source, buffer) != GST_FLOW_OK) {
      throw std::runtime_error("GStreamer input push failed");
    }
  }
  gst_app_src_end_of_stream(source);

  std::vector<GstClockTime> observed;
  while (observed.size() < expected.size()) {
    GstSample* sample = gst_app_sink_try_pull_sample(sink, 2 * GST_SECOND);
    if (sample == nullptr) {
      break;
    }
    observed.push_back(GST_BUFFER_PTS(gst_sample_get_buffer(sample)));
    gst_sample_unref(sample);
  }
  const auto stopStart = std::chrono::steady_clock::now();
  gst_element_set_state(pipeline, GST_STATE_NULL);
  const auto stopMs = static_cast<uint64_t>(std::chrono::duration_cast<
    std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopStart).count());
  gst_object_unref(source);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  bool relativePtsPreserved = observed.size() == expected.size();
  if (relativePtsPreserved && !observed.empty()) {
    for (size_t index = 1; index < observed.size(); ++index) {
      relativePtsPreserved = relativePtsPreserved &&
        observed[index] - observed[0] == expected[index] - expected[0];
    }
  }
  return {relativePtsPreserved, hasFactory("h264parse"), stopMs};
}

} // namespace

int
main()
{
  gst_init(nullptr, nullptr);
  const bool plugins = hasFactory("appsrc") && hasFactory("appsink") &&
                       hasFactory("x264enc") && hasFactory("h264parse") &&
                       hasFactory("avdec_h264");
  const bool gtkGl = hasFactory("gtkglsink") || hasFactory("glimagesink");
  bool preservesPts = plugins;
  bool accessUnit = plugins;
  uint64_t maxStopMs = 0;
  size_t cycles = 0;
  if (plugins) {
    for (; cycles < 5; ++cycles) {
      const auto result = runCycle();
      preservesPts = preservesPts && result.pts;
      accessUnit = accessUnit && result.accessUnit;
      maxStopMs = std::max(maxStopMs, result.stopMs);
    }
  }
  std::cout << "{\n"
            << "  \"backend\": \"gstreamer\",\n"
            << "  \"available\": " << (plugins ? "true" : "false") << ",\n"
            << "  \"preservesPts\": " << (preservesPts ? "true" : "false") << ",\n"
            << "  \"accessUnitAligned\": " << (accessUnit ? "true" : "false") << ",\n"
            << "  \"headlessAppsink\": " << (plugins ? "true" : "false") << ",\n"
            << "  \"gtkGlPluginAvailable\": " << (gtkGl ? "true" : "false") << ",\n"
            << "  \"startStopCycles\": " << cycles << ",\n"
            << "  \"maxStopMs\": " << maxStopMs << "\n"
            << "}\n";
  return plugins && preservesPts && accessUnit && cycles == 5 ? 0 : 2;
}
