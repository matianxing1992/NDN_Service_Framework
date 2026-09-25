#pragma once
#include <stdexcept>
#include <map>
#include <memory>
#include <string>
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
};

/** Policy-neutral validation: feasible custom assignments need not equal the default ranking. */
void validateNativeRolePlacement(const NativeRolePlacementProposalV3& proposal,
  const std::vector<NativeSelectionRoleV3>& preparedRoles,
  const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs);
} // namespace ndnsf::di
