#ifndef NDNSF_EXAMPLES_UAV_COLLABORATION_POLICY_HPP
#define NDNSF_EXAMPLES_UAV_COLLABORATION_POLICY_HPP

#include "UavProtocol.hpp"

#include <cstdint>
#include <string>
#include <optional>
#include <vector>

namespace ndnsf::examples::uav {

struct UavDetectorRequirement
{
  std::string modelId;
  std::string qualityProfile;
  std::string preferredDeviceClass;
  uint64_t maxAckAgeMs = 5000;
  uint64_t maxQueueDepth = 64;
};

struct UavDetectorCandidate
{
  UavProviderCapabilitySnapshot capability;
  // This is a provenance assertion supplied by the NDNSF ServiceUser after
  // its ACK validation path. The policy never treats a provider name or a
  // payload parse as cryptographic verification; callers must leave it false
  // unless the candidate came from the accepted ACK_CLOSED snapshot.
  bool ackVerified = false;
  bool explicitFallback = false;
};

struct UavSelectionDecision
{
  bool selected = false;
  ndn::Name providerIdentity;
  bool fallbackUsed = false;
  std::string reason;
};

/**
 * Select one detector using request-scoped, verified capability metadata.
 * Provider identities are NDN names; this function has no transport-address
 * inputs and never infers capability from a name.
 */
UavSelectionDecision
selectUavDetector(const UavDetectorRequirement& requirement,
                  const std::vector<UavDetectorCandidate>& candidates,
                  uint64_t nowMs,
                  const ndn::Name& explicitFallback = {},
                  const std::optional<UavDetectorCandidate>& fallbackCandidate = std::nullopt);

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_COLLABORATION_POLICY_HPP
