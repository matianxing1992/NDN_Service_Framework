#ifndef NDNSF_EXAMPLES_UAV_PERSON_COMPUTATION_HPP
#define NDNSF_EXAMPLES_UAV_PERSON_COMPUTATION_HPP

#include "uav-person-tracking-profile.hpp"

#include <cstddef>
#include <map>
#include <string>

namespace ndnsf::examples::uav {

/** Frozen computation identity. Inventories describe artifacts, not execution evidence. */
struct UavPersonComputationManifest
{
  std::string canonicalJson;
  std::string profileDigest;
  std::map<std::string, std::string> backendInventory;
  std::map<std::string, std::string> nativeInventory;
  std::string backendClosureDigest;
  std::string nativeClosureDigest;
  std::string computationDigest;
};

constexpr size_t MAX_PERSON_COMPUTATION_BYTES = 65536;

/** Validate bounded artifact inventories and derive the DM-1 identity without reading files. */
UavPersonComputationManifest
makeUavPersonComputationManifest(const UavPersonTrackingProfile& profile,
                               const std::map<std::string, std::string>& backendInventory,
                               const std::map<std::string, std::string>& nativeInventory);

/** Reject unknown fields and recompute every declared digest against the local profile. */
UavPersonComputationManifest
parseUavPersonComputationManifest(const std::string& json,
                                const UavPersonTrackingProfile& profile);

/** Fields for the maintained signed request. The manifest belongs to the compute producer. */
std::map<std::string, std::string>
makeUavPersonRequestBindingFields(const UavPersonTrackingProfile& profile,
                                const UavPersonComputationManifest& manifest);

/** Detect even a partial person binding so legacy callers cannot silently accept it. */
bool
hasUavPersonRequestBindingFields(const std::map<std::string, std::string>& fields);

/** Require all person fields to match locally frozen values; other request fields are allowed. */
void
requireUavPersonRequestBindingFields(const std::map<std::string, std::string>& fields,
                                   const UavPersonTrackingProfile& profile,
                                   const UavPersonComputationManifest& manifest);

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_PERSON_COMPUTATION_HPP
