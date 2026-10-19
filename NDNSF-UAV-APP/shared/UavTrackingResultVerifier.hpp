#pragma once

#include <ndn-cxx/name.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace ndnsf::examples::uav {

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
};

/** Bind a Spec210 terminal result and its three frames to the request context. */
void
validateUavTrackingResultContext(const std::string& resultName,
                                 const std::string& responseProvider,
                                 const std::string& resultJson,
                                 const UavTrackingResultExpectation& expected);

} // namespace ndnsf::examples::uav
