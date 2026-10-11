#ifndef NDNSF_DI_NATIVE_PLAN_SEALER_HPP
#define NDNSF_DI_NATIVE_PLAN_SEALER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantTypes.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeSecurityPolicySnapshot
{
  std::string policyDigest;
  bool requireProtectedArtifacts = true;
};

struct NativePlacementPlanCore;

/** Factory-frozen placement configuration; current request authority is absent. */
class NativeFixedPlacementCoreV1 final
{
public:
  const std::map<std::string, NativeSelectionRoleV3>& roles() const { return m_roles; }
  const std::string& canonicalRoles() const { return m_canonicalRoles; }
  const std::string& roleDigest(const std::string& role) const { return m_roleDigests.at(role); }
  bool matches(const NativePlacementPlanCore& core) const;
  bool canonicalRecipes() const { return m_canonicalRecipes; }

private:
  friend class NativePlanSealer;
  explicit NativeFixedPlacementCoreV1(const NativePlacementPlanCore& core);
  std::map<std::string, NativeSelectionRoleV3> m_roles;
  NativeExecutionPlan m_execution;
  NativeGenerationExecutionContractV1 m_generation;
  std::map<std::string, std::string> m_providers;
  std::string m_model, m_source, m_graph, m_candidate;
  std::string m_canonicalRoles;
  std::map<std::string, std::string> m_roleDigests;
  bool m_canonicalRecipes = true;
};

struct NativePlacementPlanCore
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::string modelDigest;
  // Source binding checked by the sealer against inspection. V3 modelDigest
  // is the request intent; the legacy snapshot overload retains its old wire.
  std::string sourceContentDigest;
  std::string graphDigest;
  std::string ackClosedDigest;
  std::string candidateDigest;
  NativeStrategyIdentity strategy;
  NativeExecutionPlan executionPlan;
  NativeProviderAssignment assignment;
  std::map<std::string, std::string> offerDigestByProvider;
  std::map<std::string, std::string> artifactDigestByRole;
  std::string coreDigest;
  NativeArtifactBinding artifacts;
  std::string requesterIdentity;
  std::string protectionEpoch;
  std::uint64_t expiresAtMs = 0;

  std::map<std::string, NativeSelectionRoleV3> assemblyByRole;
  std::string requestContractDigest;
  NativeGenerationExecutionContractV1 generationContract;
  // Optional V4 observation snapshot binding. Empty preserves the V3 core
  // canonical bytes exactly.
  std::string placementObservationSnapshotDigest;
  /** Optional V4 policy result, canonically bound only for V4 placement. */
  std::optional<NativeJson> placementDecisionV4;
  std::shared_ptr<const NativeFixedPlacementCoreV1> fixedCoreOwner;
  bool reusedFixedCore = false; // Runtime evidence, never a wire authority.

  const std::map<std::string, NativeSelectionRoleV3>& roleContracts() const
  { return fixedCoreOwner ? fixedCoreOwner->roles() : assemblyByRole; }

  void validate() const;
};

/** Authenticated publication output and request-owner security context.
 * The requester passes its original wire expiry, never a new relative TTL.
 * No artifact identity is synthesized from a role or Provider name. */
struct NativePlanSealingInputs
{
  NativeArtifactBinding artifacts;
  std::string requesterIdentity;
  std::string protectionEpoch;
  std::uint64_t expiresAtMs = 0;
  std::map<std::string, NativeSelectionRoleV3> assemblyByRole;
  std::string requestContractDigest;
  NativeGenerationExecutionContractV1 generationContract;
  std::string placementObservationSnapshotDigest;
  // Synchronous request-local view; never retained by a returned core.
  const std::vector<NativeSelectionRoleV3>* borrowedPreparedRoles = nullptr;
  std::shared_ptr<const NativeFixedPlacementCoreV1> previousFixedCore;
};

/** Execution-owner contracts, bound after the plan digest is finalized. */
struct NativeRoleProjectionInputs
{
  NativeExecutionRoleV3 executionRole;
  NativeRoleDataflowContractV3 dataflow;
  NativeDeviceBindingV3 deviceBinding;
  std::string groupCapabilityV1;
  std::optional<ConversationStateReferenceV1> conversationStateReference;
  std::optional<ConversationTurnBindingV1> conversationTurnBinding;
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
  std::optional<NativeGrantLeaseScope> grantLeaseScope;

  void validate() const;
};

/** A factory-checked, independently owned, physically const plan. Public value
 * copies remain ordinary mutable plans and do not inherit this proof. */
class NativeOwnedSealedPlan final
{
public:
  NativeSealedPlan snapshot() const;
  /** Mutable current bindings plus shared immutable configuration. This value
   * is not an owned-sealed proof and its public consumers still validate it. */
  NativeSealedPlan invocationContext() const;

private:
  friend class NativePlanSealer;
  friend class NativePlanProjectionBuilder;
  friend class NativeGroupProjectionBuilder;
  // Only the checked production consumers can borrow internal storage.
  const NativeSealedPlan& value() const;
  explicit NativeOwnedSealedPlan(std::shared_ptr<const NativeSealedPlan> value);
  std::shared_ptr<const NativeSealedPlan> m_value;
};

/** Stateless canonical plan constructor used between placement and Core. */
class NativePlanSealer final
{
public:
  /** Retain a matched immutable configuration, keeping the original core digest.
   * Mutable value inputs are fully checked before becoming a new owner. */
  static NativePlacementPlanCore shareFixedCore(NativePlacementPlanCore core,
    std::shared_ptr<const NativeFixedPlacementCoreV1> previous = {});

  /** Recheck the request scope, digest, and freshness of the exact V4
   * observation input immediately before the plan core is sealed. */
  static void validateObservationSnapshotBinding(
    const NativeOfferBindingContext& context,
    const NativePlanSealingInputs& inputs,
    const std::string& ackClosedDigest,
    std::uint64_t nowMs);

  static NativePlacementPlanCore sealCore(
    const NativePlanningSnapshot& snapshot,
    const NativePlacementProposal& proposal,
    const NativePlanSealingInputs& inputs);

  static NativePlacementPlanCore sealCore(
    const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
    const NativeRolePlacementProposalV3& proposal, const NativeExecutionPlan& executionPlan,
    const std::vector<NativeAdmittedOfferV3>& offers, const std::string& expectedAckClosedDigest,
    const NativePlanSealingInputs& inputs);

  static NativeProviderGrantView grantView(
    const NativePlacementPlanCore& core,
    const NativeProviderPlanningView& provider,
    const NativeSecurityPolicySnapshot& security,
    const std::string& role = {});

  static NativeProviderGrantView grantView(const NativePlacementPlanCore& core,
    const NativeAdmittedOfferV3& provider, const NativeSecurityPolicySnapshot& security,
    const std::string& role = {});

  static NativeSealedPlan finalizeSecurity(
    const NativePlacementPlanCore& core,
    const std::vector<NativeGrantBinding>& grants,
    const NativeSecurityPolicySnapshot& security,
    std::optional<NativeGrantLeaseScope> grantLeaseScope = {});

  static NativeOwnedSealedPlan finalizeSecurityOwned(
    const NativePlacementPlanCore& core,
    const std::vector<NativeGrantBinding>& grants,
    const NativeSecurityPolicySnapshot& security,
    std::optional<NativeGrantLeaseScope> grantLeaseScope = {});

  static NativeSelectionProjectionV3 project(
    const NativeSealedPlan& sealed, const std::string& provider,
    const NativeRoleProjectionInputs& inputs);

  static std::vector<std::uint8_t> projectAndEncode(
    const NativeSealedPlan& sealed, const std::string& provider,
    const NativeRoleProjectionInputs& inputs);

  /** Validate the sealed plan once and encode its exact assigned role set.
   * Inputs must not be mutated concurrently; returned bytes are owned.
   * Each role still passes the production serializer and wire-size limit. */
  static std::map<std::string, std::vector<std::uint8_t>> projectAndEncodeAll(
    const NativeSealedPlan& sealed,
    const std::map<std::string, NativeRoleProjectionInputs>& inputsByRole);

  static std::map<std::string, std::vector<std::uint8_t>> projectAndEncodeAll(
    const NativeOwnedSealedPlan& sealed,
    const std::map<std::string, NativeRoleProjectionInputs>& inputsByRole);

  static std::vector<std::uint8_t> encode(
    const NativeSelectionProjectionV3& projection);

  /** Current context borrowing a matched immutable role from a checked core. */
  static NativeSelectionProjectionV3 projectInvocation(
    const NativeOwnedSealedPlan& sealed, const std::string& provider,
    const NativeRoleProjectionInputs& inputs,
    const NativeFixedRoleAssignmentV1* fixedAssignment = nullptr);

private:
  friend class NativePlanProjectionBuilder;
  static std::map<std::string, std::vector<std::uint8_t>> projectAndEncodeAllFromValidatedSealed(
    const NativeSealedPlan& sealed,
    const std::map<std::string, NativeRoleProjectionInputs>& inputsByRole);
  // Only the checked projection construction may reuse core validation.
  static NativeProviderGrantView grantViewFromValidatedCore(
    const NativePlacementPlanCore& core, const NativeAdmittedOfferV3& provider,
    const NativeSecurityPolicySnapshot& security, const std::string& role);
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_PLAN_SEALER_HPP
