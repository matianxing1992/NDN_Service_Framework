#include "UavTrackingResultVerifier.hpp"

#include "UavNames.hpp"
#include "UavProtocol.hpp"
#include "uav-person-json.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {
namespace {

using boost::property_tree::ptree;
using person_json::Json;
using person_json::boolean;
using person_json::exactKeys;
using person_json::finiteNumber;
using person_json::isDigest;
using person_json::parseBounded;
using person_json::reject;
using person_json::text;
using person_json::unsignedNumber;

constexpr size_t MAX_JSON_NESTING_DEPTH = 64;
constexpr std::array<const char*, 8> PERSON_ASSOCIATION_STATUSES = {
  "UNTRACKED", "CONTINUED", "NO_CANDIDATE", "NO_FEATURE", "CLOCK_UNAVAILABLE", "AMBIGUOUS",
  "COMPETITION", "MATCHED"};

ndn::Name
sourceProducer(const UavTrackingResultExpectation& expected, size_t index)
{
  if (std::all_of(expected.sourceProviders.begin(), expected.sourceProviders.end(),
                  [] (const ndn::Name& identity) { return identity.empty(); })) {
    return droneIdentity("UAV" + std::to_string(index + 1));
  }
  std::set<ndn::Name> identities{expected.computeProvider};
  for (const auto& identity : expected.sourceProviders) {
    if (identity.empty() || !identities.insert(identity).second) {
      throw std::invalid_argument("tracking result source Producer mapping is incomplete or duplicate");
    }
  }
  return expected.sourceProviders.at(index);
}

void
requireJsonNestingBounded(const std::string& json)
{
  size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  for (const char c : json) {
    if (inString) {
      if (escaped) {
        escaped = false;
      }
      else if (c == '\\') {
        escaped = true;
      }
      else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    }
    else if (c == '{' || c == '[') {
      if (++depth > MAX_JSON_NESTING_DEPTH) {
        throw std::runtime_error("tracking result JSON nesting exceeds 64 levels");
      }
    }
    else if (c == '}' || c == ']') {
      if (depth == 0) {
        throw std::runtime_error("tracking result JSON has unbalanced delimiters");
      }
      --depth;
    }
  }
  if (inString || escaped || depth != 0) {
    throw std::runtime_error("tracking result JSON has unbalanced strings or delimiters");
  }
}

void
requireUniqueKeys(const ptree& tree, size_t depth = 0)
{
  if (depth > MAX_JSON_NESTING_DEPTH) {
    throw std::runtime_error("tracking result JSON nesting exceeds 64 levels");
  }
  std::set<std::string> keys;
  for (const auto& child : tree) {
    if (!child.first.empty() && !keys.insert(child.first).second) {
      throw std::runtime_error("tracking result JSON contains a duplicate object key");
    }
    requireUniqueKeys(child.second, depth + 1);
  }
}

const ptree&
requiredChild(const ptree& tree, const std::string& key)
{
  const ptree* found = nullptr;
  for (const auto& child : tree) {
    if (child.first != key) continue;
    if (found != nullptr) {
      throw std::runtime_error("tracking result has a duplicate " + key + " field");
    }
    found = &child.second;
  }
  if (found == nullptr) {
    throw std::runtime_error("tracking result is missing " + key);
  }
  return *found;
}

std::string
requiredScalar(const ptree& tree, const std::string& key)
{
  const auto& child = requiredChild(tree, key);
  if (!child.empty()) {
    throw std::runtime_error("tracking result field is not scalar: " + key);
  }
  return child.data();
}

uint64_t
requiredPositiveInteger(const ptree& tree, const std::string& key)
{
  const auto value = requiredScalar(tree, key);
  uint64_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size() || parsed == 0 ||
      std::to_string(parsed) != value) {
    throw std::runtime_error("tracking result field is not a positive canonical integer: " + key);
  }
  return parsed;
}

void
requireDigest(const std::string& digest, const std::string& label)
{
  if (digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0 ||
      digest.find_first_not_of("0123456789abcdef", 7) != std::string::npos) {
    throw std::runtime_error("tracking result has a malformed " + label + " digest");
  }
}

ndn::Name
requireCanonicalName(const std::string& value, const std::string& label)
{
  ndn::Name name(value);
  if (name.empty() || name.toUri() != value) {
    throw std::runtime_error("tracking result has a noncanonical " + label + " name");
  }
  return name;
}

Json
personLocalKey(const Json& track)
{
  return {{"cameraId", track.at("cameraId")}, {"sourceId", track.at("sourceId")},
    {"sourceEpoch", track.at("sourceEpoch")}, {"trackerGeneration", track.at("trackerGeneration")},
    {"byteTrackId", track.at("byteTrackId")}};
}

Json
personEvent(const Json& track)
{
  return {{"cameraId", track.at("cameraId")}, {"byteTrackId", track.at("byteTrackId")},
    {"globalId", track.at("globalId")}, {"status", track.at("associationStatus")},
    {"distance", track.at("appearanceDistance")}, {"targetLocalKey", personLocalKey(track)},
    {"ptsUs", track.at("ptsUs")}, {"origin", track.at("associationOrigin")}};
}

void
validatePersonOrigin(const Json& track, const Json& frame, const Json& frames,
                     const UavPersonTrackingProfile& profile,
                     const UavTrackingResultExpectation& expected)
{
  const auto& origin = track.at("associationOrigin");
  if (text(track.at("associationStatus")) != "MATCHED") {
    if (!origin.is_null()) reject("person result origin without a match");
    return;
  }
  exactKeys(origin, {"localKey", "ptsUs", "active"});
  const auto& key = origin.at("localKey");
  exactKeys(key, {"cameraId", "sourceId", "sourceEpoch", "trackerGeneration", "byteTrackId"});
  const auto camera = text(key.at("cameraId"));
  if (camera != "UAV1" && camera != "UAV2" && camera != "UAV3") {
    reject("person result origin camera");
  }
  const auto index = static_cast<size_t>(camera.back() - '1');
  const auto& source = frames.at(index);
  if (text(key.at("sourceId")) != expected.sourceIds[index] ||
      unsignedNumber(key.at("sourceEpoch")) != expected.sourceEpoch ||
      unsignedNumber(key.at("trackerGeneration")) != unsignedNumber(source.at("trackerGeneration")) ||
      unsignedNumber(key.at("byteTrackId")) == 0 || key.dump() == personLocalKey(track).dump()) {
    reject("person result origin local/source binding");
  }
  const auto pts = unsignedNumber(origin.at("ptsUs"), std::numeric_limits<int64_t>::max());
  const auto targetPts = unsignedNumber(track.at("ptsUs"), std::numeric_limits<int64_t>::max());
  if (pts > targetPts) reject("person result future origin");
  const auto gap = targetPts - pts;
  if (gap > profile.limits.at("galleryTtlUs")) reject("person result expired origin");
  const bool active = boolean(origin.at("active"));
  const auto targetCamera = text(track.at("cameraId"));
  if (camera == targetCamera) {
    if (active || gap > profile.limits.at("maxTransitionUs")) {
      reject("person result same-camera origin eligibility");
    }
    return;
  }
  const auto maxSkew = profile.limits.at("maxClockSkewUs");
  const auto targetUncertainty = unsignedNumber(frame.at("clockUncertaintyUs"));
  if (!boolean(frame.at("clockAvailable")) || !boolean(source.at("clockAvailable")) ||
      targetUncertainty > maxSkew ||
      unsignedNumber(source.at("clockUncertaintyUs")) > maxSkew - targetUncertainty) {
    reject("person result origin clock eligibility");
  }
  const auto pair = std::minmax(camera, targetCamera);
  const bool overlap = profile.overlappingCameras.at({pair.first, pair.second});
  if (active ? (!overlap || gap > maxSkew) :
      (gap > profile.limits.at("maxTransitionUs") ||
       (!overlap && gap < profile.limits.at("minTransitionUs")))) {
    reject("person result cross-camera origin eligibility");
  }
  // Only the native owner can establish historical existence/activity of this signed witness.
}

void
validatePersonReference(const Json& reference, const ndn::Name& name, const std::string& digest,
                         const ndn::Name& producer)
{
  exactKeys(reference, {"name", "digest", "producerIdentity"});
  if (text(reference.at("name")) != name.toUri() ||
      text(reference.at("digest")) != digest || !isDigest(digest) ||
      text(reference.at("producerIdentity")) != producer.toUri()) {
    reject("person result original reference binding");
  }
}

void
validatePersonResult(const std::string& resultJson, const UavTrackingResultExpectation& expected)
{
  const auto& person = *expected.personTracking;
  const auto profile = parseUavPersonTrackingProfile(person.profile.canonicalJson);
  if (!isDigest(person.computationDigest)) reject("person result expected computation digest");
  const auto wire = parseBounded(resultJson, profile.limits.at("maxResultBytes"));
  exactKeys(wire, {"schema", "terminal", "runId", "missionId", "windowId", "sequence",
    "trackingMode", "profileDigest", "computationDigest", "detectorDigest", "reidDigest",
    "preprocessId", "requestedDevice", "actualDetectorDevice", "detectorModelDevice",
    "detectorInputDevice", "actualReidDevice", "reidModelDevice", "reidInputDevice",
    "reidInferenceBatches", "frames", "associationEvents", "limits", "counters", "timing"});
  if (text(wire.at("schema")) != "uav-person-tracking-result-v1" ||
      !boolean(wire.at("terminal")) || text(wire.at("runId")) != expected.runId ||
      text(wire.at("missionId")) != expected.missionId ||
      text(wire.at("windowId")) != expected.windowId ||
      unsignedNumber(wire.at("sequence")) != expected.sequence ||
      text(wire.at("trackingMode")) != "indoor-person-reid" ||
      text(wire.at("profileDigest")) != profile.profileDigest ||
      text(wire.at("computationDigest")) != person.computationDigest ||
      text(wire.at("detectorDigest")) != profile.detectorDigest ||
      text(wire.at("reidDigest")) != profile.reidDigest ||
      text(wire.at("preprocessId")) != profile.reidPreprocessId) {
    reject("person result computation/context binding");
  }
  for (const auto* key : {"requestedDevice", "actualDetectorDevice", "detectorModelDevice",
                          "detectorInputDevice", "reidModelDevice"}) {
    if (text(wire.at(key)) != profile.requestedDevice) reject("person result model device");
  }
  const auto batches = unsignedNumber(wire.at("reidInferenceBatches"),
    3 * profile.limits.at("maxPersonsPerFrame"));
  const auto reidDevice = batches ? profile.requestedDevice : "not-run";
  if (text(wire.at("actualReidDevice")) != reidDevice ||
      text(wire.at("reidInputDevice")) != reidDevice) reject("person result ReID device");
  const auto canonicalProfile = Json::parse(profile.canonicalJson);
  if (wire.at("limits").dump() != canonicalProfile.at("limits").dump()) {
    reject("person result frozen limits");
  }
  const auto& counters = wire.at("counters");
  exactKeys(counters, {"gallerySize", "localKeyCount", "watermarkUs"});
  unsignedNumber(counters.at("gallerySize"), profile.limits.at("maxGlobalIdentities"));
  unsignedNumber(counters.at("localKeyCount"), profile.limits.at("maxLocalKeys"));
  const auto watermark = unsignedNumber(counters.at("watermarkUs"),
    std::numeric_limits<int64_t>::max());
  const auto& timing = wire.at("timing");
  exactKeys(timing, {"modelWindowNs"});
  unsignedNumber(timing.at("modelWindowNs"));
  const auto& frames = wire.at("frames");
  if (!frames.is_array() || frames.size() != 3) reject("person result three frames required");
  Json events = Json::array();
  uint64_t maximumPts = 0;
  for (size_t index = 0; index < 3; ++index) {
    const auto& frame = frames[index];
    exactKeys(frame, {"cameraId", "sourceId", "sourceEpoch", "trackerGeneration", "sequence",
      "inputDigest", "commonPtsUs", "width", "height", "clockAvailable", "clockUncertaintyUs",
      "geometryStatus", "observationName", "observationDigest", "observation", "tracks",
      "lostByteTrackIds"});
    const auto camera = "UAV" + std::to_string(index + 1);
    const auto generation = unsignedNumber(frame.at("trackerGeneration"));
    if (text(frame.at("cameraId")) != camera ||
        text(frame.at("sourceId")) != expected.sourceIds[index] ||
        unsignedNumber(frame.at("sourceEpoch")) != expected.sourceEpoch || generation == 0 ||
        unsignedNumber(frame.at("sequence")) != expected.sequence ||
        text(frame.at("inputDigest")) != expected.inputDigests[index] ||
        text(frame.at("geometryStatus")) != "UNKNOWN") reject("person result frame binding");
    const auto pts = unsignedNumber(frame.at("commonPtsUs"), std::numeric_limits<int64_t>::max());
    maximumPts = std::max(maximumPts, pts);
    const auto width = unsignedNumber(frame.at("width"), profile.limits.at("maxFrameWidth"));
    const auto height = unsignedNumber(frame.at("height"), profile.limits.at("maxFrameHeight"));
    if (width == 0 || height == 0) reject("person result frame dimensions");
    boolean(frame.at("clockAvailable"));
    unsignedNumber(frame.at("clockUncertaintyUs"));
    const auto producer = sourceProducer(expected, index);
    const auto observationName = makeUavObservationDataName(
      producer, camera, expected.sourceEpoch, expected.sequence, "OBSERVATION", 1);
    const auto imageName = makeUavObservationDataName(
      producer, camera, expected.sourceEpoch, expected.sequence, "FRAME", 1);
    const auto observationDigest = text(frame.at("observationDigest"));
    if (text(frame.at("observationName")) != observationName.toUri() ||
        !isDigest(observationDigest)) reject("person result observation binding");
    const auto& observation = frame.at("observation");
    exactKeys(observation, {"schema", "sourceId", "sourceEpoch", "sequence", "observationRef",
                            "imageRef"});
    if (text(observation.at("schema")) != "uav-observation-v2" ||
        text(observation.at("sourceId")) != expected.sourceIds[index] ||
        unsignedNumber(observation.at("sourceEpoch")) != expected.sourceEpoch ||
        unsignedNumber(observation.at("sequence")) != expected.sequence) {
      reject("person result observation source");
    }
    validatePersonReference(observation.at("observationRef"), observationName, observationDigest,
                             producer);
    validatePersonReference(observation.at("imageRef"), imageName, expected.inputDigests[index],
                             producer);
    const auto& tracks = frame.at("tracks");
    if (!tracks.is_array() || tracks.size() > profile.limits.at("maxPersonsPerFrame")) {
      reject("person result track count");
    }
    std::set<uint64_t> localIds;
    std::set<uint64_t> globalIds;
    uint64_t previousLocal = 0;
    for (const auto& track : tracks) {
      exactKeys(track, {"cameraId", "sourceId", "sourceEpoch", "trackerGeneration", "localId",
        "byteTrackId", "globalId", "class", "confidence", "box", "ptsUs", "associationStatus",
        "appearanceDistance", "associationOrigin"});
      if (text(track.at("cameraId")) != camera ||
          text(track.at("sourceId")) != expected.sourceIds[index] ||
          unsignedNumber(track.at("sourceEpoch")) != expected.sourceEpoch ||
          unsignedNumber(track.at("trackerGeneration")) != generation ||
          unsignedNumber(track.at("ptsUs"), std::numeric_limits<int64_t>::max()) != pts ||
          text(track.at("class")) != "person") reject("person result local/source binding");
      const auto confidence = finiteNumber(track.at("confidence"));
      if (confidence < 0 || confidence > 1) reject("person result confidence");
      const auto& box = track.at("box");
      if (!box.is_array() || box.size() != 4) reject("person result box dimensions");
      std::array<uint64_t, 4> bounds;
      for (size_t i = 0; i < 4; ++i) {
        bounds[i] = unsignedNumber(box[i], i % 2 == 0 ? width : height);
      }
      if (bounds[0] >= bounds[2] || bounds[1] >= bounds[3]) reject("person result empty box");
      const auto status = text(track.at("associationStatus"));
      if (std::find(PERSON_ASSOCIATION_STATUSES.begin(),
                    PERSON_ASSOCIATION_STATUSES.end(), status) ==
            PERSON_ASSOCIATION_STATUSES.end() ||
          track.at("localId").dump() != track.at("byteTrackId").dump()) {
        reject("person result identity/status");
      }
      uint64_t localId = 0;
      if (track.at("byteTrackId").is_null()) {
        if (!track.at("globalId").is_null() || status != "UNTRACKED") {
          reject("invented untracked ID");
        }
      }
      else {
        localId = unsignedNumber(track.at("byteTrackId"));
        const auto globalId = unsignedNumber(track.at("globalId"));
        if (localId == 0 || globalId == 0 || status == "UNTRACKED" ||
            !localIds.insert(localId).second || !globalIds.insert(globalId).second) {
          reject("person result duplicate/zero local/global ID");
        }
      }
      if (localId < previousLocal) reject("person result local-key order");
      previousLocal = localId;
      const bool requiresDistance = status == "MATCHED" || status == "AMBIGUOUS" ||
        status == "COMPETITION";
      if (batches == 0 && (requiresDistance || status == "NO_CANDIDATE" ||
                           status == "CLOCK_UNAVAILABLE")) {
        reject("person result feature decision without inference");
      }
      if (requiresDistance == track.at("appearanceDistance").is_null()) {
        reject("person result distance/status");
      }
      if (requiresDistance) {
        const auto distance = finiteNumber(track.at("appearanceDistance"));
        const auto maximum = static_cast<double>(profile.limits.at("maxCosineDistancePpm")) / 1e6;
        if (distance < 0 || distance > maximum) reject("person result cosine distance");
      }
      validatePersonOrigin(track, frame, frames, profile, expected);
      events.push_back(personEvent(track));
    }
    const auto& lost = frame.at("lostByteTrackIds");
    if (!lost.is_array() || lost.size() > profile.limits.at("maxBackendTracksPerCamera")) {
      reject("person result lost count");
    }
    std::set<uint64_t> lostIds;
    for (const auto& id : lost) {
      const auto value = unsignedNumber(id);
      if (value == 0 || localIds.count(value) || !lostIds.insert(value).second) {
        reject("person result active/lost identity conflict");
      }
    }
  }
  if (watermark != maximumPts || wire.at("associationEvents").dump() != events.dump()) {
    reject("person result event/watermark consistency");
  }
}

} // namespace

std::string
makeUavPersonTrackingResultJson(const UavPersonWindowResult& result,
                                const UavPersonBackendWindow& backend,
                                const UavPersonTrackingProfile& inputProfile,
                                const std::array<std::string, 3>& observationNames,
                                const std::array<std::string, 3>& observationDigests)
{
  return makeUavPersonTrackingResultJson(result, backend, inputProfile, observationNames,
    observationDigests, {droneIdentity("UAV1"), droneIdentity("UAV2"), droneIdentity("UAV3")});
}

std::string
makeUavPersonTrackingResultJson(const UavPersonWindowResult& result,
                                const UavPersonBackendWindow& backend,
                                const UavPersonTrackingProfile& inputProfile,
                                const std::array<std::string, 3>& observationNames,
                                const std::array<std::string, 3>& observationDigests,
                                const std::array<ndn::Name, 3>& sourceProducers)
{
  if (std::any_of(sourceProducers.begin(), sourceProducers.end(),
                  [] (const ndn::Name& producer) { return producer.empty(); })) {
    reject("person result builder requires explicit source Producers");
  }
  const auto profile = parseUavPersonTrackingProfile(inputProfile.canonicalJson);
  if (!(result.context == backend.context) || backend.profileDigest != profile.profileDigest) {
    reject("person result builder window/profile");
  }
  const auto& context = result.context;
  Json frames = Json::array();
  Json events = Json::array();
  UavTrackingResultExpectation expected;
  expected.runId = context.runId;
  expected.missionId = context.missionId;
  expected.windowId = context.windowId;
  expected.sequence = context.sequence;
  expected.sourceEpoch = context.frames[0].sourceEpoch;
  expected.personTracking = UavPersonTrackingResultExpectation{profile, context.computationDigest};
  expected.sourceProviders = sourceProducers;
  for (size_t index = 0; index < 3; ++index) {
    const auto& frame = context.frames[index];
    expected.sourceIds[index] = frame.sourceId;
    expected.inputDigests[index] = frame.inputDigest;
    const auto producer = sourceProducer(expected, index);
    const auto imageName = makeUavObservationDataName(producer, frame.cameraId, frame.sourceEpoch,
      frame.sequence, "FRAME", 1);
    Json observation = {{"schema", "uav-observation-v2"}, {"sourceId", frame.sourceId},
      {"sourceEpoch", frame.sourceEpoch}, {"sequence", frame.sequence},
      {"observationRef", {{"name", observationNames[index]}, {"digest", observationDigests[index]},
                           {"producerIdentity", producer.toUri()}}},
      {"imageRef", {{"name", imageName.toUri()}, {"digest", frame.inputDigest},
                     {"producerIdentity", producer.toUri()}}}};
    Json tracks = Json::array();
    for (const auto& track : result.tracks[index]) {
      if (track.distance && !std::isfinite(*track.distance)) reject("nonfinite native distance");
      Json row = {{"cameraId", track.key.cameraId}, {"sourceId", track.key.sourceId},
        {"sourceEpoch", track.key.sourceEpoch}, {"trackerGeneration", track.key.trackerGeneration},
        {"localId", track.key.byteTrackId ? Json(track.key.byteTrackId) : Json(nullptr)},
        {"byteTrackId", track.key.byteTrackId ? Json(track.key.byteTrackId) : Json(nullptr)},
        {"globalId", track.globalId ? Json(*track.globalId) : Json(nullptr)}, {"class", "person"},
        {"confidence", track.confidence}, {"box", track.box}, {"ptsUs", track.commonPtsUs},
        {"associationStatus", track.associationStatus},
        {"appearanceDistance", track.distance ? Json(*track.distance) : Json(nullptr)},
        {"associationOrigin", nullptr}};
      if (track.associationOrigin) {
        const auto& origin = *track.associationOrigin;
        const auto& key = origin.key;
        row["associationOrigin"] = {
          {"localKey", {{"cameraId", key.cameraId}, {"sourceId", key.sourceId},
            {"sourceEpoch", key.sourceEpoch}, {"trackerGeneration", key.trackerGeneration},
            {"byteTrackId", key.byteTrackId}}},
          {"ptsUs", origin.ptsUs}, {"active", origin.active}};
      }
      events.push_back(personEvent(row));
      tracks.push_back(std::move(row));
    }
    frames.push_back({{"cameraId", frame.cameraId}, {"sourceId", frame.sourceId},
      {"sourceEpoch", frame.sourceEpoch}, {"trackerGeneration", frame.trackerGeneration},
      {"sequence", frame.sequence}, {"inputDigest", frame.inputDigest},
      {"commonPtsUs", frame.commonPtsUs}, {"width", frame.width}, {"height", frame.height},
      {"clockAvailable", frame.clockAvailable}, {"clockUncertaintyUs", frame.clockUncertaintyUs},
      {"geometryStatus", "UNKNOWN"}, {"observationName", observationNames[index]},
      {"observationDigest", observationDigests[index]}, {"observation", std::move(observation)},
      {"tracks", std::move(tracks)}, {"lostByteTrackIds", backend.frames[index].lostByteTrackIds}});
  }
  Json wire = {{"schema", "uav-person-tracking-result-v1"}, {"terminal", true},
    {"runId", context.runId}, {"missionId", context.missionId}, {"windowId", context.windowId},
    {"sequence", context.sequence}, {"trackingMode", "indoor-person-reid"},
    {"profileDigest", profile.profileDigest}, {"computationDigest", context.computationDigest},
    {"detectorDigest", profile.detectorDigest}, {"reidDigest", profile.reidDigest},
    {"preprocessId", profile.reidPreprocessId},
    {"requestedDevice", backend.requestedDevice},
    {"actualDetectorDevice", backend.actualDetectorDevice},
    {"detectorModelDevice", backend.detectorModelDevice},
    {"detectorInputDevice", backend.detectorInputDevice},
    {"actualReidDevice", backend.actualReidDevice}, {"reidModelDevice", backend.reidModelDevice},
    {"reidInputDevice", backend.reidInputDevice},
    {"reidInferenceBatches", backend.reidInferenceBatches},
    {"frames", std::move(frames)}, {"associationEvents", std::move(events)},
    {"limits", Json::parse(profile.canonicalJson).at("limits")},
    {"counters", {{"gallerySize", result.gallerySize}, {"localKeyCount", result.localKeyCount},
                    {"watermarkUs", result.watermarkUs}}},
    {"timing", {{"modelWindowNs", backend.modelWindowNs}}}};
  auto encoded = wire.dump();
  validatePersonResult(encoded, expected);
  return encoded;
}

void
validateUavTrackingResultContext(const std::string& resultName,
                                 const std::string& responseProvider,
                                 const std::string& resultJson,
                                 const UavTrackingResultExpectation& expected)
{
  const bool legacyVehicle = expected.vehicleTracking && !expected.vehicleTracking->observationMode;
  if (expected.runId.empty() || expected.missionId.empty() || expected.windowId.empty() ||
      expected.sequence == 0 || (!legacyVehicle && expected.sourceEpoch == 0) ||
      expected.computeProvider.empty() || resultJson.empty() ||
      (expected.personTracking && expected.vehicleTracking)) {
    throw std::invalid_argument("tracking result expectation is incomplete");
  }
  for (size_t i = 0; i < expected.sourceIds.size(); ++i) {
    if (expected.vehicleTracking && expected.sourceProviders[i].empty()) {
      throw std::invalid_argument("native vehicle result requires explicit original Producers");
    }
    if (!legacyVehicle && expected.sourceIds[i].empty()) {
      throw std::invalid_argument("tracking result source identity is incomplete");
    }
    requireDigest(expected.inputDigests[i], "input");
  }

  const auto provider = requireCanonicalName(responseProvider, "provider");
  if (provider != expected.computeProvider) {
    throw std::runtime_error("tracking result provider does not match the selected compute role");
  }
  const auto namedResult = requireCanonicalName(resultName, "result");
  const auto expectedName = makeUavMultiViewResultName(
    expected.computeProvider, expected.missionId, expected.windowId, expected.sequence, 1);
  if (namedResult != expectedName ||
      !isUavProviderMultiViewDataName(expected.computeProvider, namedResult, "RESULT",
                                      expected.missionId, expected.windowId)) {
    throw std::runtime_error("tracking result name does not match the request context");
  }

  if (expected.personTracking) {
    validatePersonResult(resultJson, expected);
    return;
  }
  if (expected.vehicleTracking) {
    const auto native = parseBounded(resultJson, 8 * 1024 * 1024);
    for (const auto* field : {"schema", "runId", "missionId", "windowId", "decisionSchema",
                               "nativeDecisionDigest", "actualDevice"}) text(native.at(field));
    if (unsignedNumber(native.at("sequence")) != expected.sequence || !boolean(native.at("terminal"))) {
      reject("native vehicle terminal/sequence type");
    }
    const auto& nativeFrames = native.at("frames");
    if (!nativeFrames.is_array() || nativeFrames.size() != 3) reject("native vehicle frame count");
    for (const auto& frame : nativeFrames) {
      text(frame.at("cameraId"));
      text(frame.at("inputDigest"));
      if (unsignedNumber(frame.at("sequence")) != expected.sequence) reject("native frame sequence");
      unsignedNumber(frame.at("ptsUs"), std::numeric_limits<int64_t>::max());
      const auto width = unsignedNumber(frame.at("imageWidth"), 8192);
      const auto height = unsignedNumber(frame.at("imageHeight"), 8192);
      if (width == 0 || height == 0) reject("native vehicle dimensions");
    }
  }

  requireJsonNestingBounded(resultJson);
  std::istringstream input(resultJson);
  ptree result;
  boost::property_tree::read_json(input, result);
  requireUniqueKeys(result);
  if (requiredScalar(result, "schema") !=
        (legacyVehicle ? "spec191-tracking-result-v1" : "spec210-tracking-result-v2") ||
      requiredScalar(result, "runId") != expected.runId ||
      requiredScalar(result, "missionId") != expected.missionId ||
      requiredScalar(result, "windowId") != expected.windowId ||
      requiredPositiveInteger(result, "sequence") != expected.sequence ||
      requiredScalar(result, "terminal") != "true") {
    throw std::runtime_error("tracking result header does not match the request context");
  }
  if (expected.vehicleTracking) {
    const auto decisionDigest = requiredScalar(result, "nativeDecisionDigest");
    requireDigest(decisionDigest, "native vehicle decision");
    if (requiredScalar(result, "decisionSchema") != "uav-vehicle-native-decision-v1" ||
        requiredScalar(result, "actualDevice") != "cpu" ||
        (!expected.vehicleTracking->nativeDecisionDigest.empty() &&
         decisionDigest != expected.vehicleTracking->nativeDecisionDigest)) {
      throw std::runtime_error("tracking result native vehicle computation mismatch");
    }
  }

  const auto& frames = requiredChild(result, "frames");
  if (frames.size() != expected.sourceIds.size()) {
    throw std::runtime_error("tracking result must contain exactly three ordered source frames");
  }
  size_t index = 0;
  for (const auto& frameEntry : frames) {
    if (!frameEntry.first.empty()) {
      throw std::runtime_error("tracking result frames must be an array");
    }
    const auto& frame = frameEntry.second;
    const std::string camera = "UAV" + std::to_string(index + 1);
    if (legacyVehicle) {
      const auto expectedImage = makeUavEvidenceName(sourceProducer(expected, index), expected.missionId,
        expected.windowId, camera + "-" + std::to_string(expected.sequence), 1);
      const auto& image = requiredChild(frame, "imageRef");
      if (requiredScalar(frame, "cameraId") != camera ||
          requiredPositiveInteger(frame, "sequence") != expected.sequence ||
          requiredScalar(frame, "inputDigest") != expected.inputDigests[index] ||
          requireCanonicalName(requiredScalar(image, "name"), "image") != expectedImage ||
          requiredScalar(image, "digest") != expected.inputDigests[index]) {
        throw std::runtime_error("legacy tracking result frame does not match original source Data");
      }
      ++index;
      continue;
    }
    if (requiredScalar(frame, "cameraId") != camera ||
        requiredPositiveInteger(frame, "sequence") != expected.sequence ||
        requiredScalar(frame, "sourceId") != expected.sourceIds[index] ||
        requiredPositiveInteger(frame, "sourceEpoch") != expected.sourceEpoch ||
        requiredScalar(frame, "inputDigest") != expected.inputDigests[index]) {
      throw std::runtime_error("tracking result frame does not match its requested source");
    }
    if (expected.vehicleTracking) {
      const auto expectedImage = makeUavObservationDataName(sourceProducer(expected, index),
        camera, expected.sourceEpoch, expected.sequence, "FRAME", 1);
      const auto& image = requiredChild(frame, "imageRef");
      if (requireCanonicalName(requiredScalar(image, "name"), "image") != expectedImage ||
          requiredScalar(image, "digest") != expected.inputDigests[index]) {
        throw std::runtime_error("native vehicle frame does not reference original source Data");
      }
    }

    const auto expectedObservationName = makeUavObservationDataName(
      sourceProducer(expected, index), camera, expected.sourceEpoch, expected.sequence, "OBSERVATION", 1);
    const auto observationName = requiredScalar(frame, "observationName");
    const auto observationDigest = requiredScalar(frame, "observationDigest");
    requireDigest(observationDigest, "observation");
    const auto& observation = requiredChild(frame, "observation");
    if (requiredScalar(observation, "schema") != "uav-observation-v2" ||
        requiredScalar(observation, "sourceId") != expected.sourceIds[index] ||
        requiredPositiveInteger(observation, "sourceEpoch") != expected.sourceEpoch ||
        requiredPositiveInteger(observation, "sequence") != expected.sequence) {
      throw std::runtime_error("tracking result observation identity does not match its frame");
    }
    const auto& observationRef = requiredChild(observation, "observationRef");
    const auto referenceName = requireCanonicalName(
      requiredScalar(observationRef, "name"), "observation reference");
    if (referenceName != expectedObservationName || observationName != referenceName.toUri() ||
        requiredScalar(observationRef, "digest") != observationDigest) {
      throw std::runtime_error("tracking result observation reference does not match its frame");
    }
    ++index;
  }
}

} // namespace ndnsf::examples::uav
