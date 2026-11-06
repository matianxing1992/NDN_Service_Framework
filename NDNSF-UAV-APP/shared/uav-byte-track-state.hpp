#ifndef NDNSF_UAV_BYTE_TRACK_STATE_HPP
#define NDNSF_UAV_BYTE_TRACK_STATE_HPP

#include "uav-person-json.hpp"

#include <cstdint>
#include <set>

namespace ndnsf::examples::uav::byte_track {

/** Current activated IDs and lost IDs admitted from the maintained tracker codec. */
struct TrackState
{
  std::set<uint64_t> currentIds;
  std::set<uint64_t> lost;
};

enum class BackendStateFormat
{
  LEGACY_CONFIRMED,
  COMPACT_CONFIRMED,
  COMPACT_CURRENT
};

/** Validate every serialized stock track before its candidate ID can be admitted. */
TrackState
checkTracker(const person_json::Json& wire, uint64_t maximumTracks, BackendStateFormat format);

} // namespace ndnsf::examples::uav::byte_track

#endif // NDNSF_UAV_BYTE_TRACK_STATE_HPP
