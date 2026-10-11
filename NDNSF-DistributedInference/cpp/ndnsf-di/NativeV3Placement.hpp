#pragma once
#include <stdexcept>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

namespace ndnsf::di {
struct NativeRolePlacementProposalV3;
/** Expected feasibility outcome, distinct from malformed or unauthenticated input. */
class NativeNoFeasiblePlacement : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

/**
 * Deterministic operator-supplied role-to-Provider placement for controlled
 * two-node experiments.  The map keys are the exact V3 role keys: a single
 * rank uses `role`, while a ranked role uses `role#rank`.  The strategy still
 * runs the normal offer, backend, device, and memory feasibility checks; it
 * only replaces the generic spread/reuse ranking with the fixed assignment.
 * Layer ranges remain owned by the authenticated splitter/role contract.
 */
class NativeFixedProviderPlacement final : public CooperativePlacementStrategy
{
public:
  explicit NativeFixedProviderPlacement(
    std::map<std::string, std::string> providerByRole,
    NativeStrategyIdentity identity = {
      "native-fixed-provider-map", "1",
      "sha256:0000000000000000000000000000000000000000000000000000000000000000"});

  NativeStrategyIdentity identity() const override;

  NativeRolePlacementProposalV3 proposeRoles(
    const NativeOfferBindingContext&, const std::string&,
    const std::vector<NativeSelectionRoleV3>&,
    const std::vector<NativeAdmittedOfferV3>&, std::uint64_t,
    const ExtensionControl&) const override;

private:
  NativeRolePlacementProposalV3 proposeRolesImpl(
    const NativeOfferBindingContext&, const std::string&,
    const std::vector<NativeSelectionRoleV3>&,
    const std::vector<NativeAdmittedOfferV3>&, std::uint64_t,
    const ExtensionControl*) const;

  std::map<std::string, std::string> m_providerByRole;
  NativeStrategyIdentity m_identity;
};
/** Complete role/rank proposal from admitted observations. No lease authority. */
struct NativeRolePlacementProposalV3
{
  NativeOfferBindingContext context;
  std::string ackClosedDigest;
  NativeStrategyIdentity strategy;
  std::vector<NativeSelectionRoleV3> roles;
  std::map<std::string, std::string> providerByRole;
  std::map<std::string, std::string> offerDigestByProvider;
  /** Present for the V4 observation-aware policy; included in the sealed core digest. */
  std::optional<NativeJson> placementDecisionV4;
  /** Present only when the first pass needs a bounded targeted measurement round. */
  std::optional<NativePlacementProbeIntentV1> placementProbeIntent;
};

struct NativeChirpBandwidthEstimateV4
{
  std::string status = "UNKNOWN";
  std::string reason = "INCOMPLETE_OR_INVALID_SIGNATURE";
  std::optional<double> lowerMbps;
  std::optional<double> upperMbps;
};

/** Conservative offered-rate interpretation of one complete four-Data chirp. */
NativeChirpBandwidthEstimateV4 estimateNativeChirpBandwidthV4(
  const std::array<double, 3>& interestSendGapsMs,
  const std::array<double, 3>& dataArrivalGapsMs,
  const std::array<std::uint64_t, 4>& dataWireBytes,
  bool complete, bool signatureValid);

/** V4 whole-assignment policy; V3 admission and wire contracts remain unchanged.
 * network-only excludes preparation and queue wait from the numeric objective,
 * while retaining their observations. Neither mode estimates inference time.
 */
class NativeObservationAwarePlacementV4 final : public NativePlacementStrategy,
                                                 public CooperativePlacementStrategy
{
public:
  explicit NativeObservationAwarePlacementV4(
    std::size_t beamWidth = 32, std::size_t maxAssignmentStates = 4096,
    std::string scoreMode = "preparation-and-network");

  NativeStrategyIdentity identity() const override;
  NativeRolePlacementProposalV3 proposeRoles(
    const NativeOfferBindingContext&, const std::string&,
    const std::vector<NativeSelectionRoleV3>&,
    const std::vector<NativeAdmittedOfferV3>&, std::uint64_t) const override;
  NativeRolePlacementProposalV3 proposeRoles(
    const NativeOfferBindingContext&, const std::string&,
    const std::vector<NativeSelectionRoleV3>&,
    const std::vector<NativeAdmittedOfferV3>&, std::uint64_t,
    const ExtensionControl&) const override;

private:
  NativeRolePlacementProposalV3 proposeRolesImpl(
    const NativeOfferBindingContext&, const std::string&,
    const std::vector<NativeSelectionRoleV3>&,
    const std::vector<NativeAdmittedOfferV3>&, std::uint64_t,
    const ExtensionControl*) const;

  std::size_t m_beamWidth;
  std::size_t m_maxAssignmentStates;
  std::string m_scoreMode;
};

/** Policy-neutral validation: feasible custom assignments need not equal the default ranking. */
void validateNativeRolePlacement(const NativeRolePlacementProposalV3& proposal,
  const std::vector<NativeSelectionRoleV3>& preparedRoles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs);
/** Reuse structural validation only for exact immutable role interfaces.
 * Current placement and admitted-offer checks still execute synchronously. */
void validateNativeRolePlacementWithFixedCore(const NativeRolePlacementProposalV3& proposal,
  const std::vector<NativeSelectionRoleV3>& preparedRoles, const NativeFixedPlacementCoreV1& fixed,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs);
/** Caller has bound the cached certificates to this invocation's publication. */
void validateNativePublishedRolePlacement(const NativeRolePlacementProposalV3& proposal,
  const NativeFixedPlacementCoreV1& fixed, const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs);
} // namespace ndnsf::di
