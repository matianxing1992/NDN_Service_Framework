#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

namespace ndnsf::di {
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
} // namespace ndnsf::di
