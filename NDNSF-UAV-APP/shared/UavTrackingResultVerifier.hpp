#ifndef NDNSF_EXAMPLES_UAV_TRACKING_RESULT_VERIFIER_HPP
#define NDNSF_EXAMPLES_UAV_TRACKING_RESULT_VERIFIER_HPP

#include "uav-person-tracking.hpp"

#include <ndn-cxx/name.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace ndnsf::examples::uav {

/** Explicit indoor mode and immutable computation expectation, absent for legacy results. */
struct UavPersonTrackingResultExpectation
{
  UavPersonTrackingProfile profile;
  std::string computationDigest;
};

/** Native vehicle output; a GS requires the marker, its Producer also pins the digest. */
struct UavVehicleTrackingResultExpectation
{
  bool observationMode = true;
  std::string nativeDecisionDigest;
};

struct UavTrackingResultExpectation
{
  std::string runId;
  std::string missionId;
  std::string windowId;
  uint64_t sequence = 0;
  uint64_t sourceEpoch = 0;
  std::array<std::string, 3> sourceIds;
  std::array<std::string, 3> inputDigests;
  ndn::Name computeProvider;
  std::optional<UavPersonTrackingResultExpectation> personTracking;
  /** Explicit source Producers; all-empty retains the maintained legacy CLI namespace. */
  std::array<ndn::Name, 3> sourceProviders;
  std::optional<UavVehicleTrackingResultExpectation> vehicleTracking;
};

/** Serialize native decisions and original source references without exposing gallery vectors. */
std::string
makeUavPersonTrackingResultJson(const UavPersonWindowResult& result,
                                const UavPersonBackendWindow& backend,
                                const UavPersonTrackingProfile& profile,
                                const std::array<std::string, 3>& observationNames,
                                const std::array<std::string, 3>& observationDigests);

std::string
makeUavPersonTrackingResultJson(const UavPersonWindowResult& result,
                                const UavPersonBackendWindow& backend,
                                const UavPersonTrackingProfile& profile,
                                const std::array<std::string, 3>& observationNames,
                                const std::array<std::string, 3>& observationDigests,
                                const std::array<ndn::Name, 3>& sourceProducers);

/** Bind a Spec210 terminal result and its three frames to the request context. */
void
validateUavTrackingResultContext(const std::string& resultName,
                                 const std::string& responseProvider,
                                 const std::string& resultJson,
                                 const UavTrackingResultExpectation& expected);

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_TRACKING_RESULT_VERIFIER_HPP
