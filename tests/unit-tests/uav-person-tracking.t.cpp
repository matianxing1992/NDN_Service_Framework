#include "tests/boost-test.hpp"
#include "tests/fixtures/uav-person-removal-capture.hpp"
#include "tests/fixtures/uav-person-window.hpp"

#include "NDNSF-UAV-APP/shared/UavDigest.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-tracking.hpp"
#include "NDNSF-DistributedInference/cpp/vendor/nlohmann/json.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf::test {
namespace {

using namespace examples::uav;
using Json = nlohmann::json;

using namespace person_fixture;

Json
currentTrackFixture()
{
  auto wire = compactBackendFixture();
  const auto context = atWindow(2, 1500000);
  wire["windowId"] = context.windowId;
  wire["sequence"] = context.sequence;
  wire["backendState"]["schema"] = "uav-person-backend-state-v3";
  for (size_t i = 0; i < context.frames.size(); ++i) {
    wire["frames"][i]["sequence"] = context.sequence;
    wire["frames"][i]["commonPtsUs"] = context.frames[i].commonPtsUs;
    auto& tracker = wire["backendState"]["cameras"][context.frames[i].cameraId]["tracker"];
    tracker["frameId"] = 2;
    tracker["tracked"][0]["frameId"] = 2;
  }
  auto newcomer = stockTrack(8);
  newcomer["isActivated"] = false;
  newcomer["startFrame"] = 2;
  newcomer["frameId"] = 2;
  newcomer["index"] = 1.0;
  newcomer["tlwh"] = {80.0, 20.0, 40.0, 100.0};
  newcomer["mean"][0] = 100.0;
  auto& tracker = wire["backendState"]["cameras"]["UAV1"]["tracker"];
  tracker["tracked"].push_back(std::move(newcomer));
  tracker["idHighWatermark"] = 8;
  auto detection = person(8, 0.0, 1.0);
  detection["box"] = {80, 20, 120, 120};
  wire["frames"][0]["detections"].push_back(std::move(detection));
  return wire;
}

} // namespace

BOOST_AUTO_TEST_SUITE(UavPersonBackendAdmission)

BOOST_AUTO_TEST_CASE(CurrentStockIdsContinueAcrossConfirmationAndSnapshotRestore)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto initial = windowContext();
  const auto second = atWindow(2, 1500000);
  const auto third = atWindow(3, 2000000);
  auto firstWire = compactBackendFixture();
  firstWire["backendState"]["schema"] = "uav-person-backend-state-v3";
  const auto firstBackend = parseUavPersonBackendWindow(firstWire.dump(), initial, profile);
  UavPersonTrackingEngine continuous(profile);
  const auto first = continuous.processWindow(initial, firstBackend);
  UavPersonTrackingEngine resumed(profile);
  resumed.restoreState(first.candidateStateJson, second);
  auto wire = currentTrackFixture();
  const auto backend = parseUavPersonBackendWindow(wire.dump(), second, profile);
  const auto observed = continuous.processWindow(second, backend);
  const auto replayed = resumed.processWindow(second, backend);
  BOOST_CHECK_EQUAL(replayed.candidateStateJson, observed.candidateStateJson);
  uint64_t newcomerId = 0;
  for (const auto& row : observed.tracks[0]) {
    if (row.key.byteTrackId == 8) {
      BOOST_REQUIRE(row.globalId.has_value());
      newcomerId = *row.globalId;
    }
  }
  BOOST_REQUIRE_NE(newcomerId, 0);
  auto snapshot = Json::parse(observed.candidateStateJson);
  BOOST_CHECK(!snapshot["backendState"]["cameras"]["UAV1"]["tracker"]["tracked"][1]
    ["isActivated"].get<bool>());
  UavPersonTrackingEngine afterArrival(profile);
  afterArrival.restoreState(observed.candidateStateJson, third);
  BOOST_CHECK_EQUAL(afterArrival.serializeState(), observed.candidateStateJson);
  wire["windowId"] = third.windowId;
  wire["sequence"] = third.sequence;
  for (size_t i = 0; i < third.frames.size(); ++i) {
    wire["frames"][i]["sequence"] = third.sequence;
    wire["frames"][i]["commonPtsUs"] = third.frames[i].commonPtsUs;
    auto& tracker = wire["backendState"]["cameras"][third.frames[i].cameraId]["tracker"];
    tracker["frameId"] = 3;
    for (auto& track : tracker["tracked"]) {
      track["frameId"] = 3;
      track["isActivated"] = true;
    }
  }
  const auto confirmed = parseUavPersonBackendWindow(wire.dump(), third, profile);
  const auto result = continuous.processWindow(third, confirmed);
  BOOST_CHECK_EQUAL(afterArrival.processWindow(third, confirmed).candidateStateJson,
                    result.candidateStateJson);
  bool found = false;
  for (const auto& row : result.tracks[0]) {
    if (row.key.byteTrackId == 8) {
      found = true;
      BOOST_REQUIRE(row.globalId.has_value());
      BOOST_CHECK_EQUAL(*row.globalId, newcomerId);
      BOOST_CHECK_EQUAL(row.associationStatus, "CONTINUED");
    }
  }
  BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(CurrentStockMalformedIdentityCannotAdvanceAnyNativeEntry)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto initial = windowContext();
  const auto second = atWindow(2, 1500000);
  auto firstWire = compactBackendFixture();
  firstWire["backendState"]["schema"] = "uav-person-backend-state-v3";
  const auto firstBackend = parseUavPersonBackendWindow(firstWire.dump(), initial, profile);
  const auto baseline = currentTrackFixture();
  const auto admitted = parseUavPersonBackendWindow(baseline.dump(), second, profile);
  UavPersonTrackingEngine reference(profile);
  reference.processWindow(initial, firstBackend);
  const auto original = reference.processWindow(second, admitted).candidateStateJson;
  const std::vector<std::function<void(Json&)>> mutations = {
    [] (auto& t) { t["tracked"][1]["frameId"] = 1; t["tracked"][1]["startFrame"] = 1; },
    [] (auto& t) { t["tracked"][1]["state"] = 2; },
    [] (auto& t) { t["tracked"][1]["trackId"] = 0; },
    [] (auto& t) { t["tracked"][1]["trackId"] = 9; },
    [] (auto& t) { t["tracked"][1]["trackId"] = 7; },
    [] (auto& t) { t["tracked"][1]["isActivated"] = "false"; },
    [] (auto& t) { t["tracked"].erase(t["tracked"].begin() + 1); },
    [] (auto& t) { t["lost"].push_back(stockTrack(8, 2)); },
  };
  for (size_t i = 0; i < mutations.size(); ++i) {
    auto wire = baseline;
    mutations[i](wire["backendState"]["cameras"]["UAV1"]["tracker"]);
    BOOST_TEST_CONTEXT("current stock identity mutation " << i) {
      BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), second, profile),
                        std::invalid_argument);
      auto bad = admitted;
      bad.backendStateJson = wire["backendState"].dump();
      UavPersonTrackingEngine engine(profile);
      engine.processWindow(initial, firstBackend);
      const auto before = engine.serializeState();
      BOOST_CHECK_THROW(engine.processWindow(second, bad), std::invalid_argument);
      BOOST_CHECK_EQUAL(engine.serializeState(), before);
      BOOST_CHECK_EQUAL(engine.processWindow(second, admitted).candidateStateJson, original);
      auto snapshot = Json::parse(original);
      snapshot["backendState"] = wire["backendState"];
      BOOST_CHECK_THROW(engine.restoreState(snapshot.dump(), atWindow(3, 2000000)),
                        std::invalid_argument);
      BOOST_CHECK_EQUAL(engine.serializeState(), original);
    }
  }
  for (const bool duplicate : {false, true}) {
    auto wire = baseline;
    auto bad = admitted;
    if (duplicate) {
      wire["frames"][0]["detections"].push_back(wire["frames"][0]["detections"][1]);
      bad.frames[0].detections.push_back(bad.frames[0].detections[1]);
    }
    else {
      wire["frames"][0]["detections"].erase(wire["frames"][0]["detections"].begin() + 1);
      bad.frames[0].detections.erase(bad.frames[0].detections.begin() + 1);
    }
    BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), second, profile),
                      std::invalid_argument);
    UavPersonTrackingEngine engine(profile);
    engine.processWindow(initial, firstBackend);
    const auto before = engine.serializeState();
    BOOST_CHECK_THROW(engine.processWindow(second, bad), std::invalid_argument);
    BOOST_CHECK_EQUAL(engine.serializeState(), before);
  }
}

BOOST_AUTO_TEST_CASE(CurrentStockFormatPreservesLegacyConfirmationAndUnownedRows)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto initial = windowContext();
  const auto second = atWindow(2, 1500000);
  auto firstWire = compactBackendFixture();
  firstWire["backendState"]["schema"] = "uav-person-backend-state-v3";
  const auto firstBackend = parseUavPersonBackendWindow(firstWire.dump(), initial, profile);
  const auto baseline = currentTrackFixture();
  const auto admitted = parseUavPersonBackendWindow(baseline.dump(), second, profile);
  UavPersonTrackingEngine reference(profile);
  reference.processWindow(initial, firstBackend);
  const auto original = reference.processWindow(second, admitted).candidateStateJson;
  for (const auto* schema : {"uav-person-backend-state-v1", "uav-person-backend-state-v2"}) {
    auto old = baseline;
    old["backendState"]["schema"] = schema;
    if (std::string(schema) == "uav-person-backend-state-v1") {
      for (auto& camera : old["backendState"]["cameras"].items()) {
        auto& tracker = camera.value()["tracker"];
        tracker.erase("removedIds");
        tracker.erase("idHighWatermark");
        tracker["removed"] = Json::array();
      }
    }
    BOOST_CHECK_THROW(parseUavPersonBackendWindow(old.dump(), second, profile), std::invalid_argument);
    auto bad = admitted;
    bad.backendStateJson = old["backendState"].dump();
    UavPersonTrackingEngine engine(profile);
    engine.processWindow(initial, firstBackend);
    const auto before = engine.serializeState();
    BOOST_CHECK_THROW(engine.processWindow(second, bad), std::invalid_argument);
    BOOST_CHECK_EQUAL(engine.serializeState(), before);
    auto snapshot = Json::parse(original);
    snapshot["backendState"] = old["backendState"];
    BOOST_CHECK_THROW(reference.restoreState(snapshot.dump(), atWindow(3, 2000000)),
                      std::invalid_argument);
    BOOST_CHECK_EQUAL(reference.serializeState(), original);
    // Legacy formats may retain the unconfirmed state, but cannot expose its ID yet.
    old["frames"][0]["detections"].erase(old["frames"][0]["detections"].begin() + 1);
    const auto legacy = parseUavPersonBackendWindow(old.dump(), second, profile);
    BOOST_CHECK_NO_THROW(engine.processWindow(second, legacy));
  }
  auto unowned = baseline;
  auto detection = person(9);
  detection["byteTrackId"] = nullptr;
  detection["active"] = false;
  detection["embedding"] = nullptr;
  detection["confidence"] = 0.2;
  unowned["frames"][0]["detections"].push_back(std::move(detection));
  UavPersonTrackingEngine engine(profile);
  engine.processWindow(initial, firstBackend);
  const auto result = engine.processWindow(second,
    parseUavPersonBackendWindow(unowned.dump(), second, profile));
  bool found = false;
  for (const auto& row : result.tracks[0]) {
    if (row.key.byteTrackId == 0) {
      found = true;
      BOOST_CHECK(!row.globalId);
      BOOST_CHECK_EQUAL(row.associationStatus, "UNTRACKED");
    }
  }
  BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(CompactHistoryRoundTripRetainsOrderDuplicatesAndAllocator)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  auto wire = compactBackendFixture();
  auto& tracker = wire["backendState"]["cameras"]["UAV1"]["tracker"];
  tracker["removedIds"] = {1, 1, 6, 7};
  tracker["idHighWatermark"] = 23;
  tracker["lost"] = {stockTrack(6, 3)};
  wire["frames"][0]["lostByteTrackIds"] = {6};
  const auto context = windowContext();
  const auto backend = parseUavPersonBackendWindow(wire.dump(), context, profile);
  UavPersonTrackingEngine engine(profile);
  const auto first = engine.processWindow(context, backend);
  const auto next = atWindow(2, 1500000);
  UavPersonTrackingEngine resumed(profile);
  resumed.restoreState(first.candidateStateJson, next);
  BOOST_CHECK_EQUAL(resumed.serializeState(), engine.serializeState());
  BOOST_CHECK_EQUAL(resumed.backendStateJson(), backend.backendStateJson);
  const auto restored = Json::parse(resumed.backendStateJson());
  BOOST_CHECK(restored["cameras"]["UAV1"]["tracker"]["removedIds"] == Json({1, 1, 6, 7}));
  BOOST_CHECK_EQUAL(restored["cameras"]["UAV1"]["tracker"]["idHighWatermark"], 23);
}

BOOST_AUTO_TEST_CASE(CompactHistoryEnforces512FullRecordsAnd1000ScalarHistoryIds)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  auto wire = compactBackendFixture();
  auto& tracker = wire["backendState"]["cameras"]["UAV1"]["tracker"];
  tracker["idHighWatermark"] = 519;
  tracker["removedIds"] = std::vector<uint64_t>(1000, 1);
  for (uint64_t id = 8; id < 519; ++id) {
    tracker["lost"].push_back(stockTrack(id, 2));
    wire["frames"][0]["lostByteTrackIds"].push_back(id);
  }
  const auto context = windowContext();
  const auto backend = parseUavPersonBackendWindow(wire.dump(), context, profile);
  UavPersonTrackingEngine engine(profile);
  BOOST_CHECK_NO_THROW(engine.processWindow(context, backend));
  const auto original = engine.serializeState();
  tracker["lost"].push_back(stockTrack(519, 2));
  wire["frames"][0]["lostByteTrackIds"].push_back(519);
  BOOST_CHECK_EXCEPTION(parseUavPersonBackendWindow(wire.dump(), context, profile),
    std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()).find("BACKEND_TRACK_LIMIT") != std::string::npos;
    });
  auto bad = backend;
  bad.backendStateJson = wire["backendState"].dump();
  UavPersonTrackingEngine fresh(profile);
  BOOST_CHECK_THROW(fresh.processWindow(context, bad), std::invalid_argument);
  BOOST_CHECK_EQUAL(fresh.processWindow(context, backend).candidateStateJson, original);
  auto snapshot = Json::parse(original);
  snapshot["backendState"] = wire["backendState"];
  BOOST_CHECK_THROW(engine.restoreState(snapshot.dump(), atWindow(2, 1500000)), std::invalid_argument);
  BOOST_CHECK_EQUAL(engine.serializeState(), original);
  wire = compactBackendFixture();
  wire["backendState"]["cameras"]["UAV1"]["tracker"]["removedIds"] =
    std::vector<uint64_t>(1001, 1);
  BOOST_CHECK_EXCEPTION(parseUavPersonBackendWindow(wire.dump(), context, profile),
    std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()).find("BACKEND_REMOVAL_HISTORY_LIMIT") != std::string::npos;
    });
}

BOOST_AUTO_TEST_CASE(CompactHistoryMalformedIngressProcessingAndRestoreCannotAdvanceState)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto context = windowContext();
  const auto baseline = compactBackendFixture();
  const auto admitted = parseUavPersonBackendWindow(baseline.dump(), context, profile);
  UavPersonTrackingEngine reference(profile);
  const auto first = reference.processWindow(context, admitted);
  const auto next = atWindow(2, 1500000);
  const std::vector<std::function<void(Json&)>> mutations = {
    [] (auto& t) { t["idHighWatermark"] = true; },
    [] (auto& t) { t["idHighWatermark"] = -1; },
    [] (auto& t) { t["idHighWatermark"] = 7.5; },
    [] (auto& t) { t["idHighWatermark"] = 6; },
    [] (auto& t) { t["idHighWatermark"] = 16777215; },
    [] (auto& t) { t.erase("idHighWatermark"); },
    [] (auto& t) { t["removedIds"] = {0}; },
    [] (auto& t) { t["removedIds"] = {-1}; },
    [] (auto& t) { t["removedIds"] = {true}; },
    [] (auto& t) { t["removedIds"] = {1.5}; },
    [] (auto& t) { t["removedIds"] = {8}; },
    [] (auto& t) { t["removedIds"] = "not-a-list"; },
    [] (auto& t) { t["removedIds"] = std::vector<uint64_t>(1001, 1); },
    [] (auto& t) { t["removed"] = Json::array(); },
    [] (auto& t) { t["tracked"][0]["mean"] = {1.0}; },
    [] (auto& t) { t["frameId"] = true; },
  };
  for (size_t index = 0; index < mutations.size(); ++index) {
    auto wire = baseline;
    mutations[index](wire["backendState"]["cameras"]["UAV1"]["tracker"]);
    BOOST_TEST_CONTEXT("compact mutation " << index) {
      BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), context, profile),
                        std::invalid_argument);
      auto bad = admitted;
      bad.backendStateJson = wire["backendState"].dump();
      UavPersonTrackingEngine fresh(profile);
      BOOST_CHECK_THROW(fresh.processWindow(context, bad), std::invalid_argument);
      BOOST_CHECK_EQUAL(fresh.processWindow(context, admitted).candidateStateJson,
                        first.candidateStateJson);
      auto snapshot = Json::parse(first.candidateStateJson);
      snapshot["backendState"] = wire["backendState"];
      BOOST_CHECK_THROW(reference.restoreState(snapshot.dump(), next), std::invalid_argument);
      BOOST_CHECK_EQUAL(reference.serializeState(), first.candidateStateJson);
    }
  }
  auto unknown = baseline;
  unknown["backendState"]["schema"] = "uav-person-backend-state-v99";
  BOOST_CHECK_THROW(parseUavPersonBackendWindow(unknown.dump(), context, profile),
                    std::invalid_argument);
  auto bad = admitted;
  bad.backendStateJson = unknown["backendState"].dump();
  UavPersonTrackingEngine fresh(profile);
  BOOST_CHECK_THROW(fresh.processWindow(context, bad), std::invalid_argument);
  auto snapshot = Json::parse(first.candidateStateJson);
  snapshot["backendState"] = unknown["backendState"];
  BOOST_CHECK_THROW(reference.restoreState(snapshot.dump(), next), std::invalid_argument);
  BOOST_CHECK_EQUAL(reference.serializeState(), first.candidateStateJson);
}

BOOST_AUTO_TEST_CASE(AcceptsExactCapturedStockRemovalHistoryAgainstIndependentNativeInput)
{
  const std::string captured = CAPTURED_REMOVAL_BACKEND;
  ndn::util::Sha256 digest;
  digest << captured;
  BOOST_CHECK_EQUAL(canonicalSha256Hex(digest.toString()), CAPTURED_REMOVAL_BACKEND_SHA256);
  const auto profile = parseUavPersonTrackingProfile(CAPTURED_REMOVAL_PROFILE);
  const auto admitted = parseUavPersonBackendWindow(captured, capturedRemovalContext(), profile);
  BOOST_CHECK_EQUAL(admitted.context.sequence, 66);
  const auto state = Json::parse(admitted.backendStateJson);
  std::vector<Json> repeated;
  for (const auto& track : state["cameras"]["UAV1"]["tracker"]["removed"]) {
    if (track["trackId"] == 3) repeated.push_back(track);
  }
  BOOST_REQUIRE_EQUAL(repeated.size(), 2);
  BOOST_CHECK(repeated[0]["mean"] != repeated[1]["mean"]);
  BOOST_CHECK(repeated[0]["covariance"] != repeated[1]["covariance"]);
}

BOOST_AUTO_TEST_CASE(RemovalHistoryRetainsRepeatedPredictionsAcrossNativeSnapshotRestore)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  auto wire = backendFixture();
  const auto removed = stockTrack(23, 3);
  wire["backendState"]["cameras"]["UAV1"]["tracker"]["removed"] = {removed, removed};
  // Distinct predictions are valid historical entries for the same removed track.
  wire["backendState"]["cameras"]["UAV1"]["tracker"]["removed"][1]["mean"][0] = 31.0;
  const auto context = windowContext();
  const auto backend = parseUavPersonBackendWindow(wire.dump(), context, profile);
  UavPersonTrackingEngine engine(profile);
  const auto first = engine.processWindow(context, backend);
  BOOST_CHECK_EQUAL(Json::parse(engine.backendStateJson())["cameras"]["UAV1"]
    ["tracker"]["removed"].size(), 2);
  const auto nextContext = atWindow(2, 1500000);
  UavPersonTrackingEngine resumed(profile);
  resumed.restoreState(first.candidateStateJson, nextContext);
  BOOST_CHECK_EQUAL(resumed.backendStateJson(), engine.backendStateJson());
  const auto next = resumed.processWindow(nextContext,
    admitWindow(nextContext, profile, {{{person(7)}, {person(7)}, {person(7)}}}));
  for (size_t i = 0; i < 3; ++i) {
    BOOST_REQUIRE_EQUAL(first.tracks[i].size(), 1);
    BOOST_REQUIRE_EQUAL(next.tracks[i].size(), 1);
    BOOST_REQUIRE(first.tracks[i][0].globalId.has_value());
    BOOST_REQUIRE(next.tracks[i][0].globalId.has_value());
    BOOST_CHECK_EQUAL(*first.tracks[i][0].globalId, *next.tracks[i][0].globalId);
  }
}

BOOST_AUTO_TEST_CASE(RepeatedHistoryStillValidatesEveryEntryAndCountsAgainstCapacity)
{
  auto config = Json::parse(std::string(PERSON_PROFILE_GOLDEN));
  config["limits"]["maxPersonsPerFrame"] = 1;
  config["limits"]["maxBackendTracksPerCamera"] = 3;
  const auto profile = parseUavPersonTrackingProfile(config.dump());
  auto wire = backendFixture();
  wire["profileDigest"] = profile.profileDigest;
  const auto removed = stockTrack(23, 3);
  wire["backendState"]["cameras"]["UAV1"]["tracker"]["removed"] = {removed, removed};
  const auto baseline = wire;
  BOOST_REQUIRE_NO_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), profile));
  wire["backendState"]["cameras"]["UAV1"]["tracker"]["removed"].push_back(removed);
  BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), profile),
                    std::invalid_argument);
  const std::vector<std::function<void(Json&)>> invalidEntries = {
    [] (auto& track) { track["trackId"] = 0; },
    [] (auto& track) { track["trackId"] = true; },
    [] (auto& track) { track["state"] = 2; },
    [] (auto& track) { track["frameId"] = 2; },
    [] (auto& track) { track["score"] = 2; },
    [] (auto& track) { track["mean"] = {1.0}; },
    [] (auto& track) { track["covariance"][0] = {1.0}; },
    [] (auto& track) { track["unexpected"] = true; },
  };
  for (const auto& mutate : invalidEntries) {
    wire = baseline;
    mutate(wire["backendState"]["cameras"]["UAV1"]["tracker"]["removed"][1]);
    BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), profile),
                      std::invalid_argument);
  }
  wire = backendFixture();
  const auto originalProfile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  auto& tracker = wire["backendState"]["cameras"]["UAV1"]["tracker"];
  tracker["tracked"].push_back(tracker["tracked"][0]);
  BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), originalProfile),
                    std::invalid_argument);
  tracker["tracked"].erase(1);
  tracker["lost"] = {stockTrack(23, 2), stockTrack(23, 2)};
  BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), originalProfile),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(FullLocalIdentityAndFeatureAdmission)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto admitted = parseUavPersonBackendWindow(backendFixture().dump(), windowContext(), profile);
  std::set<UavPersonLocalKey> keys;
  for (const auto& frame : admitted.frames) {
    BOOST_REQUIRE_EQUAL(frame.detections.size(), 1);
    const auto& detection = frame.detections.front();
    BOOST_CHECK_EQUAL(detection.key.byteTrackId, 7);
    BOOST_CHECK_EQUAL(detection.commonPtsUs, 1000000);
    BOOST_REQUIRE(detection.embedding.has_value());
    BOOST_CHECK_EQUAL(detection.embedding->at(0), 1.0f);
    BOOST_CHECK_EQUAL(detection.box[3], 120);
    keys.insert(detection.key);
  }
  BOOST_CHECK_EQUAL(keys.size(), 3);
  auto changed = admitted.frames[0].detections[0].key;
  ++changed.sourceEpoch;
  keys.insert(changed);
  --changed.sourceEpoch;
  ++changed.trackerGeneration;
  keys.insert(changed);
  changed.sourceId = "/source/replacement";
  keys.insert(changed);
  BOOST_CHECK_EQUAL(keys.size(), 6);
  BOOST_CHECK_EQUAL(admitted.detectorModelDevice, "cuda:0");
  BOOST_CHECK_EQUAL(admitted.reidInferenceBatches, 3);
  // A declared device in a fixture proves admission semantics, never actual GPU execution.
}

BOOST_AUTO_TEST_CASE(UntrackedAndNoFeatureDoNotInventIdsOrInference)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  auto wire = backendFixture();
  wire["reidInferenceBatches"] = 0;
  wire["actualReidDevice"] = "not-run";
  wire["reidInputDevice"] = "not-run";
  for (auto& frame : wire["frames"]) {
    auto& detection = frame["detections"][0];
    detection["byteTrackId"] = nullptr;
    detection["active"] = false;
    detection["embedding"] = nullptr;
    wire["backendState"]["cameras"][frame["cameraId"].get<std::string>()]
      ["tracker"]["tracked"] = Json::array();
  }
  const auto admitted = parseUavPersonBackendWindow(wire.dump(), windowContext(), profile);
  for (const auto& frame : admitted.frames) {
    BOOST_CHECK(!frame.detections[0].active);
    BOOST_CHECK_EQUAL(frame.detections[0].key.byteTrackId, 0);
    BOOST_CHECK(!frame.detections[0].embedding);
  }
  BOOST_CHECK_EQUAL(admitted.actualReidDevice, "not-run");
}

BOOST_AUTO_TEST_CASE(RejectForgedBindingsTypesVectorsAndState)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto baseline = backendFixture();
  BOOST_REQUIRE_NO_THROW(parseUavPersonBackendWindow(baseline.dump(), windowContext(), profile));
  const std::vector<std::function<void(Json&)>> mutations = {
    [] (auto& w) { w["runId"] = "other"; },
    [] (auto& w) { w["missionId"] = "other"; },
    [] (auto& w) { w["windowId"] = "other"; },
    [] (auto& w) { w["sequence"] = true; },
    [] (auto& w) { w["sequence"] = 1.0; },
    [] (auto& w) { w["computationDigest"] = "wrong"; },
    [] (auto& w) { w["profileDigest"] = "wrong"; },
    [] (auto& w) { w["detectorDigest"] = "wrong"; },
    [] (auto& w) { w["reidDigest"] = "wrong"; },
    [] (auto& w) { w["detectorModelDevice"] = "cpu"; },
    [] (auto& w) { w["detectorInputDevice"] = "cpu"; },
    [] (auto& w) { w["actualDetectorDevice"] = "cpu"; },
    [] (auto& w) { w["reidModelDevice"] = "cpu"; },
    [] (auto& w) { w["reidInputDevice"] = "cpu"; },
    [] (auto& w) { w["actualReidDevice"] = "not-run"; },
    [] (auto& w) { w["reidInferenceBatches"] = 0; },
    [] (auto& w) { w["frames"][0]["sourceId"] = "forged"; },
    [] (auto& w) { w["frames"][0]["sourceEpoch"] = 2; },
    [] (auto& w) { w["frames"][0]["trackerGeneration"] = 2; },
    [] (auto& w) { w["frames"][0]["commonPtsUs"] = -1; },
    [] (auto& w) { w["frames"][0]["inputDigest"] = "wrong"; },
    [] (auto& w) { w["frames"][0]["detections"][0]["globalId"] = 1; },
    [] (auto& w) { w["frames"][0]["detections"][0]["class"] = "car"; },
    [] (auto& w) { w["frames"][0]["detections"][0]["box"] = {0, 0, 641, 480}; },
    [] (auto& w) { w["frames"][0]["detections"][0]["confidence"] = -0.1; },
    [] (auto& w) { w["frames"][0]["detections"][0]["byteTrackId"] = 8; },
    [] (auto& w) { w["frames"][0]["detections"][0]["active"] = false; },
    [] (auto& w) { w["frames"][0]["detections"][0]["embedding"][0] = 0; },
    [] (auto& w) { w["frames"][0]["detections"][0]["embedding"][0] = 1.002; },
    [] (auto& w) { w["frames"][0]["detections"][0]["embedding"] = {1.0}; },
    [] (auto& w) { w["frames"][0]["detections"][0]["embedding"][1] = "nan"; },
    [] (auto& w) { auto d = w["frames"][0]["detections"][0];
                  w["frames"][0]["detections"].push_back(d); },
    [] (auto& w) { w["frames"][0]["lostByteTrackIds"] = {7}; },
    [] (auto& w) { w["backendState"]["cameras"]["UAV1"]["sourceEpoch"] = 2; },
    [] (auto& w) { w["backendState"]["cameras"]["UAV1"]["tracker"]["tracked"][0]
                    ["mean"] = {1.0}; },
    [] (auto& w) { w["backendState"]["cameras"]["UAV1"]["tracker"]["tracked"][0]
                    ["mean"][0] = 1e100; },
    [] (auto& w) { w["backendState"]["cameras"]["UAV1"]["tracker"]["frameId"] = 0; },
    [] (auto& w) { std::swap(w["frames"][0], w["frames"][1]); },
    [] (auto& w) { w["frames"].erase(0); },
  };
  for (size_t i = 0; i < mutations.size(); ++i) {
    auto wire = baseline;
    mutations[i](wire);
    BOOST_TEST_CONTEXT("mutation " << i) {
      BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), profile),
                        std::invalid_argument);
    }
  }
  const auto wire = backendFixture().dump();
  BOOST_CHECK_THROW(parseUavPersonBackendWindow("{\"schema\":0," + wire.substr(1),
                                               windowContext(), profile), std::invalid_argument);
  BOOST_CHECK_THROW(parseUavPersonBackendWindow(std::string(4194305, ' '),
                                               windowContext(), profile), std::invalid_argument);
  BOOST_CHECK_THROW(parseUavPersonBackendWindow("{\"x\":1e400}", windowContext(), profile),
                    std::invalid_argument);
  auto context = windowContext();
  context.frames[0].width = 0;
  BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire, context, profile), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StockLostRemovedTransitionAndCountBounds)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  auto wire = backendFixture();
  auto& tracker = wire["backendState"]["cameras"]["UAV1"]["tracker"];
  tracker["lost"].push_back(stockTrack(8, 3));
  tracker["removed"].push_back(stockTrack(8, 3));
  wire["frames"][0]["lostByteTrackIds"] = {8};
  BOOST_CHECK_NO_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), profile));
  tracker["lost"].push_back(stockTrack(8, 3));
  BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), profile),
                    std::invalid_argument);
  wire = backendFixture();
  auto& detections = wire["frames"][0]["detections"];
  auto untracked = detections[0];
  untracked["active"] = false;
  untracked["byteTrackId"] = nullptr;
  while (detections.size() < 64) detections.push_back(untracked);
  BOOST_CHECK_NO_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), profile));
  detections.push_back(untracked);
  BOOST_CHECK_THROW(parseUavPersonBackendWindow(wire.dump(), windowContext(), profile),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(UavPersonAssociation)

BOOST_AUTO_TEST_CASE(OverlapContinuesButNonOverlapCannotMergeActivePeople)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine engine(profile);
  auto context = atWindow(1, 1000000);
  auto first = engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {person(7)}, {}}}));
  BOOST_REQUIRE(first.tracks[0][0].globalId.has_value());
  BOOST_REQUIRE(first.tracks[1][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*first.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(*first.tracks[1][0].globalId, 1);
  BOOST_CHECK(!first.tracks[0][0].associationOrigin);
  BOOST_REQUIRE(first.tracks[1][0].associationOrigin.has_value());
  BOOST_CHECK_EQUAL(first.tracks[1][0].associationOrigin->key.cameraId, "UAV1");
  BOOST_CHECK_EQUAL(first.tracks[1][0].associationOrigin->ptsUs, 1000000);
  BOOST_CHECK(first.tracks[1][0].associationOrigin->active);
  context = atWindow(2, 1500000);
  const auto second = engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {person(7)}, {person(99)}}}));
  BOOST_CHECK_EQUAL(*second.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(second.tracks[0][0].associationStatus, "CONTINUED");
  BOOST_CHECK(!second.tracks[0][0].associationOrigin);
  BOOST_CHECK_EQUAL(*second.tracks[1][0].globalId, 1);
  BOOST_CHECK_EQUAL(*second.tracks[2][0].globalId, 2);
}

BOOST_AUTO_TEST_CASE(NonOverlapHandoffTransfersContinuityAndReappearanceIsFresh)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine engine(profile);
  auto context = atWindow(1, 1000000);
  engine.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 1500000);
  const auto handoff = engine.processWindow(context,
    admitWindow(context, profile, {{{}, {}, {person(99)}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(*handoff.tracks[2][0].globalId, 1);
  BOOST_CHECK_EQUAL(handoff.tracks[2][0].associationStatus, "MATCHED");
  BOOST_REQUIRE(handoff.tracks[2][0].associationOrigin.has_value());
  BOOST_CHECK_EQUAL(handoff.tracks[2][0].associationOrigin->key.cameraId, "UAV1");
  BOOST_CHECK_EQUAL(handoff.tracks[2][0].associationOrigin->key.byteTrackId, 7);
  BOOST_CHECK_EQUAL(handoff.tracks[2][0].associationOrigin->ptsUs, 1000000);
  BOOST_CHECK(!handoff.tracks[2][0].associationOrigin->active);
  const auto snapshot = Json::parse(handoff.candidateStateJson);
  BOOST_REQUIRE_EQUAL(snapshot["localMappings"].size(), 1);
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["key"]["cameraId"].get<std::string>(), "UAV3");
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["key"]["byteTrackId"].get<uint64_t>(), 99);
  context = atWindow(3, 2000000);
  UavPersonTrackingEngine resumed(profile);
  resumed.restoreState(handoff.candidateStateJson, context);
  const auto backend = admitWindow(context, profile, {{{person(7)}, {}, {person(99)}}});
  const auto continued = engine.processWindow(context, backend);
  const auto restored = resumed.processWindow(context, backend);
  BOOST_CHECK_EQUAL(restored.candidateStateJson, continued.candidateStateJson);
  BOOST_CHECK_EQUAL(resumed.serializeState(), engine.serializeState());
  for (const auto* result : {&continued, &restored}) {
    BOOST_CHECK(result->context == context);
    BOOST_CHECK_EQUAL(result->gallerySize, 2);
    BOOST_CHECK_EQUAL(result->localKeyCount, 2);
    BOOST_CHECK_EQUAL(result->watermarkUs, 2000000);
    BOOST_CHECK(result->tracks[1].empty());
    for (size_t camera : {size_t{0}, size_t{2}}) {
      BOOST_REQUIRE_EQUAL(result->tracks[camera].size(), 1);
      const auto& row = result->tracks[camera][0];
      BOOST_CHECK_EQUAL(row.key.cameraId, camera == 0 ? "UAV1" : "UAV3");
      BOOST_CHECK_EQUAL(row.key.sourceId, camera == 0 ? "/source/UAV1" : "/source/UAV3");
      BOOST_CHECK_EQUAL(row.key.sourceEpoch, 1);
      BOOST_CHECK_EQUAL(row.key.trackerGeneration, 1);
      BOOST_CHECK_EQUAL(row.key.byteTrackId, camera == 0 ? 7 : 99);
      BOOST_CHECK_EQUAL(row.commonPtsUs, 2000000);
      BOOST_CHECK(row.box == backend.frames[camera].detections[0].box);
      BOOST_CHECK_EQUAL(row.confidence, 0.9);
      BOOST_REQUIRE(row.globalId.has_value());
      BOOST_CHECK_EQUAL(*row.globalId, camera == 0 ? 2 : 1);
      BOOST_CHECK_EQUAL(row.associationStatus, camera == 0 ? "NO_CANDIDATE" : "CONTINUED");
      BOOST_CHECK(!row.distance);
      BOOST_CHECK(!row.associationOrigin);
    }
  }
}

BOOST_AUTO_TEST_CASE(GapSnapshotCannotBeRestoredUnderGemProfile)
{
  const auto gapProfile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  auto gemConfig = Json::parse(std::string(PERSON_PROFILE_GOLDEN));
  gemConfig["preprocessId"] = "osnet-rgb-256x128-imagenet-gem-p3-l2-v1";
  const auto gemProfile = parseUavPersonTrackingProfile(gemConfig.dump());
  BOOST_CHECK(gapProfile.profileDigest != gemProfile.profileDigest);

  UavPersonTrackingEngine gapEngine(gapProfile);
  const auto first = atWindow(1, 1000000);
  gapEngine.processWindow(first,
    admitWindow(first, gapProfile, {{{person(7)}, {}, {}}}));
  const auto snapshot = gapEngine.serializeState();
  BOOST_CHECK_EQUAL(Json::parse(snapshot)["profileDigest"].get<std::string>(),
                    gapProfile.profileDigest);

  UavPersonTrackingEngine gemEngine(gemProfile);
  const auto next = atWindow(2, 1500000);
  BOOST_CHECK_THROW(gemEngine.restoreState(snapshot, next), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OverlapAliasSurvivesReacquisitionAndCanContinueAlongsideNewOwner)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine engine(profile);
  auto context = atWindow(1, 1000000);
  engine.processWindow(context, admitWindow(context, profile, {{{person(7)}, {person(8)}, {}}}));
  context = atWindow(2, 1500000);
  const auto reacquired = engine.processWindow(context,
    admitWindow(context, profile, {{{}, {person(9)}, {}}}, {{{7}, {8}, {}}}));
  const auto snapshot = Json::parse(reacquired.candidateStateJson);
  BOOST_REQUIRE_EQUAL(snapshot["localMappings"].size(), 2);
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["key"]["cameraId"].get<std::string>(), "UAV1");
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["key"]["byteTrackId"].get<uint64_t>(), 7);
  BOOST_CHECK(!snapshot["localMappings"][0]["active"].get<bool>());
  BOOST_CHECK_EQUAL(snapshot["localMappings"][1]["key"]["byteTrackId"].get<uint64_t>(), 9);
  context = atWindow(3, 2000000);
  const auto continued = engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {person(9)}, {}}}));
  for (size_t camera : {size_t{0}, size_t{1}}) {
    BOOST_REQUIRE_EQUAL(continued.tracks[camera].size(), 1);
    BOOST_REQUIRE(continued.tracks[camera][0].globalId.has_value());
    BOOST_CHECK_EQUAL(*continued.tracks[camera][0].globalId, 1);
    BOOST_CHECK_EQUAL(continued.tracks[camera][0].associationStatus, "CONTINUED");
  }
}

BOOST_AUTO_TEST_CASE(ExistingLocalKeyAppearanceMismatchPreservesIdentityAndRestores)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto first = atWindow(1, 1000000);
  const auto second = atWindow(2, 1500000);
  const auto third = atWindow(3, 2000000);
  UavPersonTrackingEngine continuous(profile);
  continuous.processWindow(first,
    admitWindow(first, profile, {{{person(7, 1.0, 0.0)}, {}, {}}}));

  const auto split = continuous.processWindow(second,
    admitWindow(second, profile, {{{person(7, 0.0, 1.0)}, {}, {}}}));
  BOOST_REQUIRE_EQUAL(split.tracks[0].size(), 1);
  BOOST_REQUIRE(split.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*split.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(split.tracks[0][0].associationStatus, "NO_CANDIDATE");
  const auto splitState = Json::parse(split.candidateStateJson);
  BOOST_REQUIRE_EQUAL(splitState["gallery"].size(), 1);
  BOOST_CHECK_EQUAL(splitState["gallery"][0]["globalId"].get<uint64_t>(), 1);
  BOOST_CHECK_EQUAL(splitState["gallery"][0]["lastSeenUs"].get<int64_t>(), 1500000);
  BOOST_REQUIRE_EQUAL(splitState["gallery"][0]["samples"].size(), 1);
  BOOST_CHECK_EQUAL(splitState["gallery"][0]["samples"][0]["embedding"][0].get<double>(), 1.0);
  BOOST_CHECK_EQUAL(splitState["gallery"][0]["samples"][0]["embedding"][1].get<double>(), 0.0);
  BOOST_CHECK_EQUAL(splitState["nextGlobalId"].get<uint64_t>(), 2);
  BOOST_REQUIRE_EQUAL(splitState["localMappings"].size(), 1);
  BOOST_CHECK_EQUAL(splitState["localMappings"][0]["globalId"].get<uint64_t>(), 1);
  BOOST_CHECK(splitState["localMappings"][0]["active"].get<bool>());
  BOOST_CHECK_EQUAL(splitState["localMappings"][0]["lastSeenUs"].get<int64_t>(), 1500000);

  UavPersonTrackingEngine resumed(profile);
  resumed.restoreState(split.candidateStateJson, third);
  const auto backend = admitWindow(third, profile, {{{person(7, 0.0, 1.0)}, {}, {}}});
  const auto expected = continuous.processWindow(third, backend);
  const auto actual = resumed.processWindow(third, backend);
  BOOST_CHECK_EQUAL(actual.candidateStateJson, expected.candidateStateJson);
  BOOST_REQUIRE_EQUAL(actual.tracks[0].size(), 1);
  BOOST_REQUIRE(actual.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*actual.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(actual.tracks[0][0].associationStatus, "NO_CANDIDATE");
}

BOOST_AUTO_TEST_CASE(AppearanceMismatchRebindsToUniqueEligibleInactiveIdentity)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto first = atWindow(1, 1000000);
  const auto second = atWindow(2, 1500000);
  UavPersonTrackingEngine engine(profile);
  engine.processWindow(first,
    admitWindow(first, profile, {{{person(7, 1.0, 0.0), person(8, 0.0, 1.0)}, {}, {}}}));

  const auto rebound = engine.processWindow(second,
    admitWindow(second, profile, {{{person(7, 0.0, 1.0)}, {}, {}}}));
  BOOST_REQUIRE_EQUAL(rebound.tracks[0].size(), 1);
  const auto& row = rebound.tracks[0][0];
  BOOST_REQUIRE(row.globalId.has_value());
  BOOST_CHECK_EQUAL(*row.globalId, 2);
  BOOST_CHECK_EQUAL(row.associationStatus, "MATCHED");
  BOOST_REQUIRE(row.associationOrigin.has_value());
  BOOST_CHECK_EQUAL(row.associationOrigin->key.cameraId, "UAV1");
  BOOST_CHECK_EQUAL(row.associationOrigin->key.byteTrackId, 8);
  BOOST_CHECK(!row.associationOrigin->active);

  const auto snapshot = Json::parse(rebound.candidateStateJson);
  BOOST_REQUIRE_EQUAL(snapshot["gallery"].size(), 1);
  BOOST_CHECK_EQUAL(snapshot["gallery"][0]["globalId"].get<uint64_t>(), 2);
  BOOST_CHECK_EQUAL(snapshot["nextGlobalId"].get<uint64_t>(), 3);
  BOOST_REQUIRE_EQUAL(snapshot["localMappings"].size(), 1);
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["key"]["byteTrackId"].get<uint64_t>(), 7);
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["globalId"].get<uint64_t>(), 2);
}

BOOST_AUTO_TEST_CASE(AppearanceMismatchDoesNotPreserveMultipleKeysForOneIdentity)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  auto context = atWindow(1, 1000000);
  UavPersonTrackingEngine engine(profile);
  engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {person(8)}, {}}}));

  context = atWindow(2, 1500000);
  engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {}, {}}}));

  context = atWindow(3, 2000000);
  const auto split = engine.processWindow(context,
    admitWindow(context, profile, {{{person(7, 0.0, 1.0)}, {person(8, 0.0, 1.0)}, {}}}));
  BOOST_REQUIRE_EQUAL(split.tracks[0].size(), 1);
  BOOST_REQUIRE_EQUAL(split.tracks[1].size(), 1);
  BOOST_REQUIRE(split.tracks[0][0].globalId.has_value());
  BOOST_REQUIRE(split.tracks[1][0].globalId.has_value());
  BOOST_CHECK_NE(*split.tracks[0][0].globalId, 1);
  BOOST_CHECK_NE(*split.tracks[1][0].globalId, 1);
  const auto snapshot = Json::parse(split.candidateStateJson);
  for (const auto& identity : snapshot["gallery"]) {
    BOOST_CHECK_NE(identity["globalId"].get<uint64_t>(), 1);
  }
  for (const auto& mapping : snapshot["localMappings"]) {
    BOOST_CHECK_NE(mapping["globalId"].get<uint64_t>(), 1);
  }
}

BOOST_AUTO_TEST_CASE(AppearanceFailureCannotPreserveIdClaimedByLaterSameCameraMatch)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto first = atWindow(1, 1000000);
  const auto second = atWindow(2, 1500000);
  UavPersonTrackingEngine engine(profile);
  engine.processWindow(first,
    admitWindow(first, profile, {{{person(7, 1.0, 0.0)}, {}, {}}}));

  const auto result = engine.processWindow(second,
    admitWindow(second, profile,
      {{{person(7, 0.0, 1.0), person(8, 1.0, 0.0)}, {}, {}}}));
  BOOST_REQUIRE_EQUAL(result.tracks[0].size(), 2);
  BOOST_CHECK_EQUAL(result.tracks[0][0].key.byteTrackId, 7);
  BOOST_CHECK_EQUAL(result.tracks[0][0].associationStatus, "NO_CANDIDATE");
  BOOST_REQUIRE(result.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(result.tracks[0][0].globalId.value(), 2);
  BOOST_CHECK_EQUAL(result.tracks[0][1].key.byteTrackId, 8);
  BOOST_CHECK_EQUAL(result.tracks[0][1].associationStatus, "MATCHED");
  BOOST_REQUIRE(result.tracks[0][1].globalId.has_value());
  BOOST_CHECK_EQUAL(result.tracks[0][1].globalId.value(), 1);

  std::set<uint64_t> localIds;
  std::set<uint64_t> globalIds;
  for (const auto& row : result.tracks[0]) {
    BOOST_CHECK(localIds.insert(row.key.byteTrackId).second);
    BOOST_REQUIRE(row.globalId.has_value());
    BOOST_CHECK(globalIds.insert(*row.globalId).second);
  }
}

BOOST_AUTO_TEST_CASE(ExistingLocalKeyWithinAppearanceGateContinues)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto first = atWindow(1, 1000000);
  const auto second = atWindow(2, 1500000);
  UavPersonTrackingEngine engine(profile);
  engine.processWindow(first,
    admitWindow(first, profile, {{{person(7, 1.0, 0.0)}, {}, {}}}));
  const auto result = engine.processWindow(second,
    admitWindow(second, profile, {{{person(7, 1.0, 0.0)}, {}, {}}}));
  BOOST_REQUIRE_EQUAL(result.tracks[0].size(), 1);
  BOOST_REQUIRE(result.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*result.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(result.tracks[0][0].associationStatus, "CONTINUED");
}

BOOST_AUTO_TEST_CASE(ExistingLocalKeyWithoutFeatureOrPrototypeContinues)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto first = atWindow(1, 1000000);
  const auto second = atWindow(2, 1500000);
  UavPersonTrackingEngine missingFeature(profile);
  missingFeature.processWindow(first,
    admitWindow(first, profile, {{{person(7, 1.0, 0.0)}, {}, {}}}));
  const auto noFeature = missingFeature.processWindow(second,
    admitWindow(second, profile, {{{person(7, 0.0, 0.0, false)}, {}, {}}}));
  BOOST_REQUIRE_EQUAL(noFeature.tracks[0].size(), 1);
  BOOST_REQUIRE(noFeature.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*noFeature.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(noFeature.tracks[0][0].associationStatus, "CONTINUED");

  UavPersonTrackingEngine missingPrototype(profile);
  missingPrototype.processWindow(first,
    admitWindow(first, profile, {{{person(7, 0.0, 0.0, false)}, {}, {}}}));
  const auto noPrototype = missingPrototype.processWindow(second,
    admitWindow(second, profile, {{{person(7, 0.0, 1.0)}, {}, {}}}));
  BOOST_REQUIRE_EQUAL(noPrototype.tracks[0].size(), 1);
  BOOST_REQUIRE(noPrototype.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*noPrototype.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(noPrototype.tracks[0][0].associationStatus, "CONTINUED");
}

BOOST_AUTO_TEST_CASE(AppearanceBreakPreservesIdentityWithAnotherCurrentOwner)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine engine(profile);
  auto context = atWindow(1, 1000000);
  engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {person(8)}, {}}}));
  context = atWindow(2, 1500000);
  engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {}, {}}}));

  context = atWindow(3, 2000000);
  const auto result = engine.processWindow(context,
    admitWindow(context, profile, {{{person(7, 0.0, 1.0)}, {person(8)}, {}}}));
  BOOST_REQUIRE_EQUAL(result.tracks[0].size(), 1);
  BOOST_REQUIRE_EQUAL(result.tracks[1].size(), 1);
  BOOST_REQUIRE(result.tracks[0][0].globalId.has_value());
  BOOST_REQUIRE(result.tracks[1][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*result.tracks[0][0].globalId, 2);
  BOOST_CHECK_EQUAL(result.tracks[0][0].associationStatus, "NO_CANDIDATE");
  BOOST_CHECK_EQUAL(*result.tracks[1][0].globalId, 1);
  BOOST_CHECK_EQUAL(result.tracks[1][0].associationStatus, "CONTINUED");
  const auto snapshot = Json::parse(result.candidateStateJson);
  BOOST_REQUIRE_EQUAL(snapshot["gallery"].size(), 2);
  BOOST_REQUIRE_EQUAL(snapshot["localMappings"].size(), 2);
}

BOOST_AUTO_TEST_CASE(AppearanceBreakWithoutCandidatePreservesExistingIdentity)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine engine(profile);
  auto context = atWindow(1, 1000000);
  engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {person(8)}, {}}}));
  context = atWindow(2, 1500000);
  engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {}, {}}}));

  context = atWindow(3, 2000000);
  const auto result = engine.processWindow(context,
    admitWindow(context, profile, {{{person(7, 0.0, 1.0)}, {}, {}}}));
  BOOST_REQUIRE_EQUAL(result.tracks[0].size(), 1);
  BOOST_REQUIRE(result.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*result.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(result.tracks[0][0].associationStatus, "NO_CANDIDATE");
  const auto snapshot = Json::parse(result.candidateStateJson);
  BOOST_REQUIRE_EQUAL(snapshot["gallery"].size(), 1);
  BOOST_CHECK_EQUAL(snapshot["gallery"][0]["globalId"].get<uint64_t>(), 1);
  BOOST_REQUIRE_EQUAL(snapshot["localMappings"].size(), 2);
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["key"]["cameraId"].get<std::string>(), "UAV1");
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["globalId"].get<uint64_t>(), 1);
  BOOST_CHECK(snapshot["localMappings"][0]["active"].get<bool>());
  BOOST_CHECK_EQUAL(snapshot["localMappings"][1]["key"]["cameraId"].get<std::string>(), "UAV2");
  BOOST_CHECK_EQUAL(snapshot["localMappings"][1]["globalId"].get<uint64_t>(), 1);
  BOOST_CHECK(!snapshot["localMappings"][1]["active"].get<bool>());
  BOOST_CHECK_EQUAL(snapshot["nextGlobalId"].get<uint64_t>(), 2);
}

BOOST_AUTO_TEST_CASE(InvalidActiveNonOverlapSnapshotReachesExactConflictAndPreservesState)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine engine(profile);
  const auto context = atWindow(1, 1000000);
  const auto initial = engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {}, {person(99)}}}));
  const auto before = engine.serializeState();
  auto forged = Json::parse(initial.candidateStateJson);
  BOOST_REQUIRE_EQUAL(forged["gallery"].size(), 2);
  BOOST_REQUIRE_EQUAL(forged["localMappings"].size(), 2);
  BOOST_CHECK_EQUAL(forged["localMappings"][1]["key"]["cameraId"].get<std::string>(), "UAV3");
  BOOST_CHECK_EQUAL(forged["localMappings"][1]["globalId"].get<uint64_t>(), 2);
  // Both source/time/backend active tracks remain valid; only global overlap is contradictory.
  forged["localMappings"][1]["globalId"] = 1;
  forged["gallery"].erase(forged["gallery"].begin() + 1);
  BOOST_CHECK_EXCEPTION(engine.restoreState(forged.dump(), atWindow(2, 1500000)),
    std::invalid_argument, [] (const auto& error) {
      return std::string(error.what()) == "person-backend-invalid: snapshot overlap conflict";
    });
  BOOST_CHECK_EQUAL(engine.serializeState(), before);
}

BOOST_AUTO_TEST_CASE(OriginTieLatestReacquireAndLaterGlobalVeto)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto first = atWindow(1, 1000000);
  const auto second = atWindow(2, 1500000);
  const auto third = atWindow(3, 2000000);
  UavPersonTrackingEngine tied(profile);
  tied.processWindow(first, admitWindow(first, profile, {{{person(7)}, {person(8)}, {}}}));
  const auto handoff = tied.processWindow(second,
    admitWindow(second, profile, {{{}, {}, {person(9)}}}, {{{7}, {8}, {}}}));
  BOOST_REQUIRE(handoff.tracks[2][0].associationOrigin.has_value());
  BOOST_CHECK_EQUAL(*handoff.tracks[2][0].globalId, 1);
  BOOST_CHECK_EQUAL(handoff.tracks[2][0].associationOrigin->key.cameraId, "UAV1");

  UavPersonTrackingEngine latest(profile);
  latest.processWindow(first, admitWindow(first, profile, {{{person(7)}, {person(8)}, {}}}));
  latest.processWindow(second,
    admitWindow(second, profile, {{{}, {person(8)}, {}}}, {{{7}, {}, {}}}));
  const auto newer = latest.processWindow(third,
    admitWindow(third, profile, {{{}, {}, {person(9)}}}, {{{7}, {8}, {}}}));
  BOOST_REQUIRE(newer.tracks[2][0].associationOrigin.has_value());
  BOOST_CHECK_EQUAL(newer.tracks[2][0].associationOrigin->key.cameraId, "UAV2");
  BOOST_CHECK_EQUAL(newer.tracks[2][0].associationOrigin->ptsUs, 1500000);

  UavPersonTrackingEngine veto(profile);
  veto.processWindow(first, admitWindow(first, profile, {{{person(7)}, {person(8)}, {}}}));
  const auto rejected = veto.processWindow(second,
    admitWindow(second, profile, {{{}, {person(8)}, {person(9)}}}, {{{7}, {}, {}}}));
  // UAV1 is an eligible inactive witness, but the later UAV2 active mapping vetoes the ID.
  BOOST_CHECK_EQUAL(*rejected.tracks[2][0].globalId, 2);
  BOOST_CHECK(!rejected.tracks[2][0].associationOrigin);

  UavPersonTrackingEngine reacquire(profile);
  reacquire.processWindow(first, admitWindow(first, profile, {{{person(7)}, {}, {}}}));
  const auto sameCamera = reacquire.processWindow(second,
    admitWindow(second, profile, {{{person(9)}, {}, {}}}, {{{7}, {}, {}}}));
  BOOST_REQUIRE(sameCamera.tracks[0][0].associationOrigin.has_value());
  BOOST_CHECK_EQUAL(*sameCamera.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(sameCamera.tracks[0][0].associationOrigin->key.cameraId, "UAV1");
  BOOST_CHECK_EQUAL(sameCamera.tracks[0][0].associationOrigin->key.byteTrackId, 7);
  BOOST_CHECK(!sameCamera.tracks[0][0].associationOrigin->active);
}

BOOST_AUTO_TEST_CASE(SameCameraReacquisitionRetiresLostOwnerBeforeReactivation)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine engine(profile);
  auto context = atWindow(1, 1000000);
  engine.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 1500000);
  const auto reacquired = engine.processWindow(context,
    admitWindow(context, profile, {{{person(9)}, {}, {}}}, {{{7}, {}, {}}}));
  BOOST_REQUIRE_EQUAL(reacquired.tracks[0].size(), 1);
  BOOST_REQUIRE(reacquired.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*reacquired.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(reacquired.tracks[0][0].associationStatus, "MATCHED");
  BOOST_REQUIRE(reacquired.tracks[0][0].associationOrigin.has_value());
  const auto& origin = *reacquired.tracks[0][0].associationOrigin;
  BOOST_CHECK_EQUAL(origin.key.cameraId, "UAV1");
  BOOST_CHECK_EQUAL(origin.key.byteTrackId, 7);
  BOOST_CHECK_EQUAL(origin.ptsUs, 1000000);
  BOOST_CHECK(!origin.active);
  const auto snapshot = Json::parse(reacquired.candidateStateJson);
  BOOST_REQUIRE_EQUAL(snapshot["localMappings"].size(), 1);
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["key"]["byteTrackId"].get<uint64_t>(), 9);
  BOOST_CHECK_EQUAL(snapshot["localMappings"][0]["globalId"].get<uint64_t>(), 1);
  BOOST_CHECK(snapshot["localMappings"][0]["active"].get<bool>());

  context = atWindow(3, 2000000);
  UavPersonTrackingEngine resumed(profile);
  resumed.restoreState(reacquired.candidateStateJson, context);
  const auto backend = admitWindow(context, profile, {{{person(7), person(9)}, {}, {}}});
  const auto reference = engine.processWindow(context, backend);
  const auto actual = resumed.processWindow(context, backend);
  BOOST_CHECK_EQUAL(actual.candidateStateJson, reference.candidateStateJson);
  BOOST_CHECK_EQUAL(resumed.serializeState(), engine.serializeState());
  for (const auto* result : {&reference, &actual}) {
    BOOST_CHECK(result->context == context);
    BOOST_CHECK_EQUAL(result->gallerySize, 2);
    BOOST_CHECK_EQUAL(result->localKeyCount, 2);
    BOOST_CHECK_EQUAL(result->watermarkUs, 2000000);
    BOOST_REQUIRE_EQUAL(result->tracks[0].size(), 2);
    BOOST_CHECK(result->tracks[1].empty());
    BOOST_CHECK(result->tracks[2].empty());
    for (size_t index = 0; index < 2; ++index) {
      const auto& row = result->tracks[0][index];
      BOOST_CHECK_EQUAL(row.key.cameraId, "UAV1");
      BOOST_CHECK_EQUAL(row.key.sourceId, "/source/UAV1");
      BOOST_CHECK_EQUAL(row.key.sourceEpoch, 1);
      BOOST_CHECK_EQUAL(row.key.trackerGeneration, 1);
      BOOST_CHECK_EQUAL(row.key.byteTrackId, index == 0 ? 7 : 9);
      BOOST_CHECK_EQUAL(row.commonPtsUs, 2000000);
      BOOST_CHECK(row.box == backend.frames[0].detections[index].box);
      BOOST_CHECK_EQUAL(row.confidence, 0.9);
      BOOST_REQUIRE(row.globalId.has_value());
      BOOST_CHECK_EQUAL(*row.globalId, index == 0 ? 2 : 1);
      BOOST_CHECK_EQUAL(row.associationStatus, index == 0 ? "NO_CANDIDATE" : "CONTINUED");
      BOOST_CHECK(!row.distance);
      BOOST_CHECK(!row.associationOrigin);
    }
  }
}

BOOST_AUTO_TEST_CASE(SameCameraRetirementMakesCapacityWithoutWeakeningOverflow)
{
  const auto profile = withLimit(withLimit(
    parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN)), "maxPersonsPerFrame", 1),
    "maxLocalKeys", 1);
  UavPersonTrackingEngine engine(profile);
  auto context = atWindow(1, 1000000);
  engine.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 1500000);
  const auto reacquired = engine.processWindow(context,
    admitWindow(context, profile, {{{person(9)}, {}, {}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(reacquired.localKeyCount, 1);
  BOOST_REQUIRE(reacquired.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*reacquired.tracks[0][0].globalId, 1);
  const auto before = engine.serializeState();
  context = atWindow(3, 2000000);
  // Another camera does not retire this camera's owner to evade the final bound.
  BOOST_CHECK_THROW(engine.processWindow(context,
    admitWindow(context, profile, {{{person(9)}, {person(11)}, {}}})), std::invalid_argument);
  BOOST_CHECK_EQUAL(engine.serializeState(), before);
  const auto recovered = engine.processWindow(context,
    admitWindow(context, profile, {{{person(9)}, {}, {}}}));
  BOOST_CHECK_EQUAL(recovered.localKeyCount, 1);
  BOOST_CHECK_EQUAL(recovered.tracks[0][0].associationStatus, "CONTINUED");
}

BOOST_AUTO_TEST_CASE(OneToOneAssignmentRejectsAmbiguityAndSameCameraGreed)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  auto firstContext = atWindow(1, 1000000);
  auto secondContext = atWindow(2, 1500000);
  UavPersonTrackingEngine competing(profile);
  competing.processWindow(firstContext,
    admitWindow(firstContext, profile, {{{person(7)}, {}, {}}}));
  const auto tie = competing.processWindow(secondContext,
    admitWindow(secondContext, profile, {{{}, {}, {person(8), person(9)}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(tie.tracks[2][0].associationStatus, "AMBIGUOUS");
  BOOST_CHECK_EQUAL(tie.tracks[2][1].associationStatus, "AMBIGUOUS");
  BOOST_CHECK_EQUAL(*tie.tracks[2][0].globalId, 2);
  BOOST_CHECK_EQUAL(*tie.tracks[2][1].globalId, 3);
  UavPersonTrackingEngine unique(profile);
  unique.processWindow(firstContext,
    admitWindow(firstContext, profile, {{{person(7)}, {}, {}}}));
  const auto nearest = unique.processWindow(secondContext,
    admitWindow(secondContext, profile, {{{}, {}, {person(8), person(9, 0.8, 0.6)}}},
                 {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(*nearest.tracks[2][0].globalId, 1);
  BOOST_CHECK_EQUAL(*nearest.tracks[2][1].globalId, 2);
  BOOST_CHECK_EQUAL(nearest.tracks[2][1].associationStatus, "COMPETITION");
  UavPersonTrackingEngine nearMargin(profile);
  nearMargin.processWindow(firstContext,
    admitWindow(firstContext, profile, {{{person(7)}, {}, {}}}));
  const auto closeContest = nearMargin.processWindow(secondContext,
    admitWindow(secondContext, profile,
      {{{}, {}, {person(8), person(9, 0.98, std::sqrt(1.0 - 0.98 * 0.98))}}},
      {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(closeContest.tracks[2][0].associationStatus, "AMBIGUOUS");
  BOOST_CHECK_EQUAL(closeContest.tracks[2][1].associationStatus, "AMBIGUOUS");
  BOOST_CHECK_EQUAL(*closeContest.tracks[2][0].globalId, 2);
  BOOST_CHECK_EQUAL(*closeContest.tracks[2][1].globalId, 3);
  UavPersonTrackingEngine distractors(profile);
  const auto ambiguous = distractors.processWindow(firstContext,
    admitWindow(firstContext, profile, {{{person(7), person(8)}, {person(9)}, {}}}));
  BOOST_CHECK_EQUAL(*ambiguous.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(*ambiguous.tracks[0][1].globalId, 2);
  BOOST_CHECK_EQUAL(*ambiguous.tracks[1][0].globalId, 3);
  BOOST_CHECK_EQUAL(ambiguous.tracks[1][0].associationStatus, "AMBIGUOUS");
}

BOOST_AUTO_TEST_CASE(CosineAndTravelThresholdsClockGatesAndMissingFeatures)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  for (const auto firstComponent : {0.71, 0.69}) {
    UavPersonTrackingEngine engine(profile);
    auto context = atWindow(1, 1000000);
    engine.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
    context = atWindow(2, 1500000);
    const auto result = engine.processWindow(context,
      admitWindow(context, profile, {{{}, {}, {person(8, firstComponent,
        std::sqrt(1.0 - firstComponent * firstComponent))}}}, {{{7}, {}, {}}}));
    BOOST_CHECK_EQUAL(*result.tracks[2][0].globalId, firstComponent > 0.7 ? 1 : 2);
  }
  const auto travel = withLimit(profile, "minTransitionUs", 600000);
  UavPersonTrackingEngine tooSoon(travel);
  auto context = atWindow(1, 1000000);
  tooSoon.processWindow(context, admitWindow(context, travel, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 1500000);
  const auto denied = tooSoon.processWindow(context,
    admitWindow(context, travel, {{{}, {}, {person(8)}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(*denied.tracks[2][0].globalId, 2);
  UavPersonTrackingEngine exactTravel(travel);
  context = atWindow(1, 1000000);
  exactTravel.processWindow(context, admitWindow(context, travel, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 1600000);
  const auto allowed = exactTravel.processWindow(context,
    admitWindow(context, travel, {{{}, {}, {person(8)}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(*allowed.tracks[2][0].globalId, 1);
  UavPersonTrackingEngine unknownClock(profile);
  context = atWindow(1, 1000000);
  context.frames[1].clockAvailable = false;
  const auto localOnly = unknownClock.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {person(8)}, {}}}));
  BOOST_CHECK_EQUAL(*localOnly.tracks[1][0].globalId, 2);
  BOOST_CHECK_EQUAL(localOnly.tracks[1][0].associationStatus, "CLOCK_UNAVAILABLE");
  UavPersonTrackingEngine uncertainty(profile);
  context = atWindow(1, 1000000);
  context.frames[0].clockUncertaintyUs = 60000;
  context.frames[1].clockUncertaintyUs = 60000;
  const auto noMerge = uncertainty.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {person(8)}, {}}}));
  BOOST_CHECK_EQUAL(*noMerge.tracks[1][0].globalId, 2);
  UavPersonTrackingEngine exactClock(profile);
  context.frames[0].clockUncertaintyUs = 50000;
  context.frames[1].clockUncertaintyUs = 50000;
  const auto samePerson = exactClock.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {person(8)}, {}}}));
  BOOST_CHECK_EQUAL(*samePerson.tracks[1][0].globalId, 1);
  UavPersonTrackingEngine missing(profile);
  const auto noFeature = missing.processWindow(context,
    admitWindow(context, profile, {{{person(7, 1.0, 0.0, false)}, {person(8)}, {}}}));
  BOOST_CHECK_EQUAL(noFeature.tracks[0][0].associationStatus, "NO_FEATURE");
  BOOST_CHECK_EQUAL(*noFeature.tracks[1][0].globalId, 2);
  UavPersonTrackingEngine untracked(profile);
  auto raw = person(7, 1.0, 0.0, false);
  raw["byteTrackId"] = nullptr;
  raw["active"] = false;
  const auto diagnostic = untracked.processWindow(context,
    admitWindow(context, profile, {{{raw}, {}, {}}}));
  BOOST_CHECK(!diagnostic.tracks[0][0].globalId.has_value());
  BOOST_CHECK_EQUAL(diagnostic.localKeyCount, 0);
  BOOST_CHECK_EQUAL(diagnostic.gallerySize, 0);
}

BOOST_AUTO_TEST_CASE(SourceTimeOrderAndExpiryCannotConsumeAFutureFrame)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine order(profile);
  auto context = atWindow(1, 1000000);
  context.frames[2].commonPtsUs = 900000;
  const auto ordered = order.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {}, {person(8)}}}));
  BOOST_CHECK_EQUAL(*ordered.tracks[2][0].globalId, 1);
  BOOST_CHECK_EQUAL(*ordered.tracks[0][0].globalId, 2);
  UavPersonTrackingEngine expiry(profile);
  context = atWindow(1, 1000000);
  expiry.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 1500000);
  expiry.processWindow(context, admitWindow(context, profile, {{{}, {}, {}}}, {{{7}, {}, {}}}));
  context = atWindow(3, 32000000);
  context.frames[2].commonPtsUs = 30500000;
  const auto beforeFutureExpiry = expiry.processWindow(context,
    admitWindow(context, profile, {{{}, {}, {person(8)}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(*beforeFutureExpiry.tracks[2][0].globalId, 1);
  BOOST_CHECK_EQUAL(beforeFutureExpiry.watermarkUs, 32000000);
  UavPersonTrackingEngine retired(profile);
  context = atWindow(1, 1000000);
  retired.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 32000000);
  const auto newIdentity = retired.processWindow(context,
    admitWindow(context, profile, {{{}, {}, {person(8)}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(*newIdentity.tracks[2][0].globalId, 2);
  BOOST_CHECK_EQUAL(newIdentity.gallerySize, 1);
  UavPersonTrackingEngine exactTtl(profile);
  context = atWindow(1, 1000000);
  exactTtl.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 31000000);
  const auto stillValid = exactTtl.processWindow(context,
    admitWindow(context, profile, {{{}, {}, {person(8)}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(*stillValid.tracks[2][0].globalId, 1);
}

BOOST_AUTO_TEST_CASE(OverThresholdEdgesCannotCreateForwardOrReverseAmbiguity)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto first = atWindow(1, 1000000);
  const auto second = atWindow(2, 1500000);
  UavPersonTrackingEngine forward(profile);
  forward.processWindow(first, admitWindow(first, profile,
    {{{person(7, 0.71, std::sqrt(1.0 - 0.71 * 0.71)),
       person(8, 0.69, std::sqrt(1.0 - 0.69 * 0.69))}, {}, {}}}));
  const auto matched = forward.processWindow(second,
    admitWindow(second, profile, {{{}, {}, {person(99)}}}, {{{7, 8}, {}, {}}}));
  BOOST_CHECK_EQUAL(*matched.tracks[2][0].globalId, 1);
  BOOST_CHECK_EQUAL(matched.tracks[2][0].associationStatus, "MATCHED");
  UavPersonTrackingEngine reverse(profile);
  reverse.processWindow(first, admitWindow(first, profile, {{{person(7)}, {}, {}}}));
  const auto candidates = reverse.processWindow(second, admitWindow(second, profile,
    {{{}, {}, {person(8, 0.71, std::sqrt(1.0 - 0.71 * 0.71)),
                person(9, 0.69, std::sqrt(1.0 - 0.69 * 0.69))}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(*candidates.tracks[2][0].globalId, 1);
  BOOST_CHECK_EQUAL(candidates.tracks[2][0].associationStatus, "MATCHED");
  BOOST_CHECK_EQUAL(*candidates.tracks[2][1].globalId, 2);
  BOOST_CHECK_EQUAL(candidates.tracks[2][1].associationStatus, "NO_CANDIDATE");
}

BOOST_AUTO_TEST_CASE(ResourceAndOrderFailuresPreserveTheCandidateState)
{
  const auto profile = withLimit(
    parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN)), "maxGlobalIdentities", 1);
  UavPersonTrackingEngine engine(profile);
  auto context = atWindow(1, 1000000);
  engine.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 1500000);
  BOOST_CHECK_THROW(engine.processWindow(context,
    admitWindow(context, profile, {{{}, {}, {person(8, 0.0, 1.0)}}}, {{{7}, {}, {}}})),
    std::invalid_argument);
  auto stale = context;
  stale.frames[0].commonPtsUs = 999999;
  BOOST_CHECK_THROW(engine.processWindow(stale,
    admitWindow(stale, profile, {{{}, {}, {person(8)}}}, {{{7}, {}, {}}})), std::invalid_argument);
  const auto successfulRetry = engine.processWindow(context,
    admitWindow(context, profile, {{{}, {}, {person(8)}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(*successfulRetry.tracks[2][0].globalId, 1);
  BOOST_CHECK_EQUAL(successfulRetry.gallerySize, 1);
  const auto localProfile = withLimit(withLimit(
    parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN)), "maxPersonsPerFrame", 1),
    "maxLocalKeys", 1);
  UavPersonTrackingEngine transferred(localProfile);
  context = atWindow(1, 1000000);
  transferred.processWindow(context,
    admitWindow(context, localProfile, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 1500000);
  const auto handoff = transferred.processWindow(context,
    admitWindow(context, localProfile, {{{}, {}, {person(8)}}}, {{{7}, {}, {}}}));
  BOOST_CHECK_EQUAL(handoff.localKeyCount, 1);
  BOOST_REQUIRE(handoff.tracks[2][0].globalId.has_value());
  BOOST_CHECK_EQUAL(*handoff.tracks[2][0].globalId, 1);
  UavPersonTrackingEngine local(localProfile);
  context = atWindow(1, 1000000);
  local.processWindow(context, admitWindow(context, localProfile, {{{person(7)}, {}, {}}}));
  context = atWindow(2, 1500000);
  BOOST_CHECK_THROW(local.processWindow(context,
    admitWindow(context, localProfile, {{{}, {person(8)}, {}}}, {{{7}, {}, {}}})),
    std::invalid_argument);
  const auto stillLocal = local.processWindow(context,
    admitWindow(context, localProfile, {{{person(7)}, {}, {}}}));
  BOOST_CHECK_EQUAL(*stillLocal.tracks[0][0].globalId, 1);
  BOOST_CHECK_EQUAL(stillLocal.localKeyCount, 1);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(UavPersonState)

BOOST_AUTO_TEST_CASE(ContinuousAndRestoredExecutionHaveIdenticalDecisionsAndBytes)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine continuous(profile);
  std::string predecessor;
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    const auto context = atWindow(sequence, 500000 + static_cast<int64_t>(sequence) * 500000);
    const auto backend = sequence < 3 ?
      admitWindow(context, profile, {{{person(7)}, {}, {}}}) :
      admitWindow(context, profile, {{{}, {}, {person(99)}}}, {{{7}, {}, {}}});
    UavPersonTrackingEngine resumed(profile);
    if (!predecessor.empty()) resumed.restoreState(predecessor, context);
    const auto reference = continuous.processWindow(context, backend);
    const auto actual = resumed.processWindow(context, backend);
    const auto camera = sequence < 3 ? 0 : 2;
    BOOST_REQUIRE(actual.tracks[camera][0].globalId.has_value());
    BOOST_CHECK_EQUAL(*actual.tracks[camera][0].globalId, 1);
    BOOST_CHECK_EQUAL(actual.tracks[camera][0].associationStatus,
                      sequence == 1 ? "NO_CANDIDATE" : sequence == 2 ? "CONTINUED" : "MATCHED");
    BOOST_CHECK_EQUAL(actual.candidateStateJson, reference.candidateStateJson);
    BOOST_CHECK_EQUAL(resumed.serializeState(), continuous.serializeState());
    BOOST_CHECK_EQUAL(resumed.backendStateJson(), backend.backendStateJson);
    if (sequence == 3) {
      const auto& actualOrigin = actual.tracks[camera][0].associationOrigin;
      const auto& referenceOrigin = reference.tracks[camera][0].associationOrigin;
      BOOST_REQUIRE(actualOrigin.has_value());
      BOOST_REQUIRE(referenceOrigin.has_value());
      BOOST_CHECK_EQUAL(actualOrigin->key.cameraId, referenceOrigin->key.cameraId);
      BOOST_CHECK_EQUAL(actualOrigin->key.byteTrackId, referenceOrigin->key.byteTrackId);
      BOOST_CHECK_EQUAL(actualOrigin->ptsUs, referenceOrigin->ptsUs);
      BOOST_CHECK_EQUAL(actualOrigin->active, referenceOrigin->active);
    }
    predecessor = actual.candidateStateJson;
  }
}

BOOST_AUTO_TEST_CASE(CorruptLateFieldsAndBindingsLeaveAllOwnedStateUnchanged)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine engine(profile);
  auto context = atWindow(1, 1000000);
  engine.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  const auto original = engine.serializeState();
  const auto baseline = Json::parse(original);
  context = atWindow(2, 1500000);
  BOOST_REQUIRE_NO_THROW(engine.restoreState(original, context));
  const std::vector<std::function<void(Json&)>> mutations = {
    [] (auto& w) { w["schema"] = "legacy-session"; },
    [] (auto& w) { w["profileDigest"] = "wrong"; },
    [] (auto& w) { w["binding"]["missionId"] = "other"; },
    [] (auto& w) { w["binding"]["computationDigest"] = "sha256:" + std::string(64, 'c'); },
    [] (auto& w) { w["binding"]["frames"][0]["sourceEpoch"] = 2; },
    [] (auto& w) { w["nextGlobalId"] = 1; },
    [] (auto& w) { w["watermarkUs"] = 1000001; },
    [] (auto& w) { auto row = w["gallery"][0]; w["gallery"].push_back(row); },
    [] (auto& w) { w["gallery"][0]["samples"][0]["embedding"] = {1.0}; },
    [] (auto& w) { w["gallery"][0]["samples"][0]["embedding"][0] = 0.0; },
    [] (auto& w) { w["gallery"][0]["samples"][0]["ptsUs"] = 1000001; },
    [] (auto& w) { auto sample = w["gallery"][0]["samples"][0]; sample["ptsUs"] = 999999;
                  w["gallery"][0]["samples"].push_back(sample); },
    [] (auto& w) { w["gallery"][0]["lastSeenUs"] = 999999; },
    [] (auto& w) { auto row = w["localMappings"][0]; w["localMappings"].push_back(row); },
    [] (auto& w) { w["localMappings"][0]["globalId"] = 999; },
    [] (auto& w) { w["localMappings"][0]["active"] = false; },
    [] (auto& w) { w["localMappings"][0]["key"]["sourceId"] = "forged"; },
    [] (auto& w) { w["backendState"]["cameras"]["UAV3"]["sourceEpoch"] = 2; },
    [] (auto& w) { w["backendState"]["cameras"]["UAV1"]["tracker"]["tracked"][0]
                    ["covariance"][7][7] = 1e100; },
  };
  for (size_t i = 0; i < mutations.size(); ++i) {
    auto malformed = baseline;
    mutations[i](malformed);
    BOOST_TEST_CONTEXT("snapshot mutation " << i) {
      BOOST_CHECK_THROW(engine.restoreState(malformed.dump(), context), std::invalid_argument);
      BOOST_CHECK_EQUAL(engine.serializeState(), original);
    }
  }
  for (const auto& malformed : {std::string("{}"), original.substr(0, original.size() - 1),
         "{\"schema\":0," + original.substr(1), std::string(8388609, ' ')}) {
    BOOST_CHECK_THROW(engine.restoreState(malformed, context), std::invalid_argument);
    BOOST_CHECK_EQUAL(engine.serializeState(), original);
  }
  auto wrongNext = context;
  wrongNext.sequence = 3;
  BOOST_CHECK_THROW(engine.restoreState(original, wrongNext), std::invalid_argument);
  BOOST_CHECK_EQUAL(engine.serializeState(), original);
  wrongNext = context;
  ++wrongNext.frames[0].trackerGeneration;
  BOOST_CHECK_THROW(engine.restoreState(original, wrongNext), std::invalid_argument);
  BOOST_CHECK_EQUAL(engine.serializeState(), original);
  const auto next = engine.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  BOOST_CHECK_EQUAL(*next.tracks[0][0].globalId, 1);
}

BOOST_AUTO_TEST_CASE(SamplesAndSnapshotCollectionsRemainBounded)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  UavPersonTrackingEngine engine(profile);
  for (uint64_t sequence = 1; sequence <= 12; ++sequence) {
    const auto context = atWindow(sequence, 500000 + static_cast<int64_t>(sequence) * 500000);
    engine.processWindow(context, admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  }
  const auto original = engine.serializeState();
  const auto baseline = Json::parse(original);
  const auto& samples = baseline.at("gallery")[0].at("samples");
  BOOST_CHECK_EQUAL(samples.size(), 8);
  BOOST_CHECK_EQUAL(samples[0].at("ptsUs").get<uint64_t>(), 3000000);
  const auto next = atWindow(13, 7000000);
  for (const auto& collection : {"gallery", "localMappings", "samples", "backendTracks"}) {
    auto malformed = baseline;
    if (std::string(collection) == "gallery") {
      malformed["gallery"] = std::vector<Json>(257, baseline["gallery"][0]);
    }
    else if (std::string(collection) == "localMappings") {
      malformed["localMappings"] = std::vector<Json>(1025, baseline["localMappings"][0]);
    }
    else if (std::string(collection) == "samples") {
      malformed["gallery"][0]["samples"].push_back(samples.back());
    }
    else {
      malformed["backendState"]["cameras"]["UAV1"]["tracker"]["removed"] =
        std::vector<Json>(513, stockTrack(99, 3));
    }
    BOOST_CHECK_THROW(engine.restoreState(malformed.dump(), next), std::invalid_argument);
    BOOST_CHECK_EQUAL(engine.serializeState(), original);
  }
}

BOOST_AUTO_TEST_CASE(SnapshotByteFailureCannotCommitAllocatedIds)
{
  const auto profile = parseUavPersonTrackingProfile(std::string(PERSON_PROFILE_GOLDEN));
  const auto context = atWindow(1, 1000000);
  UavPersonTrackingEngine reference(profile);
  const auto first = reference.processWindow(context,
    admitWindow(context, profile, {{{person(7)}, {}, {}}}));
  const auto byteLimit = first.candidateStateJson.size() - 1;
  const auto bounded = withLimit(profile, "maxSnapshotBytes", byteLimit);
  const auto backend = admitWindow(context, bounded, {{{person(7)}, {}, {}}});
  BOOST_REQUIRE_LT(backend.backendStateJson.size(), byteLimit);
  UavPersonTrackingEngine engine(bounded);
  BOOST_CHECK_THROW(engine.processWindow(context, backend), std::invalid_argument);
  BOOST_CHECK_THROW(engine.serializeState(), std::invalid_argument);
  const auto empty = engine.processWindow(context, admitWindow(context, bounded, {{{}, {}, {}}}));
  BOOST_CHECK_EQUAL(empty.gallerySize, 0);
  BOOST_CHECK_EQUAL(Json::parse(empty.candidateStateJson).at("nextGlobalId").get<uint64_t>(), 1);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
