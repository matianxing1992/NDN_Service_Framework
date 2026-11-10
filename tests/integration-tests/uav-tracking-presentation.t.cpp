#include "NDNSF-UAV-APP/ground-station/uav-tracking-panel.hpp"
#include "tests/fixtures/uav-tracking-core-flow.hpp"

#include <gdkmm/wrap_init.h>

namespace ndnsf::test {
using namespace tracking_flow;

struct PresentationFixture
{
  PresentationFixture()
  {
    definition = tracking_fixture::validDefinition();
    bundle.runId = definition.runId;
    bundle.generation = 7;
    bundle.windowIndex = 0;
    Json frames = Json::array();
    const auto jpeg = std::make_shared<const ndn::Buffer>(tracking_fixture::trackingFixtureJpeg());
    const auto hash = digestOf(std::string(jpeg->begin(), jpeg->end()));
    for (size_t i = 0; i < 3; ++i) {
      const auto& source = definition.sources[i];
      const auto name = makeUavObservationDataName(source.producerIdentity, source.cameraId,
        source.sourceEpoch, 1, "FRAME", 1).toUri();
      bundle.frames[i] = {source.cameraId, source.sourceId, source.producerIdentity,
                          source.sourceEpoch, 1, name, hash, 640, 480, jpeg};
      frames.push_back({{"cameraId", source.cameraId}, {"sourceId", source.sourceId},
        {"sourceEpoch", source.sourceEpoch}, {"sequence", 1}, {"inputDigest", hash},
        {"imageWidth", 640}, {"imageHeight", 480}, {"imageRef", {{"name", name}, {"digest", hash}}},
        {"tracks", Json::array({{{"globalId", 42}, {"box", {10, 20, 100, 200}}}})}});
    }
    result = {{"schema", "spec210-tracking-result-v2"}, {"terminal", true},
      {"runId", definition.runId}, {"missionId", "tracking-" + definition.runId},
      {"windowId", "window-0"}, {"frames", frames}};
    update();
  }

  void update()
  {
    bundle.resultJson = std::make_shared<const std::string>(result.dump());
    bundle.resultDigest = digestOf(*bundle.resultJson);
  }

  void project()
  {
    projectTrackingPresentation(bundle, definition, 7, bundle.resultDigest);
  }

  TrackingJobDefinition definition;
  TrackingPresentation bundle;
  Json result;
};

BOOST_AUTO_TEST_SUITE(UavTrackingPresentation)

BOOST_FIXTURE_TEST_CASE(ProjectsOriginalThreeViewIdsAndRejectsStaleOrMismatchedBindings,
                        PresentationFixture)
{
  const auto output = projectTrackingPresentation(bundle, definition, 7, bundle.resultDigest);
  for (const auto& camera : output) {
    BOOST_REQUIRE_EQUAL(camera.boxes.size(), 1);
    BOOST_CHECK_EQUAL(camera.boxes[0].globalId, 42);
    BOOST_CHECK_CLOSE(camera.boxes[0].box[0], 10.0 / 640, 0.0001);
    BOOST_CHECK_EQUAL(camera.boxes[0].label, "ID 42 | motion UNKNOWN");
  }
  BOOST_CHECK_THROW(projectTrackingPresentation(bundle, definition, 8, bundle.resultDigest),
                    std::invalid_argument);
  auto original = bundle.frames[1];
  ++bundle.frames[1].sourceEpoch;
  BOOST_CHECK_THROW(project(), std::invalid_argument);
  bundle.frames[1] = original;
  bundle.frames[1].producerIdentity = ndn::Name("/wrong/producer");
  BOOST_CHECK_THROW(project(), std::invalid_argument);
  bundle.frames[1] = original;
  bundle.frames[1].jpeg = std::make_shared<const ndn::Buffer>(ndn::Buffer{1, 2, 3});
  BOOST_CHECK_THROW(project(), std::invalid_argument);
  bundle.frames[1] = original;
  result["frames"][1]["tracks"][0]["box"][2] = 641;
  update();
  BOOST_CHECK_THROW(project(), std::invalid_argument);
  result["frames"][1]["tracks"][0]["box"][2] = 100;
  result["terminal"] = false;
  update();
  BOOST_CHECK_THROW(project(), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(ActualJpegDecoderRejectsDeclaredAndEncodedDimensionMismatch,
                        PresentationFixture)
{
  Glib::init();
  Gdk::wrap_init();
  const auto decoded = decodeTrackingJpeg(bundle.frames[0]);
  BOOST_REQUIRE(decoded);
  BOOST_CHECK_EQUAL(decoded->get_width(), 400);
  BOOST_CHECK_EQUAL(decoded->get_height(), 300);
  bundle.frames[0].width = 639;
  BOOST_CHECK_THROW(decodeTrackingJpeg(bundle.frames[0]), std::invalid_argument);
  bundle.frames[0].width = 3841;
  BOOST_CHECK_THROW(decodeTrackingJpeg(bundle.frames[0]), std::invalid_argument);
  bundle.frames[0].width = 640;
  bundle.frames[0].jpeg = std::make_shared<const ndn::Buffer>(ndn::Buffer{1, 2, 3});
  bool rejected = false;
  try { decodeTrackingJpeg(bundle.frames[0]); }
  catch (const Glib::Error&) { rejected = true; }
  catch (const std::exception&) { rejected = true; }
  BOOST_CHECK(rejected);
}

BOOST_FIXTURE_TEST_CASE(PersonUsesNestedOriginalReferenceAndKeepsGeometryUnknown,
                        PresentationFixture)
{
  definition.mode = TrackingMode::PERSON;
  bundle.mode = TrackingMode::PERSON;
  result["schema"] = "uav-person-tracking-result-v1";
  for (auto& frame : result["frames"]) {
    frame["observation"] = {{"imageRef", frame["imageRef"]}};
    frame.erase("imageRef");
    frame["width"] = frame["imageWidth"];
    frame["height"] = frame["imageHeight"];
    frame.erase("imageWidth");
    frame.erase("imageHeight");
  }
  update();
  const auto cameras = projectTrackingPresentation(bundle, definition, 7, bundle.resultDigest);
  BOOST_CHECK_EQUAL(cameras[0].boxes[0].label, "ID 42 | geometry UNKNOWN");
  result["frames"][0]["observation"]["imageRef"]["name"] = "/wrong/producer/FRAME";
  update();
  BOOST_CHECK_THROW(project(), std::invalid_argument);
}

BOOST_FIXTURE_TEST_CASE(ActualOriginal4kVehicleJpegDecodesToBoundedPreview, PresentationFixture)
{
  Glib::init();
  Gdk::wrap_init();
  auto original = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, false, 8, 3840, 2160);
  original->fill(0x404040ff);
  gchar* data = nullptr;
  gsize size = 0;
  original->save_to_buffer(data, size, "jpeg");
  const std::unique_ptr<gchar, decltype(&g_free)> owned(data, &g_free);
  auto& frame = bundle.frames[0];
  frame.width = 3840;
  frame.height = 2160;
  frame.jpeg = std::make_shared<const ndn::Buffer>(data, data + size);
  const auto decoded = decodeTrackingJpeg(frame);
  BOOST_REQUIRE(decoded);
  BOOST_CHECK_EQUAL(decoded->get_width(), 480);
  BOOST_CHECK_EQUAL(decoded->get_height(), 270);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndnsf::test
