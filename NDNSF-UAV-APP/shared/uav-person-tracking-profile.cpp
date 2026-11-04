#include "uav-person-tracking-profile.hpp"

#include "UavDigest.hpp"
#include "ndn-service-framework/strict-json.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace ndnsf::examples::uav {
namespace {

namespace json = ndn_service_framework::json;
using Kind = json::JsonValue::Kind;

struct Limit
{
  const char* name;
  uint64_t minimum;
  uint64_t maximum;
};

constexpr Limit LIMITS[] = {
  {"sampleHz", 2, 2},
  {"maxPersonsPerFrame", 1, 64},
  {"maxLocalKeys", 1, 1024},
  {"maxBackendTracksPerCamera", 1, 512},
  {"maxGlobalIdentities", 1, 256},
  {"samplesPerIdentity", 1, 8},
  {"galleryTtlUs", 1, 30000000},
  {"minTransitionUs", 0, 30000000},
  {"maxTransitionUs", 0, 30000000},
  {"maxClockSkewUs", 0, 100000},
  {"maxCosineDistancePpm", 0, 1000000},
  {"ambiguityMarginPpm", 0, 1000000},
  {"reidBatchSize", 1, 32},
  {"maxFrameBytes", 1, 4 * 1024 * 1024},
  {"maxFrameWidth", 1, 1920},
  {"maxFrameHeight", 1, 1080},
  {"maxSnapshotBytes", 1, 8 * 1024 * 1024},
  {"maxResultBytes", 1, 4 * 1024 * 1024},
  {"maxQueuedWindows", 1, 2},
  {"maxInFlightWindows", 1, 1},
  {"workerTimeoutMs", 1, 30000},
  {"runDeadlineMs", 1, 900000},
  {"maxChildLogBytes", 1, 16 * 1024 * 1024},
  {"maxRetainedTransactionWindows", 1, 120},
};

std::string
requireString(const json::JsonValue::Object& object, const char* key)
{
  const auto& value = json::member(object, key, Kind::String).string;
  if (value.empty() || value.size() > 128 ||
      !std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
        return ch >= 0x20 && ch <= 0x7e;
      })) {
    throw std::invalid_argument(std::string("invalid ASCII profile string: ") + key);
  }
  return value;
}

std::string
requireDigest(const json::JsonValue::Object& object, const char* key)
{
  const auto value = requireString(object, key);
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0 ||
      canonicalSha256Digest(value.substr(7)) != value) {
    throw std::invalid_argument(std::string("invalid canonical profile digest: ") + key);
  }
  return value;
}

} // namespace

UavPersonTrackingProfile
parseUavPersonTrackingProfile(const std::string& wire)
{
  const auto root = json::parseJson(wire, MAX_PERSON_PROFILE_BYTES);
  const auto& object = json::requireObject(root);
  const auto schema = requireString(object, "schema");
  const auto reidPreprocessId = requireString(object, "preprocessId");
  if (schema == "uav-person-tracking-profile-v2") {
    json::requireExactKeys(object, {"schema", "trackingMode", "cameras", "cameraRelations",
      "detectorDigest", "reidDigest", "architecture", "featureDimension", "preprocessId",
      "requestedDevice", "limits", "detectorPreprocessId"});
    const auto detectorMode = requireString(object, "detectorPreprocessId");
    if (detectorMode != "yolo-letterbox1280-raw-box-v1" &&
        detectorMode != "yolo-letterbox1280-raw-box-nms50-v1") {
      throw std::invalid_argument("unsupported person detector preprocessing");
    }
  }
  else {
    json::requireExactKeys(object, {"schema", "trackingMode", "cameras", "cameraRelations",
      "detectorDigest", "reidDigest", "architecture", "featureDimension", "preprocessId",
      "requestedDevice", "limits"});
  }
  if ((schema != "uav-person-tracking-profile-v1" && schema != "uav-person-tracking-profile-v2") ||
      requireString(object, "trackingMode") != "indoor-person-reid" ||
      requireString(object, "architecture") != "osnet_x1_0" ||
      json::member(object, "featureDimension", Kind::Unsigned).number != 512) {
    throw std::invalid_argument("unsupported person tracking model/profile schema");
  }
  if (reidPreprocessId != "osnet-rgb-256x128-imagenet-l2-v1" &&
      reidPreprocessId != "osnet-rgb-256x128-imagenet-gem-p3-l2-v1") {
    throw std::invalid_argument("unsupported person ReID preprocessing");
  }
  UavPersonTrackingProfile result;
  result.detectorDigest = requireDigest(object, "detectorDigest");
  result.reidDigest = requireDigest(object, "reidDigest");
  result.reidPreprocessId = reidPreprocessId;
  result.requestedDevice = requireString(object, "requestedDevice");
  // Only one explicitly selected GPU is in scope; CPU is an explicit development mode.
  if (result.requestedDevice != "cpu" && result.requestedDevice != "cuda:0") {
    throw std::invalid_argument("requestedDevice must be cpu or cuda:0");
  }
  const auto& cameras = json::member(object, "cameras", Kind::Array).array;
  const std::array<std::string_view, 3> expectedCameras = {"UAV1", "UAV2", "UAV3"};
  if (cameras.size() != expectedCameras.size()) {
    throw std::invalid_argument("person profile requires exactly three cameras");
  }
  for (size_t i = 0; i < cameras.size(); ++i) {
    if (cameras[i].kind != Kind::String || cameras[i].string != expectedCameras[i]) {
      throw std::invalid_argument("profile cameras must be UAV1,UAV2,UAV3 in order");
    }
  }
  const auto& relations = json::member(object, "cameraRelations", Kind::Array).array;
  if (relations.size() != 3) {
    throw std::invalid_argument("profile requires exactly three unordered camera pair relations");
  }
  for (const auto& relation : relations) {
    const auto& fields = json::requireObject(relation);
    json::requireExactKeys(fields, {"cameraA", "cameraB", "relation"});
    const auto a = requireString(fields, "cameraA");
    const auto b = requireString(fields, "cameraB");
    const auto kind = requireString(fields, "relation");
    if (a >= b || std::find(expectedCameras.begin(), expectedCameras.end(), a) ==
                    expectedCameras.end() ||
        std::find(expectedCameras.begin(), expectedCameras.end(), b) == expectedCameras.end() ||
        (kind != "overlap" && kind != "non-overlap") ||
        !result.overlappingCameras.emplace(std::make_pair(a, b), kind == "overlap").second) {
      throw std::invalid_argument("invalid or duplicate camera pair relation");
    }
  }
  const auto& policy = json::requireObject(json::member(object, "limits", Kind::Object));
  if (policy.size() != std::size(LIMITS)) {
    throw std::invalid_argument("profile limits must contain exactly the declared policy fields");
  }
  for (const auto& limit : LIMITS) {
    const auto value = json::member(policy, limit.name, Kind::Unsigned).number;
    if (value < limit.minimum || value > limit.maximum) {
      throw std::invalid_argument(std::string("person profile limit out of range: ") + limit.name);
    }
    result.limits.emplace(limit.name, value);
  }
  const auto& limits = result.limits;
  if (limits.at("minTransitionUs") > limits.at("maxTransitionUs") ||
      limits.at("maxTransitionUs") > limits.at("galleryTtlUs") ||
      limits.at("workerTimeoutMs") > limits.at("runDeadlineMs") ||
      limits.at("maxPersonsPerFrame") > limits.at("maxBackendTracksPerCamera") ||
      limits.at("maxPersonsPerFrame") > limits.at("maxLocalKeys")) {
    throw std::invalid_argument("inconsistent person profile time/capacity bounds");
  }
  result.canonicalJson = json::renderJson(root);
  ndn::util::Sha256 hash;
  hash << result.canonicalJson;
  result.profileDigest = canonicalSha256Digest(hash.toString());
  return result;
}

void
requireUavPersonWindowCapacity(const UavPersonTrackingProfile& profile, uint64_t window)
{
  if (window == 0 || window > profile.limits.at("maxRetainedTransactionWindows")) {
    throw std::invalid_argument("PERSON_WINDOW_LIMIT");
  }
}

uint64_t
parseUavPersonWindowIndex(const UavPersonTrackingProfile& profile, const std::string& windowId)
{
  constexpr std::string_view PREFIX = "window-";
  if (windowId.compare(0, PREFIX.size(), PREFIX) != 0 || windowId.size() == PREFIX.size()) {
    throw std::invalid_argument("PERSON_WINDOW_ID");
  }
  uint64_t index = 0;
  const auto end = windowId.data() + windowId.size();
  const auto parsed = std::from_chars(windowId.data() + PREFIX.size(), end, index);
  if (parsed.ec != std::errc() || parsed.ptr != end ||
      windowId != std::string(PREFIX) + std::to_string(index)) {
    throw std::invalid_argument("PERSON_WINDOW_ID");
  }
  if (index == std::numeric_limits<uint64_t>::max()) {
    throw std::invalid_argument("PERSON_WINDOW_LIMIT");
  }
  requireUavPersonWindowCapacity(profile, index + 1);
  return index;
}

} // namespace ndnsf::examples::uav
