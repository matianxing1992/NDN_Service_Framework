#include "uav-person-tracking.hpp"

#include "uav-person-json.hpp"
#include "uav-byte-track-state.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace ndnsf::examples::uav {
namespace {

using person_json::Json;
using person_json::boolean;
using person_json::checkIdentifier;
using person_json::exactKeys;
using person_json::finiteNumber;
using person_json::finiteArray;
using byte_track::BackendStateFormat;
using byte_track::checkTracker;
using person_json::isDigest;
using person_json::parseBounded;
using person_json::reject;
using person_json::text;
using person_json::unsignedNumber;
constexpr std::array<const char*, 3> CAMERAS = {"UAV1", "UAV2", "UAV3"};

void
checkContext(const UavPersonWindowContext& context, const UavPersonTrackingProfile& profile)
{
  checkIdentifier(context.runId);
  checkIdentifier(context.missionId);
  checkIdentifier(context.windowId);
  if (context.sequence == 0 || !isDigest(context.computationDigest)) reject("native window");
  requireUavPersonWindowCapacity(profile, context.sequence);
  for (size_t i = 0; i < CAMERAS.size(); ++i) {
    const auto& frame = context.frames[i];
    checkIdentifier(frame.sourceId);
    if (frame.cameraId != CAMERAS[i] || !isDigest(frame.inputDigest) || frame.sourceEpoch == 0 ||
        frame.trackerGeneration == 0 || frame.sequence == 0 || frame.commonPtsUs < 0 ||
        frame.width <= 0 || frame.height <= 0 ||
        static_cast<uint64_t>(frame.width) > profile.limits.at("maxFrameWidth") ||
        static_cast<uint64_t>(frame.height) > profile.limits.at("maxFrameHeight")) {
      reject("native frame context");
    }
  }
}

void
checkSource(const Json& wire, const UavPersonFrameContext& expected)
{
  if (text(wire.at("sourceId")) != expected.sourceId ||
      unsignedNumber(wire.at("sourceEpoch")) != expected.sourceEpoch ||
      unsignedNumber(wire.at("trackerGeneration")) != expected.trackerGeneration) {
    reject("source/epoch/generation binding");
  }
}


std::array<float, 512>
unitEmbedding(const Json& wire)
{
  finiteArray(wire, 512);
  std::array<float, 512> vector;
  double normSquared = 0;
  for (size_t i = 0; i < vector.size(); ++i) {
    const auto value = finiteNumber(wire[i]);
    if (std::abs(value) > 1.001) reject("embedding component range");
    vector[i] = static_cast<float>(value);
    normSquared += static_cast<double>(vector[i]) * vector[i];
  }
  if (std::abs(std::sqrt(normSquared) - 1.0) > 1e-3) reject("embedding norm");
  return vector;
}

BackendStateFormat
backendStateFormat(const Json& state)
{
  exactKeys(state, {"schema", "cameras"});
  const auto schema = text(state.at("schema"));
  if (schema == "uav-person-backend-state-v1") return BackendStateFormat::LEGACY_CONFIRMED;
  if (schema == "uav-person-backend-state-v2") return BackendStateFormat::COMPACT_CONFIRMED;
  if (schema == "uav-person-backend-state-v3") return BackendStateFormat::COMPACT_CURRENT;
  reject("backend state schema");
}

UavPersonDetection
parseDetection(const Json& wire, const UavPersonFrameContext& expected)
{
  exactKeys(wire, {"class", "byteTrackId", "box", "confidence", "active", "embedding"});
  if (text(wire.at("class")) != "person") reject("nonperson class");
  UavPersonDetection result;
  result.active = boolean(wire.at("active"));
  result.key = {expected.cameraId, expected.sourceId, expected.sourceEpoch,
                expected.trackerGeneration, 0};
  result.commonPtsUs = expected.commonPtsUs;
  if (!wire.at("byteTrackId").is_null()) {
    result.key.byteTrackId = unsignedNumber(wire.at("byteTrackId"));
    if (result.key.byteTrackId == 0) reject("zero local ID");
  }
  if (result.active != (result.key.byteTrackId != 0)) reject("local ID/active mismatch");
  const auto& box = wire.at("box");
  if (!box.is_array() || box.size() != 4) reject("box dimensions");
  for (size_t i = 0; i < 4; ++i) {
    result.box[i] = static_cast<int>(unsignedNumber(box[i], i % 2 == 0 ?
      static_cast<uint64_t>(expected.width) : static_cast<uint64_t>(expected.height)));
  }
  if (result.box[0] >= result.box[2] || result.box[1] >= result.box[3]) reject("empty box");
  result.confidence = finiteNumber(wire.at("confidence"));
  if (result.confidence < 0 || result.confidence > 1) reject("confidence range");
  const auto& embedding = wire.at("embedding");
  if (!embedding.is_null()) result.embedding = unitEmbedding(embedding);
  return result;
}

} // namespace

bool
UavPersonLocalKey::operator<(const UavPersonLocalKey& other) const
{
  return std::tie(cameraId, sourceId, sourceEpoch, trackerGeneration, byteTrackId) <
    std::tie(other.cameraId, other.sourceId, other.sourceEpoch,
             other.trackerGeneration, other.byteTrackId);
}

UavPersonBackendWindow
parseUavPersonBackendWindow(const std::string& json,
                            const UavPersonWindowContext& expected,
                            const UavPersonTrackingProfile& profile)
{
  checkContext(expected, profile);
  const auto wire = parseBounded(json, profile.limits.at("maxResultBytes"));
  exactKeys(wire, {"schema", "requestedDevice", "actualDetectorDevice", "detectorModelDevice",
                   "detectorInputDevice", "actualReidDevice", "reidModelDevice", "reidInputDevice",
                   "reidInferenceBatches", "detectorDigest", "reidDigest", "profileDigest",
                   "frames", "backendState", "timing", "runId", "missionId", "windowId",
                   "sequence", "computationDigest"});
  if (text(wire.at("schema")) != "uav-person-backend-v1" ||
      text(wire.at("runId")) != expected.runId ||
      text(wire.at("missionId")) != expected.missionId ||
      text(wire.at("windowId")) != expected.windowId ||
      unsignedNumber(wire.at("sequence")) != expected.sequence ||
      text(wire.at("computationDigest")) != expected.computationDigest ||
      text(wire.at("profileDigest")) != profile.profileDigest ||
      text(wire.at("detectorDigest")) != profile.detectorDigest ||
      text(wire.at("reidDigest")) != profile.reidDigest) reject("window/model/profile binding");
  UavPersonBackendWindow result;
  result.context = expected;
  result.profileDigest = profile.profileDigest;
  result.requestedDevice = text(wire.at("requestedDevice"));
  result.actualDetectorDevice = text(wire.at("actualDetectorDevice"));
  result.detectorModelDevice = text(wire.at("detectorModelDevice"));
  result.detectorInputDevice = text(wire.at("detectorInputDevice"));
  result.actualReidDevice = text(wire.at("actualReidDevice"));
  result.reidModelDevice = text(wire.at("reidModelDevice"));
  result.reidInputDevice = text(wire.at("reidInputDevice"));
  result.reidInferenceBatches = unsignedNumber(wire.at("reidInferenceBatches"),
    3 * profile.limits.at("maxPersonsPerFrame"));
  for (const auto* device : {&result.requestedDevice, &result.actualDetectorDevice,
                             &result.detectorModelDevice, &result.detectorInputDevice,
                             &result.reidModelDevice}) {
    if (*device != profile.requestedDevice) reject("model device binding");
  }
  const auto reidDevice = result.reidInferenceBatches == 0 ? "not-run" : profile.requestedDevice;
  if (result.actualReidDevice != reidDevice || result.reidInputDevice != reidDevice) {
    reject("ReID inference device binding");
  }
  const auto& timing = wire.at("timing");
  exactKeys(timing, {"modelWindowNs"});
  result.modelWindowNs = unsignedNumber(timing.at("modelWindowNs"));
  const auto& state = wire.at("backendState");
  const auto format = backendStateFormat(state);
  const auto& cameras = state.at("cameras");
  exactKeys(cameras, {"UAV1", "UAV2", "UAV3"});
  result.backendStateJson = state.dump();
  if (result.backendStateJson.size() > profile.limits.at("maxSnapshotBytes")) reject("state bytes");
  const auto& frames = wire.at("frames");
  if (!frames.is_array() || frames.size() != 3) reject("three frames required");
  for (size_t i = 0; i < frames.size(); ++i) {
    const auto& expectedFrame = expected.frames[i];
    const auto& frame = frames[i];
    exactKeys(frame, {"cameraId", "sourceId", "sourceEpoch", "trackerGeneration", "sequence",
                      "commonPtsUs", "inputDigest", "detections", "lostByteTrackIds"});
    checkSource(frame, expectedFrame);
    if (text(frame.at("cameraId")) != expectedFrame.cameraId ||
        unsignedNumber(frame.at("sequence")) != expectedFrame.sequence ||
        unsignedNumber(frame.at("commonPtsUs"), std::numeric_limits<int64_t>::max()) !=
          static_cast<uint64_t>(expectedFrame.commonPtsUs) ||
        text(frame.at("inputDigest")) != expectedFrame.inputDigest) reject("frame input binding");
    const auto& cameraState = cameras.at(expectedFrame.cameraId);
    exactKeys(cameraState, {"sourceId", "sourceEpoch", "trackerGeneration", "tracker"});
    checkSource(cameraState, expectedFrame);
    const auto tracker = checkTracker(cameraState.at("tracker"),
                                      profile.limits.at("maxBackendTracksPerCamera"), format);
    auto& output = result.frames[i];
    output.context = expectedFrame;
    const auto& detections = frame.at("detections");
    if (!detections.is_array() || detections.size() > profile.limits.at("maxPersonsPerFrame")) {
      reject("PERSON_LIMIT");
    }
    std::set<uint64_t> localIds;
    for (const auto& detection : detections) {
      auto admitted = parseDetection(detection, expectedFrame);
      if (admitted.active && (!localIds.insert(admitted.key.byteTrackId).second ||
          !tracker.currentIds.count(admitted.key.byteTrackId))) reject("active local key/state");
      if (admitted.embedding && result.reidInferenceBatches == 0) reject("feature without inference");
      output.detections.push_back(std::move(admitted));
    }
    if (localIds != tracker.currentIds) reject("missing active stock detection");
    const auto& lostIds = frame.at("lostByteTrackIds");
    if (!lostIds.is_array() || lostIds.size() > profile.limits.at("maxBackendTracksPerCamera")) {
      reject("lost ID count");
    }
    std::set<uint64_t> lost;
    for (const auto& id : lostIds) {
      const auto value = unsignedNumber(id);
      if (value == 0 || !lost.insert(value).second) reject("duplicate/zero lost ID");
      output.lostByteTrackIds.push_back(value);
    }
    if (lost != tracker.lost) reject("lost stock ID/state mismatch");
  }
  return result;
}

bool
UavPersonFrameContext::operator==(const UavPersonFrameContext& b) const
{
  const auto& a = *this;
  return std::tie(a.cameraId, a.sourceId, a.inputDigest, a.sourceEpoch, a.trackerGeneration,
                   a.sequence, a.commonPtsUs, a.width, a.height, a.clockAvailable,
                   a.clockUncertaintyUs) ==
    std::tie(b.cameraId, b.sourceId, b.inputDigest, b.sourceEpoch, b.trackerGeneration,
             b.sequence, b.commonPtsUs, b.width, b.height, b.clockAvailable,
             b.clockUncertaintyUs);
}

bool
UavPersonWindowContext::operator==(const UavPersonWindowContext& other) const
{
  return std::tie(runId, missionId, windowId, computationDigest, sequence) ==
    std::tie(other.runId, other.missionId, other.windowId, other.computationDigest, other.sequence) &&
    frames == other.frames;
}

namespace {

void
validateAdmittedWindow(const UavPersonWindowContext& context, const UavPersonBackendWindow& backend,
                       const UavPersonTrackingProfile& profile)
{
  checkContext(context, profile);
  const auto& binding = backend.context;
  if (backend.profileDigest != profile.profileDigest ||
      std::tie(context.runId, context.missionId, context.windowId, context.sequence,
               context.computationDigest) !=
        std::tie(binding.runId, binding.missionId, binding.windowId, binding.sequence,
                 binding.computationDigest)) reject("admitted window binding");
  for (const auto* device : {&backend.requestedDevice, &backend.actualDetectorDevice,
                             &backend.detectorModelDevice, &backend.detectorInputDevice,
                             &backend.reidModelDevice}) {
    if (*device != profile.requestedDevice) reject("admitted model device");
  }
  const auto reidDevice = backend.reidInferenceBatches ? profile.requestedDevice : "not-run";
  if (backend.reidInferenceBatches > 3 * profile.limits.at("maxPersonsPerFrame") ||
      backend.actualReidDevice != reidDevice || backend.reidInputDevice != reidDevice) {
    reject("admitted ReID device/count");
  }
  const auto state = parseBounded(backend.backendStateJson, profile.limits.at("maxSnapshotBytes"));
  const auto format = backendStateFormat(state);
  exactKeys(state.at("cameras"), {"UAV1", "UAV2", "UAV3"});
  for (size_t i = 0; i < 3; ++i) {
    const auto& frame = backend.frames[i];
    const auto& expected = context.frames[i];
    if (!(expected == binding.frames[i]) ||
        !(expected == frame.context)) reject("admitted frame binding");
    const auto& cameraState = state.at("cameras").at(expected.cameraId);
    exactKeys(cameraState, {"sourceId", "sourceEpoch", "trackerGeneration", "tracker"});
    checkSource(cameraState, expected);
    const auto tracker = checkTracker(cameraState.at("tracker"),
                                      profile.limits.at("maxBackendTracksPerCamera"), format);
    if (frame.detections.size() > profile.limits.at("maxPersonsPerFrame") ||
        frame.lostByteTrackIds.size() > profile.limits.at("maxBackendTracksPerCamera")) {
      reject("admitted count limit");
    }
    std::set<uint64_t> active;
    for (const auto& detection : frame.detections) {
      const auto& key = detection.key;
      if (key.cameraId != expected.cameraId || key.sourceId != expected.sourceId ||
          key.sourceEpoch != expected.sourceEpoch ||
          key.trackerGeneration != expected.trackerGeneration ||
          detection.commonPtsUs != expected.commonPtsUs ||
          detection.active != (key.byteTrackId != 0) ||
          !std::isfinite(detection.confidence) || detection.confidence < 0 ||
          detection.confidence > 1 || detection.box[0] < 0 || detection.box[1] < 0 ||
          detection.box[0] >= detection.box[2] || detection.box[1] >= detection.box[3] ||
          detection.box[2] > expected.width || detection.box[3] > expected.height) {
        reject("admitted detection invariant");
      }
      if (detection.active && !active.insert(key.byteTrackId).second) reject("duplicate admitted ID");
      if (detection.embedding) {
        double norm = 0;
        for (const auto value : *detection.embedding) {
          if (!std::isfinite(value)) reject("admitted nonfinite feature");
          norm += static_cast<double>(value) * value;
        }
        if (backend.reidInferenceBatches == 0 || std::abs(std::sqrt(norm) - 1.0) > 1e-3) {
          reject("admitted feature invariant");
        }
      }
    }
    const std::set<uint64_t> lost(frame.lostByteTrackIds.begin(), frame.lostByteTrackIds.end());
    if (active != tracker.currentIds || lost != tracker.lost ||
        lost.size() != frame.lostByteTrackIds.size()) reject("admitted tracker correspondence");
  }
}

UavPersonTrack
decisionRow(const UavPersonDetection& detection)
{
  return {detection.key, detection.commonPtsUs, detection.box, detection.confidence,
          std::nullopt, "UNTRACKED", std::nullopt, std::nullopt};
}

Json
contextJson(const UavPersonWindowContext& context)
{
  Json frames = Json::array();
  for (const auto& frame : context.frames) {
    frames.push_back({{"cameraId", frame.cameraId}, {"sourceId", frame.sourceId},
      {"inputDigest", frame.inputDigest}, {"sourceEpoch", frame.sourceEpoch},
      {"trackerGeneration", frame.trackerGeneration}, {"sequence", frame.sequence},
      {"commonPtsUs", frame.commonPtsUs}, {"width", frame.width}, {"height", frame.height},
      {"clockAvailable", frame.clockAvailable}, {"clockUncertaintyUs", frame.clockUncertaintyUs}});
  }
  return {{"runId", context.runId}, {"missionId", context.missionId},
    {"windowId", context.windowId}, {"computationDigest", context.computationDigest},
    {"sequence", context.sequence}, {"frames", std::move(frames)}};
}

UavPersonWindowContext
parseContext(const Json& wire, const UavPersonTrackingProfile& profile)
{
  exactKeys(wire, {"runId", "missionId", "windowId", "computationDigest", "sequence", "frames"});
  UavPersonWindowContext context;
  context.runId = text(wire.at("runId"));
  context.missionId = text(wire.at("missionId"));
  context.windowId = text(wire.at("windowId"));
  context.computationDigest = text(wire.at("computationDigest"));
  context.sequence = unsignedNumber(wire.at("sequence"));
  const auto& frames = wire.at("frames");
  if (!frames.is_array() || frames.size() != 3) reject("snapshot frame count");
  for (size_t i = 0; i < 3; ++i) {
    const auto& frame = frames[i];
    exactKeys(frame, {"cameraId", "sourceId", "inputDigest", "sourceEpoch", "trackerGeneration",
                      "sequence", "commonPtsUs", "width", "height", "clockAvailable",
                      "clockUncertaintyUs"});
    auto& output = context.frames[i];
    output.cameraId = text(frame.at("cameraId"));
    output.sourceId = text(frame.at("sourceId"));
    output.inputDigest = text(frame.at("inputDigest"));
    output.sourceEpoch = unsignedNumber(frame.at("sourceEpoch"));
    output.trackerGeneration = unsignedNumber(frame.at("trackerGeneration"));
    output.sequence = unsignedNumber(frame.at("sequence"));
    output.commonPtsUs = static_cast<int64_t>(unsignedNumber(frame.at("commonPtsUs"),
      std::numeric_limits<int64_t>::max()));
    output.width = static_cast<int>(unsignedNumber(frame.at("width"),
      profile.limits.at("maxFrameWidth")));
    output.height = static_cast<int>(unsignedNumber(frame.at("height"),
      profile.limits.at("maxFrameHeight")));
    output.clockAvailable = boolean(frame.at("clockAvailable"));
    output.clockUncertaintyUs = unsignedNumber(frame.at("clockUncertaintyUs"));
  }
  checkContext(context, profile);
  return context;
}

void
checkContinuation(const UavPersonWindowContext& context, const UavPersonWindowContext& previous,
                   int64_t watermarkUs)
{
  if (context.runId != previous.runId || context.missionId != previous.missionId ||
      context.computationDigest != previous.computationDigest) reject("MISSION_BINDING");
  if (previous.sequence == std::numeric_limits<uint64_t>::max() ||
      context.sequence != previous.sequence + 1) reject("SEQUENCE_VIOLATION");
  for (size_t i = 0; i < 3; ++i) {
    const auto& frame = context.frames[i];
    const auto& old = previous.frames[i];
    if (std::tie(frame.sourceId, frame.sourceEpoch, frame.trackerGeneration) !=
        std::tie(old.sourceId, old.sourceEpoch, old.trackerGeneration)) {
      reject("SOURCE_RESTART_REQUIRES_NEW_MISSION");
    }
    if (frame.commonPtsUs < watermarkUs || frame.sequence <= old.sequence) reject("ORDER_VIOLATION");
  }
}

Json
localKeyJson(const UavPersonLocalKey& key)
{
  return {{"cameraId", key.cameraId}, {"sourceId", key.sourceId}, {"sourceEpoch", key.sourceEpoch},
    {"trackerGeneration", key.trackerGeneration}, {"byteTrackId", key.byteTrackId}};
}

UavPersonLocalKey
parseLocalKey(const Json& wire, const UavPersonWindowContext& binding)
{
  exactKeys(wire, {"cameraId", "sourceId", "sourceEpoch", "trackerGeneration", "byteTrackId"});
  UavPersonLocalKey key{text(wire.at("cameraId")), text(wire.at("sourceId")),
    unsignedNumber(wire.at("sourceEpoch")), unsignedNumber(wire.at("trackerGeneration")),
    unsignedNumber(wire.at("byteTrackId"))};
  const auto camera = std::find(CAMERAS.begin(), CAMERAS.end(), key.cameraId);
  if (camera == CAMERAS.end() || key.byteTrackId == 0) reject("snapshot local key");
  const auto& expected = binding.frames[static_cast<size_t>(camera - CAMERAS.begin())];
  if (key.sourceId != expected.sourceId || key.sourceEpoch != expected.sourceEpoch ||
      key.trackerGeneration != expected.trackerGeneration) reject("snapshot key source binding");
  return key;
}

} // namespace

UavPersonTrackingEngine::UavPersonTrackingEngine(UavPersonTrackingProfile profile)
  : m_profile(parseUavPersonTrackingProfile(profile.canonicalJson))
{
}

void
UavPersonTrackingEngine::expireAt(int64_t ptsUs)
{
  const auto ttl = static_cast<int64_t>(m_profile.limits.at("galleryTtlUs"));
  const auto cutoff = ptsUs > ttl ? ptsUs - ttl : 0;
  for (auto it = m_gallery.begin(); it != m_gallery.end();) {
    if (it->second.lastSeenUs < cutoff) it = m_gallery.erase(it);
    else {
      auto& samples = it->second.samples;
      samples.erase(std::remove_if(samples.begin(), samples.end(), [cutoff] (const auto& sample) {
        return sample.ptsUs < cutoff;
      }), samples.end());
      ++it;
    }
  }
  for (auto it = m_localToGlobal.begin(); it != m_localToGlobal.end();) {
    if (it->second.lastSeenUs < cutoff || !m_gallery.count(it->second.globalId)) {
      it = m_localToGlobal.erase(it);
    }
    else ++it;
  }
}

uint64_t
UavPersonTrackingEngine::allocateIdentity(int64_t ptsUs)
{
  if (m_gallery.size() >= m_profile.limits.at("maxGlobalIdentities")) reject("GALLERY_LIMIT");
  if (m_nextGlobalId == std::numeric_limits<uint64_t>::max()) reject("GLOBAL_ID_EXHAUSTED");
  const auto id = m_nextGlobalId++;
  m_gallery.emplace(id, Identity{ptsUs, {}});
  return id;
}

void
UavPersonTrackingEngine::observe(const UavPersonDetection& detection, uint64_t globalId)
{
  const bool isNewKey = m_localToGlobal.count(detection.key) == 0;
  if (isNewKey) {
    // Transfer continuity from inactive owners in the same or non-overlapping view.
    for (auto it = m_localToGlobal.begin(); it != m_localToGlobal.end();) {
      const auto cameras = std::minmax(it->first.cameraId, detection.key.cameraId);
      const bool sameCamera = it->first.cameraId == detection.key.cameraId;
      if (it->second.globalId == globalId && !it->second.active &&
          (sameCamera || !m_profile.overlappingCameras.at({cameras.first, cameras.second}))) {
        it = m_localToGlobal.erase(it);
      }
      else ++it;
    }
  }
  if (isNewKey && m_localToGlobal.size() >= m_profile.limits.at("maxLocalKeys")) {
    reject("LOCAL_KEY_LIMIT");
  }
  m_localToGlobal[detection.key] = {globalId, detection.commonPtsUs, true};
  auto& identity = m_gallery.at(globalId);
  identity.lastSeenUs = detection.commonPtsUs;
  if (detection.embedding) {
    auto& samples = identity.samples;
    samples.push_back({detection.commonPtsUs, *detection.embedding});
    std::stable_sort(samples.begin(), samples.end(), [] (const auto& a, const auto& b) {
      return a.ptsUs < b.ptsUs;
    });
    if (samples.size() > m_profile.limits.at("samplesPerIdentity")) samples.erase(samples.begin());
  }
}

std::optional<std::array<double, 512>>
UavPersonTrackingEngine::prototype(const Identity& identity) const
{
  if (identity.samples.empty()) return std::nullopt;
  std::array<double, 512> mean{};
  for (const auto& sample : identity.samples) {
    for (size_t i = 0; i < mean.size(); ++i) mean[i] += sample.embedding[i];
  }
  double normSquared = 0;
  for (auto& value : mean) {
    value /= identity.samples.size();
    normSquared += value * value;
  }
  const auto norm = std::sqrt(normSquared);
  if (!std::isfinite(norm) || norm <= 1e-12) return std::nullopt;
  for (auto& value : mean) value /= norm;
  return mean;
}

std::optional<UavPersonAssociationOrigin>
UavPersonTrackingEngine::eligibilityOrigin(uint64_t globalId,
                                          const UavPersonDetection& detection,
                                          const UavPersonWindowContext& context) const
{
  const auto currentIndex = static_cast<size_t>(detection.key.cameraId.back() - '1');
  const auto& current = context.frames.at(currentIndex);
  const auto maxSkew = m_profile.limits.at("maxClockSkewUs");
  std::optional<UavPersonAssociationOrigin> origin;
  for (const auto& [key, mapping] : m_localToGlobal) {
    if (mapping.globalId != globalId) continue;
    const bool sameCamera = key.cameraId == detection.key.cameraId;
    if (sameCamera && mapping.active) return std::nullopt;
    const auto gap = detection.commonPtsUs - mapping.lastSeenUs;
    if (gap < 0) continue;
    bool eligible = false;
    if (sameCamera) {
      eligible = static_cast<uint64_t>(gap) <= m_profile.limits.at("maxTransitionUs");
    }
    else {
      const auto& other = context.frames.at(static_cast<size_t>(key.cameraId.back() - '1'));
      const bool clockValid = current.clockAvailable && other.clockAvailable &&
        current.clockUncertaintyUs <= maxSkew &&
        other.clockUncertaintyUs <= maxSkew - current.clockUncertaintyUs;
      const auto pair = std::minmax(key.cameraId, detection.key.cameraId);
      const bool overlap = m_profile.overlappingCameras.at({pair.first, pair.second});
      if (mapping.active) {
        if (!clockValid || !overlap || static_cast<uint64_t>(gap) > maxSkew) return std::nullopt;
        eligible = true;
      }
      else if (clockValid && static_cast<uint64_t>(gap) <= m_profile.limits.at("maxTransitionUs") &&
               (overlap || static_cast<uint64_t>(gap) >= m_profile.limits.at("minTransitionUs"))) {
        eligible = true;
      }
    }
    if (eligible && (!origin || mapping.lastSeenUs > origin->ptsUs ||
                     (mapping.lastSeenUs == origin->ptsUs && key < origin->key))) {
      origin = UavPersonAssociationOrigin{key, mapping.lastSeenUs, mapping.active};
    }
  }
  // An earlier witness cannot bypass a later mapping's global conflict veto.
  return origin;
}

UavPersonWindowResult
UavPersonTrackingEngine::processValidatedWindow(const UavPersonWindowContext& context,
                                               const UavPersonBackendWindow& backend)
{
  if (!m_binding) {
    if (context.sequence != 1) reject("INITIAL_SEQUENCE_REQUIRED");
  }
  else {
    checkContinuation(context, *m_binding, m_watermarkUs);
  }
  UavPersonWindowResult result;
  result.context = context;
  std::array<size_t, 3> order{0, 1, 2};
  std::sort(order.begin(), order.end(), [&context] (size_t a, size_t b) {
    return std::tie(context.frames[a].commonPtsUs, context.frames[a].cameraId) <
      std::tie(context.frames[b].commonPtsUs, context.frames[b].cameraId);
  });
  const auto margin = static_cast<double>(m_profile.limits.at("ambiguityMarginPpm")) / 1e6;
  const auto maximum = static_cast<double>(m_profile.limits.at("maxCosineDistancePpm")) / 1e6;
  const auto cosineDistance = [] (const std::array<double, 512>& prototype,
                                  const std::array<float, 512>& embedding) {
    double dot = 0;
    for (size_t i = 0; i < prototype.size(); ++i) dot += prototype[i] * embedding[i];
    return 1.0 - std::clamp(dot, -1.0, 1.0);
  };
  for (size_t begin = 0; begin < order.size();) {
    const auto pts = context.frames[order[begin]].commonPtsUs;
    size_t end = begin + 1;
    while (end < order.size() && context.frames[order[end]].commonPtsUs == pts) ++end;
    expireAt(pts);
    // All same-time ownership changes are known before any candidate is matched.
    for (size_t position = begin; position < end; ++position) {
      const auto camera = order[position];
      for (auto& [key, mapping] : m_localToGlobal) {
        if (key.cameraId == context.frames[camera].cameraId) mapping.active = false;
      }
    }
    std::map<UavPersonLocalKey, uint64_t> appearanceFailures;
    std::set<uint64_t> validExistingIds;
    for (size_t position = begin; position < end; ++position) {
      const auto camera = order[position];
      for (const auto& detection : backend.frames[camera].detections) {
        if (!detection.active) continue;
        const auto found = m_localToGlobal.find(detection.key);
        if (found == m_localToGlobal.end()) continue;
        const auto id = found->second.globalId;
        const auto mean = prototype(m_gallery.at(id));
        if (detection.embedding && mean &&
            cosineDistance(*mean, *detection.embedding) > maximum) {
          appearanceFailures.emplace(detection.key, id);
        }
        else {
          validExistingIds.insert(id);
        }
      }
    }
    std::set<uint64_t> rejectedIds;
    std::map<uint64_t, size_t> failedKeyCounts;
    for (const auto& failure : appearanceFailures) {
      const auto id = failure.second;
      rejectedIds.insert(id);
      ++failedKeyCounts[id];
    }
    std::set<UavPersonLocalKey> preservableFailures;
    for (const auto& [key, id] : appearanceFailures) {
      if (!validExistingIds.count(id) && failedKeyCounts.at(id) == 1) {
        preservableFailures.insert(key);
      }
    }
    for (size_t position = begin; position < end; ++position) {
      const auto camera = order[position];
      for (const auto& detection : backend.frames[camera].detections) {
        if (!detection.active) continue;
        if (appearanceFailures.count(detection.key)) continue;
        const auto found = m_localToGlobal.find(detection.key);
        if (found == m_localToGlobal.end()) continue;
        const auto id = found->second.globalId;
        observe(detection, id);
        auto row = decisionRow(detection);
        row.globalId = id;
        row.associationStatus = "CONTINUED";
        result.tracks[camera].push_back(std::move(row));
      }
    }
    std::map<uint64_t, std::set<std::string>> activeCameras;
    for (const auto& [key, mapping] : m_localToGlobal) {
      if (!mapping.active) continue;
      if (!activeCameras[mapping.globalId].insert(key.cameraId).second) {
        reject("SAME_CAMERA_GLOBAL_ID_CONFLICT");
      }
    }
    for (const auto& [id, cameras] : activeCameras) {
      for (auto first = cameras.begin(); first != cameras.end(); ++first) {
        for (auto second = std::next(first); second != cameras.end(); ++second) {
          if (!m_profile.overlappingCameras.at({*first, *second})) {
            reject("NON_OVERLAP_ACTIVE_CONFLICT");
          }
        }
      }
    }
    for (size_t position = begin; position < end; ++position) {
      const auto camera = order[position];
      std::vector<const UavPersonDetection*> fresh;
      for (const auto& detection : backend.frames[camera].detections) {
        if (!detection.active) result.tracks[camera].push_back(decisionRow(detection));
        else if (!m_localToGlobal.count(detection.key) || appearanceFailures.count(detection.key)) {
          fresh.push_back(&detection);
        }
      }
      std::sort(fresh.begin(), fresh.end(), [] (const auto* a, const auto* b) {
        return a->key < b->key;
      });
      using Edge = std::pair<double, uint64_t>;
      std::vector<std::vector<Edge>> edges(fresh.size());
      std::map<uint64_t, std::vector<std::pair<double, size_t>>> reverse;
      for (size_t index = 0; index < fresh.size(); ++index) {
        const auto& detection = *fresh[index];
        if (!detection.embedding) continue;
        const auto failure = appearanceFailures.find(detection.key);
        for (const auto& [id, identity] : m_gallery) {
          if (failure != appearanceFailures.end() && id == failure->second) continue;
          if (!eligibilityOrigin(id, detection, context)) continue;
          const auto mean = prototype(identity);
          if (!mean) continue;
          const auto distance = cosineDistance(*mean, *detection.embedding);
          if (distance > maximum) continue;
          edges[index].emplace_back(distance, id);
          reverse[id].emplace_back(distance, index);
        }
        std::sort(edges[index].begin(), edges[index].end());
      }
      for (auto& [id, contenders] : reverse) std::sort(contenders.begin(), contenders.end());
      std::vector<UavPersonTrack> decisions;
      std::set<uint64_t> matchedIdsInCamera;
      for (size_t index = 0; index < fresh.size(); ++index) {
        auto row = decisionRow(*fresh[index]);
        row.associationStatus = fresh[index]->embedding ? "NO_CANDIDATE" : "NO_FEATURE";
        if (!edges[index].empty() && edges[index][0].first <= maximum) {
          const auto [distance, id] = edges[index][0];
          const auto& contenders = reverse.at(id);
          const bool forwardAmbiguous = edges[index].size() > 1 &&
            (edges[index][1].first == distance || edges[index][1].first - distance < margin);
          const bool reverseAmbiguous = contenders.size() > 1 &&
            (contenders[1].first == contenders[0].first ||
             contenders[1].first - contenders[0].first < margin);
          row.distance = distance;
          if (forwardAmbiguous || reverseAmbiguous) row.associationStatus = "AMBIGUOUS";
          else if (contenders[0].second != index) row.associationStatus = "COMPETITION";
          else {
            row.globalId = id;
            row.associationStatus = "MATCHED";
            row.associationOrigin = eligibilityOrigin(id, *fresh[index], context);
            matchedIdsInCamera.insert(id);
          }
        }
        else if (fresh[index]->embedding && std::any_of(context.frames.begin(), context.frames.end(),
                   [] (const auto& frame) { return !frame.clockAvailable; })) {
          row.associationStatus = "CLOCK_UNAVAILABLE";
        }
        decisions.push_back(std::move(row));
      }
      // Insert after this camera's edges are frozen, never greedily consume its own new IDs.
      for (size_t index = 0; index < fresh.size(); ++index) {
        auto& row = decisions[index];
        const auto failure = appearanceFailures.find(fresh[index]->key);
        if (failure != appearanceFailures.end()) {
          const auto oldId = failure->second;
          if (row.associationStatus == "MATCHED") {
            m_localToGlobal.erase(fresh[index]->key);
          }
          else {
            const auto failedKey = fresh[index]->key;
            const auto failedMapping = m_localToGlobal.find(failedKey);
            bool hasActiveOwner = false;
            for (auto owner = m_localToGlobal.cbegin(); owner != m_localToGlobal.cend(); ++owner) {
              if (owner != failedMapping && owner->second.globalId == oldId &&
                  owner->second.active) {
                hasActiveOwner = true;
                break;
              }
            }
            if (preservableFailures.count(fresh[index]->key) &&
                !matchedIdsInCamera.count(oldId) && !hasActiveOwner) {
              const auto mapping = m_localToGlobal.find(fresh[index]->key);
              const auto identity = m_gallery.find(oldId);
              if (mapping == m_localToGlobal.end() || mapping->second.globalId != oldId ||
                  identity == m_gallery.end()) {
                reject("LOCAL_IDENTITY_MISSING");
              }
              mapping->second.lastSeenUs = fresh[index]->commonPtsUs;
              mapping->second.active = true;
              identity->second.lastSeenUs = fresh[index]->commonPtsUs;
              row.globalId = oldId;
              result.tracks[camera].push_back(std::move(row));
              continue;
            }
            m_localToGlobal.erase(fresh[index]->key);
          }
        }
        if (!row.globalId) row.globalId = allocateIdentity(pts);
        observe(*fresh[index], *row.globalId);
        result.tracks[camera].push_back(std::move(row));
      }
    }
    for (const auto id : rejectedIds) {
      if (validExistingIds.count(id)) continue;
      const auto identity = m_gallery.find(id);
      if (identity == m_gallery.end()) reject("LOCAL_IDENTITY_MISSING");
      const auto lastSeenUs = identity->second.lastSeenUs;
      const auto hasLatestOwner = std::any_of(m_localToGlobal.begin(), m_localToGlobal.end(),
        [id, lastSeenUs] (const auto& item) {
          return item.second.globalId == id && item.second.lastSeenUs == lastSeenUs;
        });
      if (hasLatestOwner) continue;
      for (auto it = m_localToGlobal.begin(); it != m_localToGlobal.end();) {
        if (it->second.globalId == id) it = m_localToGlobal.erase(it);
        else ++it;
      }
      m_gallery.erase(identity);
    }
    m_watermarkUs = pts;
    begin = end;
  }
  for (auto& rows : result.tracks) {
    std::stable_sort(rows.begin(), rows.end(), [] (const auto& a, const auto& b) {
      return a.key < b.key;
    });
  }
  m_binding = context;
  m_backendTrackerState = backend.backendStateJson;
  result.gallerySize = m_gallery.size();
  result.localKeyCount = m_localToGlobal.size();
  result.watermarkUs = m_watermarkUs;
  result.candidateStateJson = serializeState();
  return result;
}

UavPersonWindowResult
UavPersonTrackingEngine::processWindow(const UavPersonWindowContext& context,
                                      const UavPersonBackendWindow& backend)
{
  validateAdmittedWindow(context, backend, m_profile);
  auto candidate = *this;
  auto result = candidate.processValidatedWindow(context, backend);
  swapState(candidate);
  return result;
}

void
UavPersonTrackingEngine::swapState(UavPersonTrackingEngine& candidate) noexcept
{
  m_binding.swap(candidate.m_binding);
  m_gallery.swap(candidate.m_gallery);
  m_localToGlobal.swap(candidate.m_localToGlobal);
  m_backendTrackerState.swap(candidate.m_backendTrackerState);
  std::swap(m_nextGlobalId, candidate.m_nextGlobalId);
  std::swap(m_watermarkUs, candidate.m_watermarkUs);
}

const std::string&
UavPersonTrackingEngine::backendStateJson() const noexcept
{
  return m_backendTrackerState;
}

std::string
UavPersonTrackingEngine::serializeState() const
{
  if (!m_binding) reject("NO_INITIALIZED_STATE");
  Json gallery = Json::array();
  for (const auto& [id, identity] : m_gallery) {
    Json samples = Json::array();
    for (const auto& sample : identity.samples) {
      samples.push_back({{"ptsUs", sample.ptsUs}, {"embedding", sample.embedding}});
    }
    gallery.push_back({{"globalId", id}, {"lastSeenUs", identity.lastSeenUs},
                        {"samples", std::move(samples)}});
  }
  Json locals = Json::array();
  for (const auto& [key, mapping] : m_localToGlobal) {
    locals.push_back({{"key", localKeyJson(key)}, {"globalId", mapping.globalId},
                       {"lastSeenUs", mapping.lastSeenUs}, {"active", mapping.active}});
  }
  Json state = {{"schema", "uav-person-session-v1"}, {"profileDigest", m_profile.profileDigest},
    {"binding", contextJson(*m_binding)}, {"nextGlobalId", m_nextGlobalId},
    {"watermarkUs", m_watermarkUs}, {"gallery", std::move(gallery)},
    {"localMappings", std::move(locals)},
    {"backendState", parseBounded(m_backendTrackerState, m_profile.limits.at("maxSnapshotBytes"))}};
  auto wire = state.dump();
  if (wire.size() > m_profile.limits.at("maxSnapshotBytes")) reject("SNAPSHOT_BYTE_LIMIT");
  return wire;
}

void
UavPersonTrackingEngine::restoreState(const std::string& stateJson,
                                     const UavPersonWindowContext& expected)
{
  checkContext(expected, m_profile);
  const auto wire = parseBounded(stateJson, m_profile.limits.at("maxSnapshotBytes"));
  exactKeys(wire, {"schema", "profileDigest", "binding", "nextGlobalId", "watermarkUs",
                   "gallery", "localMappings", "backendState"});
  if (text(wire.at("schema")) != "uav-person-session-v1" ||
      text(wire.at("profileDigest")) != m_profile.profileDigest) reject("snapshot schema/profile");
  UavPersonTrackingEngine candidate(m_profile);
  candidate.m_binding = parseContext(wire.at("binding"), m_profile);
  candidate.m_nextGlobalId = unsignedNumber(wire.at("nextGlobalId"));
  if (candidate.m_nextGlobalId == 0) reject("snapshot next ID");
  candidate.m_watermarkUs = static_cast<int64_t>(unsignedNumber(wire.at("watermarkUs"),
    std::numeric_limits<int64_t>::max()));
  int64_t maximumPts = 0;
  for (const auto& frame : candidate.m_binding->frames) {
    maximumPts = std::max(maximumPts, frame.commonPtsUs);
  }
  if (candidate.m_watermarkUs != maximumPts) reject("snapshot watermark");
  checkContinuation(expected, *candidate.m_binding, candidate.m_watermarkUs);
  const auto ttl = static_cast<int64_t>(m_profile.limits.at("galleryTtlUs"));
  const auto cutoff = maximumPts > ttl ? maximumPts - ttl : 0;
  const auto readTime = [cutoff, maximumPts] (const Json& value) {
    const auto time = static_cast<int64_t>(unsignedNumber(value,
      std::numeric_limits<int64_t>::max()));
    if (time < cutoff || time > maximumPts) reject("snapshot time range");
    return time;
  };
  const auto& gallery = wire.at("gallery");
  if (!gallery.is_array() || gallery.size() > m_profile.limits.at("maxGlobalIdentities")) {
    reject("snapshot gallery count");
  }
  for (const auto& row : gallery) {
    exactKeys(row, {"globalId", "lastSeenUs", "samples"});
    const auto id = unsignedNumber(row.at("globalId"));
    if (id == 0 || id >= candidate.m_nextGlobalId) reject("snapshot global ID");
    Identity identity;
    identity.lastSeenUs = readTime(row.at("lastSeenUs"));
    const auto& samples = row.at("samples");
    if (!samples.is_array() || samples.size() > m_profile.limits.at("samplesPerIdentity")) {
      reject("snapshot sample count");
    }
    int64_t lastSample = cutoff;
    for (const auto& sample : samples) {
      exactKeys(sample, {"ptsUs", "embedding"});
      const auto pts = readTime(sample.at("ptsUs"));
      if (pts < lastSample || pts > identity.lastSeenUs) reject("snapshot sample order");
      identity.samples.push_back({pts, unitEmbedding(sample.at("embedding"))});
      lastSample = pts;
    }
    if (!candidate.m_gallery.emplace(id, std::move(identity)).second) reject("snapshot duplicate ID");
  }
  const auto& locals = wire.at("localMappings");
  if (!locals.is_array() || locals.size() > m_profile.limits.at("maxLocalKeys")) {
    reject("snapshot local-key count");
  }
  std::map<uint64_t, int64_t> lastSeen;
  std::map<uint64_t, std::set<std::string>> activeCameras;
  std::array<std::set<uint64_t>, 3> activeIds;
  for (const auto& row : locals) {
    exactKeys(row, {"key", "globalId", "lastSeenUs", "active"});
    const auto key = parseLocalKey(row.at("key"), *candidate.m_binding);
    const auto id = unsignedNumber(row.at("globalId"));
    const auto identity = candidate.m_gallery.find(id);
    if (identity == candidate.m_gallery.end()) reject("snapshot dangling mapping");
    const auto pts = readTime(row.at("lastSeenUs"));
    if (pts > identity->second.lastSeenUs) reject("snapshot local time");
    const auto active = boolean(row.at("active"));
    if (!candidate.m_localToGlobal.emplace(key, LocalMapping{id, pts, active}).second) {
      reject("snapshot duplicate local key");
    }
    lastSeen[id] = std::max(lastSeen[id], pts);
    if (active) {
      if (!activeCameras[id].insert(key.cameraId).second) reject("snapshot same-camera conflict");
      const auto index = static_cast<size_t>(key.cameraId.back() - '1');
      if (pts != candidate.m_binding->frames[index].commonPtsUs) reject("snapshot active time");
      activeIds[index].insert(key.byteTrackId);
    }
  }
  for (const auto& [id, identity] : candidate.m_gallery) {
    if (!lastSeen.count(id) || lastSeen.at(id) != identity.lastSeenUs) {
      reject("snapshot identity/local correspondence");
    }
  }
  for (const auto& [id, cameras] : activeCameras) {
    for (auto first = cameras.begin(); first != cameras.end(); ++first) {
      for (auto second = std::next(first); second != cameras.end(); ++second) {
        if (!m_profile.overlappingCameras.at({*first, *second})) reject("snapshot overlap conflict");
      }
    }
  }
  const auto& backend = wire.at("backendState");
  const auto format = backendStateFormat(backend);
  exactKeys(backend.at("cameras"), {"UAV1", "UAV2", "UAV3"});
  for (size_t i = 0; i < 3; ++i) {
    const auto& frame = candidate.m_binding->frames[i];
    const auto& state = backend.at("cameras").at(frame.cameraId);
    exactKeys(state, {"sourceId", "sourceEpoch", "trackerGeneration", "tracker"});
    checkSource(state, frame);
    const auto tracker = checkTracker(state.at("tracker"),
                                      m_profile.limits.at("maxBackendTracksPerCamera"), format);
    if (frame.commonPtsUs >= cutoff && activeIds[i] != tracker.currentIds) {
      reject("snapshot active tracker correspondence");
    }
  }
  candidate.m_backendTrackerState = backend.dump();
  // Every field, including the last backend state, is admitted before any owned state changes.
  swapState(candidate);
}

} // namespace ndnsf::examples::uav
