#ifndef NDNSF_EXAMPLES_UAV_PERSON_TRACKING_PROFILE_HPP
#define NDNSF_EXAMPLES_UAV_PERSON_TRACKING_PROFILE_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace ndnsf::examples::uav {

/** Immutable validated configuration; numeric policy values use scaled integers. */
struct UavPersonTrackingProfile
{
  std::string canonicalJson;
  std::string profileDigest;
  std::string detectorDigest;
  std::string reidDigest;
  std::string reidPreprocessId;
  std::string requestedDevice;
  std::map<std::string, uint64_t> limits;
  std::map<std::pair<std::string, std::string>, bool> overlappingCameras;
};

/** Reject malformed, oversized, unknown, duplicate or out-of-range profile fields. */
UavPersonTrackingProfile
parseUavPersonTrackingProfile(const std::string& json);

/** Reject a zero or over-limit one-based window before execution or state admission. */
void
requireUavPersonWindowCapacity(const UavPersonTrackingProfile& profile, uint64_t window);

/** Parse the canonical zero-based application window name and enforce profile capacity. */
uint64_t
parseUavPersonWindowIndex(const UavPersonTrackingProfile& profile, const std::string& windowId);

constexpr size_t MAX_PERSON_PROFILE_BYTES = 16384;

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_PERSON_TRACKING_PROFILE_HPP
