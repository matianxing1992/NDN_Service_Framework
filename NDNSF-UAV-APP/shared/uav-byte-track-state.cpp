#include "uav-byte-track-state.hpp"

#include <limits>

namespace ndnsf::examples::uav::byte_track {
namespace {

using person_json::Json;
using person_json::finiteNumber;
using person_json::finiteArray;
using person_json::reject;
constexpr uint64_t STOCK_REMOVED_BUFFER = 1000;
constexpr uint64_t MAX_STOCK_TRACK_ID = (1ULL << 24) - 1;

} // namespace

using person_json::boolean;
using person_json::exactKeys;
using person_json::unsignedNumber;

TrackState
checkTracker(const Json& wire, uint64_t maximumTracks, BackendStateFormat format)
{
  const bool compactHistory = format != BackendStateFormat::LEGACY_CONFIRMED;
  if (compactHistory) {
    exactKeys(wire, {"frameId", "tracked", "lost", "removedIds", "idHighWatermark"});
  }
  else {
    exactKeys(wire, {"frameId", "tracked", "lost", "removed"});
  }
  const auto highWatermark = compactHistory
    ? unsignedNumber(wire.at("idHighWatermark"), MAX_STOCK_TRACK_ID - 1)
    : std::numeric_limits<uint64_t>::max();
  if (compactHistory) {
    const auto& history = wire.at("removedIds");
    if (!history.is_array() || history.size() > STOCK_REMOVED_BUFFER) {
      reject("BACKEND_REMOVAL_HISTORY_LIMIT");
    }
    // Retain order, repeated IDs and temporary overlap with the live pools.
    for (const auto& item : history) {
      if (unsignedNumber(item, highWatermark) == 0) reject("zero removed ID");
    }
  }
  const auto frameId = unsignedNumber(wire.at("frameId"));
  uint64_t count = 0;
  TrackState result;
  for (const auto* group : {"tracked", "lost", "removed"}) {
    if (compactHistory && std::string(group) == "removed") continue;
    const auto& items = wire.at(group);
    if (!items.is_array() || items.size() > maximumTracks - count) reject("BACKEND_TRACK_LIMIT");
    count += items.size();
    std::set<uint64_t> seen;
    for (const auto& track : items) {
      exactKeys(track, {"trackId", "isActivated", "state", "tlwh", "mean", "covariance",
                        "score", "class", "index", "angle", "startFrame", "frameId",
                        "timeSinceUpdate", "trackletLength"});
      const auto id = unsignedNumber(track.at("trackId"), highWatermark);
      // Stock removal history can repeat IDs with different predicted arrays after restore.
      // Active/lost groups stay unique; every history entry still counts and is validated.
      if (id == 0 || (std::string(group) != "removed" && !seen.insert(id).second)) {
        reject("duplicate/zero stock track ID");
      }
      const auto state = unsignedNumber(track.at("state"), 3);
      const auto start = unsignedNumber(track.at("startFrame"));
      const auto current = unsignedNumber(track.at("frameId"));
      if (start > current || current > frameId) reject("stock tracker frame history");
      unsignedNumber(track.at("timeSinceUpdate"));
      unsignedNumber(track.at("trackletLength"));
      const auto score = finiteNumber(track.at("score"));
      if (score < 0 || score > 1 || finiteNumber(track.at("class")) < 0 ||
          finiteNumber(track.at("index")) < 0) reject("stock track scalars");
      if (!track.at("angle").is_null()) finiteNumber(track.at("angle"));
      finiteArray(track.at("tlwh"), 4);
      finiteArray(track.at("mean"), 8);
      const auto& covariance = track.at("covariance");
      if (!covariance.is_array() || covariance.size() != 8) reject("covariance dimensions");
      for (const auto& row : covariance) finiteArray(row, 8);
      const auto activated = boolean(track.at("isActivated"));
      if (std::string(group) == "tracked") {
        if (state != 1) reject("tracked state");
        // V3 admits actual current stock IDs before confirmation; old formats remain strict.
        if ((activated || format == BackendStateFormat::COMPACT_CURRENT) && current == frameId) {
          result.currentIds.insert(id);
        }
      }
      else if (std::string(group) == "lost") {
        // A newly removed track can remain in lost for one stock update.
        if (state != 2 && state != 3) reject("lost state");
        result.lost.insert(id);
      }
      else if (state != 3) reject("removed state");
    }
  }
  for (const auto id : result.currentIds) {
    if (result.lost.count(id)) reject("stock track both active and lost");
  }
  return result;
}

} // namespace ndnsf::examples::uav::byte_track
