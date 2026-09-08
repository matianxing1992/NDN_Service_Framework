#pragma once
#include <stdexcept>
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

namespace ndnsf::di {
/** Expected feasibility outcome, distinct from malformed or unauthenticated input. */
class NativeNoFeasiblePlacement : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
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
