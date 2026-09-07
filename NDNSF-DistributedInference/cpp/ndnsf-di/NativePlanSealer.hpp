#ifndef NDNSF_DI_NATIVE_PLAN_SEALER_HPP
#define NDNSF_DI_NATIVE_PLAN_SEALER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeSecurityPolicySnapshot
{
  std::string policyDigest;
  bool requireProtectedArtifacts = true;
};

struct NativeGrantBinding
{
  std::string provider;
  std::string role;
  std::string grantName;
  std::string grantDigest;
  std::string recipient;
  std::string wireJson;
  std::uint64_t expiresAtMs = 0;
};

struct NativePlacementPlanCore
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string modelDigest;
  std::string graphDigest;
  std::string ackClosedDigest;
  std::string candidateDigest;
  NativeStrategyIdentity strategy;
  NativeExecutionPlan executionPlan;
  NativeProviderAssignment assignment;
  std::map<std::string, std::string> offerDigestByProvider;
  std::map<std::string, std::string> artifactDigestByRole;
  std::string coreDigest;

  void validate() const;
};

struct NativeProviderGrantView
{
  std::string provider;
  std::string role;
  std::string planCoreDigest;
  std::string policyDigest;
  std::string modelDigest;
  std::string graphDigest;
  std::string artifactDigest;
  std::string requesterIdentity;
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string modelManifestDigest;
  std::string protectionEpoch;
  std::uint64_t expiresAtMs = 0;
};

struct NativeSealedPlan
{
  NativePlacementPlanCore core;
  std::vector<NativeGrantBinding> grants;
  NativeSecurityPolicySnapshot security;
  std::string planDigest;

  void validate() const;
};

/** Stateless canonical plan constructor used between placement and Core. */
class NativePlanSealer final
{
public:
  static NativePlacementPlanCore sealCore(
    const NativePlanningSnapshot& snapshot,
    const NativePlacementProposal& proposal);

  static NativeProviderGrantView grantView(
    const NativePlacementPlanCore& core,
    const NativeProviderPlanningView& provider,
    const NativeSecurityPolicySnapshot& security);

  static NativeSealedPlan finalizeSecurity(
    const NativePlacementPlanCore& core,
    const std::vector<NativeGrantBinding>& grants,
    const NativeSecurityPolicySnapshot& security);

  static NativeSelectionProjectionV3 project(
    const NativeSealedPlan& sealed, const std::string& provider);

  static std::vector<std::uint8_t> encode(
    const NativeSelectionProjectionV3& projection);
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_PLAN_SEALER_HPP
