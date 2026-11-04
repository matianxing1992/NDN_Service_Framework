#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavDigest.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-json.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-tracking.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace ndnsf::test {
namespace {

using namespace examples::uav;
namespace pj = examples::uav::person_json;
namespace fs = std::filesystem;
using Json = pj::Json;

constexpr size_t SEQUENCE_WINDOWS = 120;
constexpr size_t MAX_MANIFEST_BYTES = 256 * 1024;
constexpr size_t MAX_OUTPUT_BYTES = 8 * 1024 * 1024;

std::string
sequenceDigest(const std::string& wire)
{
  ndn::util::Sha256 hash;
  hash << wire;
  return canonicalSha256Digest(hash.toString());
}

std::string
readSequenceFile(const fs::path& path, size_t maximum)
{
  if (!fs::is_regular_file(path)) throw std::invalid_argument("sequence file is not regular");
  const auto size = fs::file_size(path);
  if (size == 0 || size > maximum) throw std::invalid_argument("sequence file byte bound");
  std::ifstream input(path, std::ios::binary);
  std::string wire(size, '\0');
  input.read(wire.data(), static_cast<std::streamsize>(size));
  if (input.gcount() != static_cast<std::streamsize>(size) ||
      input.peek() != std::char_traits<char>::eof() || input.bad()) {
    throw std::invalid_argument("sequence file length changed or read failed");
  }
  return wire;
}

UavPersonFrameContext
sequenceFrame(const Json& wire, size_t camera, uint64_t sequence)
{
  pj::exactKeys(wire, {"cameraId", "sourceId", "inputDigest", "sourceEpoch",
    "trackerGeneration", "sequence", "commonPtsUs", "width", "height", "clockAvailable",
    "clockUncertaintyUs"});
  UavPersonFrameContext frame;
  frame.cameraId = pj::text(wire.at("cameraId"));
  frame.sourceId = pj::text(wire.at("sourceId"));
  frame.inputDigest = pj::text(wire.at("inputDigest"));
  pj::checkIdentifier(frame.sourceId);
  frame.sourceEpoch = pj::unsignedNumber(wire.at("sourceEpoch"));
  frame.trackerGeneration = pj::unsignedNumber(wire.at("trackerGeneration"));
  frame.sequence = pj::unsignedNumber(wire.at("sequence"));
  frame.commonPtsUs = static_cast<int64_t>(pj::unsignedNumber(wire.at("commonPtsUs"),
    std::numeric_limits<int64_t>::max()));
  frame.width = static_cast<int>(pj::unsignedNumber(wire.at("width"), 1920));
  frame.height = static_cast<int>(pj::unsignedNumber(wire.at("height"), 1080));
  frame.clockAvailable = pj::boolean(wire.at("clockAvailable"));
  frame.clockUncertaintyUs = pj::unsignedNumber(wire.at("clockUncertaintyUs"));
  if (frame.cameraId != "UAV" + std::to_string(camera + 1) || frame.sequence != sequence ||
      frame.sourceEpoch == 0 || frame.trackerGeneration == 0 || frame.width == 0 ||
      frame.height == 0 || !pj::isDigest(frame.inputDigest)) {
    throw std::invalid_argument("sequence frame identity or dimensions");
  }
  return frame;
}

void
checkSequenceManifest(const Json& manifest)
{
  pj::exactKeys(manifest, {"schema", "variant", "profileDigest", "profileSha256",
    "computationDigest", "windows"});
  const auto variant = pj::text(manifest.at("variant"));
  if (pj::text(manifest.at("schema")) != "uav-person-sequence-fixture-v1" ||
      (variant != "n" && variant != "s")) {
    throw std::invalid_argument("sequence fixture schema or variant");
  }
  for (const auto* key : {"profileDigest", "profileSha256", "computationDigest"}) {
    if (!pj::isDigest(pj::text(manifest.at(key)))) {
      throw std::invalid_argument("sequence fixture digest");
    }
  }
  const auto& windows = manifest.at("windows");
  if (!windows.is_array() || windows.size() != SEQUENCE_WINDOWS) {
    throw std::invalid_argument("sequence requires exactly120 windows");
  }
  for (size_t i = 0; i < windows.size(); ++i) {
    const auto& window = windows.at(i);
    pj::exactKeys(window, {"windowId", "backendSha256", "frames"});
    if (pj::text(window.at("windowId")) != "window-" + std::to_string(i) ||
        !pj::isDigest(pj::text(window.at("backendSha256")))) {
      throw std::invalid_argument("sequence window identity");
    }
    const auto& frames = window.at("frames");
    if (!frames.is_array() || frames.size() != 3) {
      throw std::invalid_argument("sequence requires three ordered frames");
    }
    for (size_t camera = 0; camera < frames.size(); ++camera) {
      sequenceFrame(frames.at(camera), camera, i + 1);
    }
  }
}

Json
sequenceDecisions(const UavPersonWindowResult& result)
{
  Json frames = Json::array();
  for (size_t camera = 0; camera < result.tracks.size(); ++camera) {
    Json rows = Json::array();
    for (const auto& track : result.tracks[camera]) {
      const auto& key = track.key;
      Json localKey = {{"cameraId", key.cameraId}, {"sourceId", key.sourceId},
        {"sourceEpoch", key.sourceEpoch}, {"trackerGeneration", key.trackerGeneration},
        {"byteTrackId", key.byteTrackId ? Json(key.byteTrackId) : Json(nullptr)}};
      Json origin = nullptr;
      if (track.associationOrigin) {
        const auto& witness = *track.associationOrigin;
        const auto& key = witness.key;
        origin = {{"localKey", {{"cameraId", key.cameraId}, {"sourceId", key.sourceId},
                    {"sourceEpoch", key.sourceEpoch},
                    {"trackerGeneration", key.trackerGeneration},
                    {"byteTrackId", key.byteTrackId}}},
                  {"ptsUs", witness.ptsUs}, {"active", witness.active}};
      }
      rows.push_back({{"byteTrackId", key.byteTrackId ? Json(key.byteTrackId) : Json(nullptr)},
        {"box", track.box},
        {"confidence", track.confidence}, {"associationStatus", track.associationStatus},
        {"globalId", track.globalId ? Json(*track.globalId) : Json(nullptr)},
        {"distance", track.distance ? Json(*track.distance) : Json(nullptr)},
        {"ptsUs", track.commonPtsUs}, {"localKey", std::move(localKey)},
        {"associationOrigin", std::move(origin)}});
    }
    frames.push_back({{"cameraId", result.context.frames[camera].cameraId},
      {"tracks", std::move(rows)}});
  }
  return {{"schema", "uav-person-sequence-decisions-v1"},
    {"windowId", result.context.windowId}, {"sequence", result.context.sequence},
    {"frames", std::move(frames)}, {"gallerySize", result.gallerySize},
    {"localKeyCount", result.localKeyCount}};
}

void
writeSequenceArtifact(const fs::path& path, const std::string& wire)
{
  if (wire.size() > MAX_OUTPUT_BYTES || fs::exists(path)) {
    throw std::invalid_argument("sequence artifact size or overwrite");
  }
  std::ofstream output(path, std::ios::binary);
  output.write(wire.data(), static_cast<std::streamsize>(wire.size()));
  output.close();
  if (!output) throw std::runtime_error("sequence artifact write failed");
}

Json
replaySequence(const std::string& expectedVariant)
{
  const auto* directory = std::getenv("NDNSF_PERSON_SEQUENCE_INPUT");
  const auto* destination = std::getenv("NDNSF_PERSON_SEQUENCE_OUTPUT");
  const auto* expectedDigest = std::getenv("NDNSF_PERSON_SEQUENCE_MANIFEST_DIGEST");
  if (!directory || !destination || !expectedDigest) {
    throw std::invalid_argument("explicit sequence input/output/manifest digest required");
  }
  const fs::path input(directory), output(destination);
  if (!input.is_absolute() || !output.is_absolute() || !fs::is_directory(input)) {
    throw std::invalid_argument("absolute sequence directories required");
  }
  const auto manifestWire = readSequenceFile(input / "manifest.json", MAX_MANIFEST_BYTES);
  if (sequenceDigest(manifestWire) != expectedDigest) {
    throw std::invalid_argument("sequence manifest digest mismatch");
  }
  const auto manifest = pj::parseBounded(manifestWire, MAX_MANIFEST_BYTES);
  checkSequenceManifest(manifest);
  BOOST_REQUIRE_EQUAL(pj::text(manifest.at("variant")), expectedVariant);
  const auto profileWire = readSequenceFile(input / "profile.json", MAX_PERSON_PROFILE_BYTES);
  BOOST_REQUIRE_EQUAL(sequenceDigest(profileWire), pj::text(manifest.at("profileSha256")));
  const auto profile = parseUavPersonTrackingProfile(profileWire);
  BOOST_REQUIRE_EQUAL(profile.profileDigest, pj::text(manifest.at("profileDigest")));
  BOOST_REQUIRE_EQUAL(profile.limits.at("maxGlobalIdentities"), 256);
  if (!fs::create_directory(output)) throw std::invalid_argument("fresh sequence output required");
  fs::permissions(output, fs::perms::owner_all, fs::perm_options::replace);
  std::ofstream decisions(output / "decisions.jsonl", std::ios::binary);
  if (!decisions) throw std::runtime_error("cannot open sequence decisions");
  UavPersonTrackingEngine engine(profile);
  std::string predecessor, firstFailure;
  size_t committed = 0, totalBytes = 0, lastGallery = 0;
  for (size_t i = 0; i < SEQUENCE_WINDOWS; ++i) {
    const auto& entry = manifest.at("windows").at(i);
    UavPersonWindowContext context;
    context.runId = "component-" + expectedVariant;
    context.missionId = context.runId;
    context.windowId = "window-" + std::to_string(i);
    context.sequence = i + 1;
    context.computationDigest = pj::text(manifest.at("computationDigest"));
    for (size_t camera = 0; camera < context.frames.size(); ++camera) {
      context.frames[camera] = sequenceFrame(entry.at("frames").at(camera), camera, i + 1);
    }
    const auto wire = readSequenceFile(input / (context.windowId + ".json"),
                                      profile.limits.at("maxResultBytes"));
    BOOST_REQUIRE_EQUAL(sequenceDigest(wire), pj::text(entry.at("backendSha256")));
    const auto backend = parseUavPersonBackendWindow(wire, context, profile);
    UavPersonTrackingEngine resumed(profile);
    if (!predecessor.empty()) resumed.restoreState(predecessor, context);
    std::optional<UavPersonWindowResult> result;
    try {
      result = engine.processWindow(context, backend);
    }
    catch (const std::invalid_argument& error) {
      firstFailure = error.what();
      if (predecessor.empty()) {
        BOOST_CHECK_THROW(engine.serializeState(), std::invalid_argument);
      }
      else {
        BOOST_CHECK_EQUAL(engine.serializeState(), predecessor);
      }
      bool repeated = false;
      try {
        resumed.processWindow(context, backend);
      }
      catch (const std::invalid_argument& again) {
        repeated = true;
        BOOST_CHECK_EQUAL(again.what(), firstFailure);
      }
      BOOST_CHECK(repeated);
      if (predecessor.empty()) {
        BOOST_CHECK_THROW(resumed.serializeState(), std::invalid_argument);
      }
      else {
        BOOST_CHECK_EQUAL(resumed.serializeState(), predecessor);
      }
      break;
    }
    const auto restored = resumed.processWindow(context, backend);
    BOOST_CHECK_EQUAL(result->candidateStateJson, restored.candidateStateJson);
    BOOST_CHECK_EQUAL(sequenceDecisions(*result).dump(), sequenceDecisions(restored).dump());
    size_t backendRows = 0, resultRows = 0;
    for (size_t camera = 0; camera < context.frames.size(); ++camera) {
      backendRows += backend.frames[camera].detections.size();
      resultRows += result->tracks[camera].size();
    }
    BOOST_CHECK_EQUAL(backendRows, resultRows);
    BOOST_CHECK_LE(result->gallerySize, profile.limits.at("maxGlobalIdentities"));
    BOOST_CHECK_LE(result->localKeyCount, profile.limits.at("maxLocalKeys"));
    predecessor = result->candidateStateJson;
    lastGallery = result->gallerySize;
    ++committed;
    if (i == 0 || i == SEQUENCE_WINDOWS - 1) {
      BOOST_CHECK_THROW(engine.processWindow(context, backend), std::invalid_argument);
      BOOST_CHECK_EQUAL(engine.serializeState(), predecessor);
    }
    const auto line = sequenceDecisions(*result).dump() + '\n';
    totalBytes += line.size();
    if (totalBytes > MAX_OUTPUT_BYTES) throw std::runtime_error("sequence output byte bound");
    decisions.write(line.data(), static_cast<std::streamsize>(line.size()));
    decisions.flush();
    if (!decisions) throw std::runtime_error("sequence decision write failed");
  }
  decisions.close();
  if (!decisions) throw std::runtime_error("sequence decisions close failed");
  if (!predecessor.empty()) writeSequenceArtifact(output / "last-state.json", predecessor);
  Json summary = {{"schema", "uav-person-sequence-summary-v1"}, {"variant", expectedVariant},
    {"scope", "native parser/engine/snapshot component only; no Named Data or GS qualification"},
    {"committedWindows", committed}, {"plannedWindows", SEQUENCE_WINDOWS},
    {"firstFailureWindow", firstFailure.empty() ? Json(nullptr) : Json(committed)},
    {"failure", firstFailure}, {"lastGallerySize", lastGallery},
    {"manifestDigest", sequenceDigest(manifestWire)}, {"profileDigest", profile.profileDigest}};
  writeSequenceArtifact(output / "summary.json", summary.dump() + '\n');
  return summary;
}

Json
literalSequenceManifest()
{
  const auto digest = "sha256:" + std::string(64, 'a');
  Json windows = Json::array();
  for (size_t i = 0; i < SEQUENCE_WINDOWS; ++i) {
    Json frames = Json::array();
    for (size_t camera = 0; camera < 3; ++camera) {
      frames.push_back({{"cameraId", "UAV" + std::to_string(camera + 1)},
        {"sourceId", "source"}, {"inputDigest", digest}, {"sourceEpoch", 1},
        {"trackerGeneration", 1}, {"sequence", i + 1}, {"commonPtsUs", i * 500000},
        {"width", 1920}, {"height", 1080}, {"clockAvailable", true},
        {"clockUncertaintyUs", 1000}});
    }
    windows.push_back({{"windowId", "window-" + std::to_string(i)},
      {"backendSha256", digest}, {"frames", std::move(frames)}});
  }
  const Json manifest = {{"schema", "uav-person-sequence-fixture-v1"}, {"variant", "n"},
    {"profileDigest", digest}, {"profileSha256", digest}, {"computationDigest", digest},
    {"windows", std::move(windows)}};
  return Json::parse(manifest.dump());
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavPersonSequence)

BOOST_AUTO_TEST_CASE(BoundedInputAndContextContract)
{
  const auto manifest = literalSequenceManifest();
  BOOST_CHECK_NO_THROW(checkSequenceManifest(manifest));
  for (const auto& wire : {std::string(), std::string(MAX_MANIFEST_BYTES + 1, ' '),
                           std::string("{\"schema\":1,\"schema\":2}")}) {
    BOOST_CHECK_THROW(pj::parseBounded(wire, MAX_MANIFEST_BYTES), std::invalid_argument);
  }
  for (size_t mutation = 0; mutation < 9; ++mutation) {
    auto invalid = manifest;
    switch (mutation) {
      case 0: invalid["windows"].erase(invalid["windows"].end() - 1); break;
      case 1: invalid["windows"].push_back(invalid["windows"][0]); break;
      case 2: invalid["windows"][0]["frames"][1]["cameraId"] = "UAV1"; break;
      case 3: invalid["windows"][0]["frames"][0]["sequence"] = true; break;
      case 4: invalid["windows"][0]["frames"][0]["width"] = uint64_t{0}; break;
      case 5: invalid["windows"][0]["frames"][0]["clockAvailable"] = "true"; break;
      case 6: invalid["windows"][0]["frames"][0]["commonPtsUs"] = 0.0; break;
      case 7: invalid["windows"][0]["frames"][0]["sourceId"] = ""; break;
      case 8: invalid["windows"][0]["backendSha256"] = "not-a-digest"; break;
    }
    BOOST_CHECK_THROW(checkSequenceManifest(invalid), std::invalid_argument);
  }
  BOOST_CHECK_THROW(readSequenceFile(fs::path{}, 1), std::invalid_argument);
  BOOST_CHECK_THROW(readSequenceFile(fs::temp_directory_path(), 1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CapturedBaselineCapacityAndRollback, *boost::unit_test::disabled())
{
  const auto summary = replaySequence("n");
  BOOST_CHECK_EQUAL(summary.at("committedWindows").get<size_t>(), 112);
  BOOST_CHECK_EQUAL(summary.at("firstFailureWindow").get<size_t>(), 112);
  BOOST_CHECK_EQUAL(summary.at("lastGallerySize").get<size_t>(), 256);
  BOOST_CHECK_EQUAL(summary.at("failure").get<std::string>(),
                    "person-backend-invalid: GALLERY_LIMIT");
}

BOOST_AUTO_TEST_CASE(CandidateSequenceCompletesAllWindows, *boost::unit_test::disabled())
{
  const auto summary = replaySequence("s");
  BOOST_CHECK_EQUAL(summary.at("committedWindows").get<size_t>(), SEQUENCE_WINDOWS);
  BOOST_CHECK(summary.at("firstFailureWindow").is_null());
  BOOST_CHECK(summary.at("failure").get<std::string>().empty());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
