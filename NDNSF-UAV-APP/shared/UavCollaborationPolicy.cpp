#include "UavCollaborationPolicy.hpp"

#include <algorithm>

namespace ndnsf::examples::uav {
namespace {

bool
qualityMatches(const std::string& required, const std::string& actual)
{
  return required.empty() || required == actual;
}

bool
candidateMatches(const UavDetectorRequirement& requirement,
                 const UavDetectorCandidate& candidate,
                 uint64_t nowMs)
{
  const auto& capability = candidate.capability;
  if (!candidate.ackVerified || candidate.explicitFallback ||
      capability.providerIdentity.empty() || capability.modelId.empty() ||
      capability.modelDigest.empty() || capability.deviceClass.empty() ||
      !capability.ready ||
      !capability.evidenceAccess || capability.queueDepth > requirement.maxQueueDepth ||
      capability.snapshotTimeMs == 0 || nowMs < capability.snapshotTimeMs ||
      nowMs - capability.snapshotTimeMs > requirement.maxAckAgeMs ||
      (!requirement.modelId.empty() && capability.modelId != requirement.modelId) ||
      !qualityMatches(requirement.qualityProfile, capability.qualityProfile)) {
    return false;
  }
  return requirement.preferredDeviceClass.empty() ||
         capability.deviceClass == requirement.preferredDeviceClass;
}

} // namespace

UavSelectionDecision
selectUavDetector(const UavDetectorRequirement& requirement,
                  const std::vector<UavDetectorCandidate>& candidates,
                  uint64_t nowMs,
                  const ndn::Name& explicitFallback,
                  const std::optional<UavDetectorCandidate>& fallbackCandidate)
{
  UavSelectionDecision decision;
  if (requirement.modelId.empty() || requirement.qualityProfile.empty() ||
      requirement.maxAckAgeMs == 0) {
    decision.reason = "invalid detector requirement";
    return decision;
  }

  std::vector<const UavDetectorCandidate*> feasible;
  for (const auto& candidate : candidates) {
    if (candidateMatches(requirement, candidate, nowMs)) {
      feasible.push_back(&candidate);
    }
  }
  std::sort(feasible.begin(), feasible.end(),
            [](const auto* left, const auto* right) {
              if (left->capability.queueDepth != right->capability.queueDepth) {
                return left->capability.queueDepth < right->capability.queueDepth;
              }
              if (left->capability.estimatedStartMs != right->capability.estimatedStartMs) {
                return left->capability.estimatedStartMs < right->capability.estimatedStartMs;
              }
              return left->capability.providerIdentity < right->capability.providerIdentity;
            });
  if (!feasible.empty()) {
    decision.selected = true;
    decision.providerIdentity = feasible.front()->capability.providerIdentity;
    decision.reason = "verified capability and readiness";
    return decision;
  }

  if (!explicitFallback.empty() && fallbackCandidate) {
    auto verifiedFallback = *fallbackCandidate;
    verifiedFallback.explicitFallback = false;
    // A configured fallback may intentionally run on a different device
    // class (for example, Ground Station CPU instead of UAV GPU).  It still
    // must satisfy the model/quality/readiness/evidence contract; the device
    // downgrade is observable through fallbackUsed and the terminal report.
    auto fallbackRequirement = requirement;
    fallbackRequirement.preferredDeviceClass.clear();
    if (!verifiedFallback.ackVerified ||
        verifiedFallback.capability.providerIdentity != explicitFallback ||
        !candidateMatches(fallbackRequirement, verifiedFallback, nowMs)) {
      decision.reason = "fallback capability is not verified or does not match requirement";
      return decision;
    }
    decision.selected = true;
    decision.providerIdentity = explicitFallback;
    decision.fallbackUsed = true;
    decision.reason = "explicit fallback; no eligible compute provider";
    return decision;
  }
  decision.reason = "no feasible detector provider";
  return decision;
}

} // namespace ndnsf::examples::uav
