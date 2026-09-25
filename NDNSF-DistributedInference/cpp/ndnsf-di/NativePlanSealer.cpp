#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"

#include <algorithm>
#include <tuple>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::di {
namespace {

bool contains(const std::vector<std::string>& values, const std::string& value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string canonicalCore(const NativePlacementPlanCore& core)
{
  auto roles = NativeJson::array();
  for (const auto& role : core.executionPlan.roles) {
    const auto assembly = core.assemblyByRole.find(role);
    if (assembly == core.assemblyByRole.end()) throw std::invalid_argument("missing sealed assembly role");
    roles.push_back(nativeAssemblyJson(assembly->second));
  }
  const auto strategy = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"name", core.strategy.name}, {"version", core.strategy.version},
    {"state", core.strategy.configurationDigest}}));
  return nativeCanonicalJson(NativeJson{
    {"request_id", core.requestId}, {"attempt", core.attempt},
    {"model_digest", core.modelDigest}, {"graph_digest", core.graphDigest},
    {"roles", roles}, {"provider_by_role", core.assignment.providerByRole},
    {"dependencies", nativeDependenciesJson(core.executionPlan.dependencies)},
    {"ack_closed_digest", core.ackClosedDigest}, {"strategy_digest", strategy},
    {"candidate_digest", core.candidateDigest}, {"request_contract_digest", core.requestContractDigest},
    {"generation_contract", core.generationContract.enabled ? nativeGenerationJson(core.generationContract) : NativeJson(nullptr)}});
}

std::string canonicalSealed(const NativeSealedPlan& sealed)
{
  std::vector<std::tuple<std::string, std::string, std::string>> grants;
  for (const auto& grant : sealed.grants) grants.emplace_back(grant.provider, grant.grantName, grant.grantDigest);
  std::sort(grants.begin(), grants.end());
  if (!sealed.grantLeaseScope) {
    return nativeCanonicalJson(NativeJson{{"core", sealed.core.coreDigest}, {"grants", grants},
      {"securityPolicySnapshotDigest", sealed.security.policyDigest}});
  }
  const auto& scope = *sealed.grantLeaseScope;
  const auto lease = NativeJson{{"conversation_id", scope.conversationId},
      {"requester_identity", scope.requesterIdentity}, {"service_name", scope.serviceName},
      {"request_id", scope.requestId}, {"attempt", scope.attempt},
      {"plan_core_digest", scope.planCoreDigest}, {"scope_digest", scope.scopeDigest},
      {"security_policy_snapshot_digest", scope.securityPolicySnapshotDigest},
      {"protection_epoch", scope.protectionEpoch}, {"expires_at_ms", scope.expiresAtMs},
      {"provider_by_role", scope.providerByRole}};
  return nativeCanonicalJson(NativeJson{{"core", sealed.core.coreDigest}, {"grants", grants},
    {"securityPolicySnapshotDigest", sealed.security.policyDigest}, {"grant_lease", lease}});
}

} // namespace

void NativePlacementPlanCore::validate() const
{
  artifacts.validate();
  if (artifacts.requestId != requestId || artifacts.attempt != attempt ||
      !isDigest(sourceContentDigest) || artifacts.modelDigest != sourceContentDigest ||
      artifacts.graphDigest != graphDigest ||
      !isDigest(artifacts.canonicalGraphDigest) ||
      artifacts.artifactDigestByRole != artifactDigestByRole ||
      requesterIdentity.empty() || requesterIdentity.front() != '/' ||
      protectionEpoch.empty() || expiresAtMs == 0) {
    throw std::invalid_argument("native sealed artifact/request binding is incomplete");
  }
  if (requestId.empty() || attempt == 0 || !isDigest(modelDigest) ||
      !isDigest(graphDigest) || !isDigest(ackClosedDigest) ||
      !isDigest(candidateDigest) || executionPlan.roles.empty() ||
      !isDigest(coreDigest)) {
    throw std::invalid_argument("native placement plan core identity is incomplete");
  }
  strategy.validate();
  std::set<std::string> roles(executionPlan.roles.begin(), executionPlan.roles.end());
  if (roles.size() != executionPlan.roles.size() ||
      assignment.providerByRole.size() != roles.size() ||
      artifactDigestByRole.size() != roles.size() ||
      assemblyByRole.size() != roles.size() ||
      offerDigestByProvider.empty()) {
    throw std::invalid_argument("native placement plan core role cover is incomplete");
  }
  for (const auto& role : roles) {
    const auto provider = assignment.providerByRole.find(role);
    const auto artifact = artifactDigestByRole.find(role);
    if (provider == assignment.providerByRole.end() || provider->second.empty() ||
        artifact == artifactDigestByRole.end() || !isDigest(artifact->second)) {
      throw std::invalid_argument("native placement plan core role binding is invalid");
    }
    const auto offer = offerDigestByProvider.find(provider->second);
    if (offer == offerDigestByProvider.end() || !isDigest(offer->second)) {
      throw std::invalid_argument("native placement plan core offer binding is invalid");
    }
    const auto assembly = assemblyByRole.find(role);
    if (assembly == assemblyByRole.end() || assembly->second.selectedRole != role ||
        assembly->second.artifactDigest != artifact->second ||
        assembly->second.graphDigest != artifacts.canonicalGraphDigest ||
        assembly->second.modelManifestDigest != artifacts.manifestDigest ||
        assembly->second.protectionEpoch != protectionEpoch) {
      throw std::invalid_argument("native sealed assembly differs from authenticated artifact context");
    }
    if (assembly->second.role != role &&
        assembly->second.role + "#" + std::to_string(assembly->second.rank) != role) {
      throw std::invalid_argument("native assembly logical role/rank differs from assignment");
    }
    validateNativeAssembly(assembly->second);
  }
  if (nativePlanningDigest(canonicalCore(*this)) != coreDigest) {
    throw std::invalid_argument("native plan core was modified after sealing");
  }
}

void NativeSealedPlan::validate() const
{
  core.validate();
  if (!isDigest(security.policyDigest) || !isDigest(planDigest)) {
    throw std::invalid_argument("native sealed plan security identity is invalid");
  }
  std::set<std::string> seen;
  for (const auto& grant : grants) {
    if (grant.provider.empty() || grant.role.empty() || grant.grantName.empty() ||
        grant.recipient != grant.provider || grant.grantName.front() != '/' || !isDigest(grant.grantDigest) ||
        !seen.insert(grant.provider + "\n" + grant.role).second) {
      throw std::invalid_argument("native sealed plan grant binding is invalid");
    }
    const auto role = core.assignment.providerByRole.find(grant.role);
    if (role == core.assignment.providerByRole.end() || role->second != grant.provider) {
      throw std::invalid_argument("native sealed plan grant is outside the assignment");
    }
  }
  if (security.requireProtectedArtifacts && seen.size() != core.executionPlan.roles.size()) {
    throw std::invalid_argument("native sealed plan is missing a protected grant");
  }
  if ((!security.requireProtectedArtifacts && !grants.empty()) ||
      security.requireProtectedArtifacts == (core.protectionEpoch == "plaintext-v1") ||
      nativePlanningDigest(canonicalSealed(*this)) != planDigest) {
    throw std::invalid_argument("native sealed plan security binding was modified");
  }
  if (grantLeaseScope) {
    const auto& scope = *grantLeaseScope;
    if (scope.conversationId.empty() || scope.requesterIdentity != core.requesterIdentity ||
        scope.serviceName != core.executionPlan.serviceName || scope.requestId.empty() ||
        scope.attempt == 0 || !isDigest(scope.planCoreDigest) ||
        !isDigest(scope.scopeDigest) ||
        scope.securityPolicySnapshotDigest != security.policyDigest ||
        scope.protectionEpoch != core.protectionEpoch ||
        scope.expiresAtMs < core.expiresAtMs || scope.providerByRole != core.assignment.providerByRole) {
      throw std::invalid_argument("native sealed grant lease scope is not bound to the plan");
    }
  }
}

namespace {
NativeProviderGrantView grantViewForIdentity(const NativePlacementPlanCore& core,
  const std::string& providerName, const std::string& offerDigest,
  const NativeSecurityPolicySnapshot& security, const std::string& roleName)
{
  core.validate();
  if (!isDigest(security.policyDigest)) {
    throw std::invalid_argument("security policy digest is invalid");
  }
  const auto role = roleName.empty()
    ? std::find_if(core.assignment.providerByRole.begin(),
                   core.assignment.providerByRole.end(),
                   [&providerName] (const auto& item) { return item.second == providerName; })
    : core.assignment.providerByRole.find(roleName);
  if (role == core.assignment.providerByRole.end()) {
    throw std::invalid_argument("provider is not assigned by the plan");
  }
  if (role->second != providerName) {
    throw std::invalid_argument("requested role is assigned to another Provider");
  }
  if (core.offerDigestByProvider.at(providerName) != offerDigest) {
    throw std::invalid_argument("grant view offer differs from the sealed ACK offer");
  }
  if (security.requireProtectedArtifacts == (core.protectionEpoch == "plaintext-v1")) {
    throw std::invalid_argument("grant view protection epoch disagrees with policy");
  }
  return {providerName, role->first, core.coreDigest, security.policyDigest,
          core.modelDigest, core.graphDigest, core.artifactDigestByRole.at(role->first),
          core.requesterIdentity, core.requestId, core.attempt, core.artifacts.manifestDigest,
          core.protectionEpoch, core.expiresAtMs};
}

} // namespace

NativePlacementPlanCore NativePlanSealer::sealCore(
  const NativePlanningSnapshot& snapshot,
  const NativePlacementProposal& proposal,
  const NativePlanSealingInputs& inputs)
{
  snapshot.validate();
  proposal.strategy.validate();
  if (proposal.requestId != snapshot.requestId || proposal.attempt != snapshot.attempt ||
      proposal.modelDigest != snapshot.model.contentDigest ||
      proposal.graphDigest != snapshot.graph.graphDigest ||
      !isDigest(proposal.candidateDigest) || proposal.executionPlan.roles.empty()) {
    throw std::invalid_argument("placement proposal is not bound to the snapshot");
  }
  if (proposal.executionPlan.roles.size() != proposal.assignment.providerByRole.size()) {
    throw std::invalid_argument("placement proposal does not cover plan roles");
  }
  if (proposal.assignment.providerByRole.size() != proposal.executionPlan.roles.size()) {
    throw std::invalid_argument("placement proposal does not cover plan roles");
  }
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (nowMs < 0 || inputs.expiresAtMs <= static_cast<std::uint64_t>(nowMs)) {
    throw std::invalid_argument("native sealing request expiry is not active");
  }
  for (const auto& item : proposal.assignment.providerByRole) {
    const auto found = std::find_if(snapshot.offers.begin(), snapshot.offers.end(),
      [&item] (const auto& offer) { return offer.provider == item.second; });
    if (found == snapshot.offers.end()) {
      throw std::invalid_argument("placement proposal provider is not in ACK offers");
    }
  }
  NativePlacementPlanCore core;
  core.requestId = snapshot.requestId;
  core.attempt = snapshot.attempt;
  core.modelDigest = snapshot.model.contentDigest;
  core.sourceContentDigest = snapshot.model.contentDigest;
  core.graphDigest = snapshot.graph.graphDigest;
  core.ackClosedDigest = snapshot.ackClosedDigest;
  core.candidateDigest = proposal.candidateDigest;
  core.strategy = proposal.strategy;
  core.executionPlan = proposal.executionPlan;
  core.assignment = proposal.assignment;
  for (const auto& offer : snapshot.offers) {
    const auto assigned = std::find_if(core.assignment.providerByRole.begin(),
      core.assignment.providerByRole.end(), [&offer] (const auto& item) {
        return item.second == offer.provider;
      });
    if (assigned != core.assignment.providerByRole.end()) {
      if (!isDigest(offer.offerDigest)) {
        throw std::invalid_argument("ACK offer digest is not canonical");
      }
      core.offerDigestByProvider.emplace(offer.provider, offer.offerDigest);
    }
  }
  core.artifacts = inputs.artifacts;
  core.artifactDigestByRole = inputs.artifacts.artifactDigestByRole;
  core.requesterIdentity = inputs.requesterIdentity;
  core.protectionEpoch = inputs.protectionEpoch;
  core.expiresAtMs = inputs.expiresAtMs;
  core.assemblyByRole = inputs.assemblyByRole;
  core.requestContractDigest = inputs.requestContractDigest;
  core.generationContract = inputs.generationContract;
  for (const auto& item : core.assemblyByRole) {
    if (item.second.adapterId != snapshot.model.adapterId ||
        item.second.adapterVersion != snapshot.model.adapterVersion) {
      throw std::invalid_argument("native assembly adapter differs from inspected model");
    }
  }
  core.coreDigest = nativePlanningDigest(canonicalCore(core));
  core.validate();
  return core;
}


NativePlacementPlanCore NativePlanSealer::sealCore(
  const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
  const NativeRolePlacementProposalV3& proposal, const NativeExecutionPlan& executionPlan,
  const std::vector<NativeAdmittedOfferV3>& offers, const std::string& expectedAckClosedDigest,
  const NativePlanSealingInputs& inputs)
{
  std::vector<NativeSelectionRoleV3> prepared;
  for (const auto& item : inputs.assemblyByRole) prepared.push_back(item.second);
  NativeRequestPreparation::validateRoles(model, candidate, prepared);
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (now < 0) throw std::invalid_argument("invalid sealing clock");
  validateNativeRolePlacement(proposal, prepared, offers, static_cast<std::uint64_t>(now));
  auto published = proposal;
  published.roles = NativeRequestPreparation::bindPublishedRoles(model, candidate, proposal.roles, inputs.artifacts);
  // A refreshed recipe invalidates an old exact-reuse proof. Recheck the same
  // placement against current admitted capabilities before sealing the new root.
  validateNativeRolePlacement(published, published.roles, offers, static_cast<std::uint64_t>(now));
  const auto& context = proposal.context;
  std::vector<std::string> selectedOrder;
  for (const auto& role : proposal.roles) selectedOrder.push_back(role.selectedRole);
  if (executionPlan.roles != selectedOrder)
    throw std::invalid_argument("execution role order differs from the V3 proposal");
  if (proposal.ackClosedDigest != expectedAckClosedDigest || !isDigest(expectedAckClosedDigest) ||
      context.requestId != inputs.artifacts.requestId || context.attempt != inputs.artifacts.attempt ||
      context.modelDigest != model.descriptor.intentDigest() || context.graphDigest != model.graph.graphDigest ||
      inputs.artifacts.modelDigest != model.descriptor.contentDigest ||
      context.deadlineMs != inputs.expiresAtMs || executionPlan.serviceName != context.serviceName ||
      executionPlan.modelName != model.descriptor.modelName ||
      inputs.artifacts.canonicalGraphDigest != model.canonicalGraphDigest || !isDigest(candidate.candidateDigest))
    throw std::invalid_argument("V3 sealing inputs differ from the request or inspected model");
  NativePlacementPlanCore core;
  core.requestId = context.requestId; core.attempt = context.attempt;
  core.modelDigest = context.modelDigest; core.graphDigest = context.graphDigest;
  core.sourceContentDigest = model.descriptor.contentDigest;
  core.ackClosedDigest = proposal.ackClosedDigest; core.candidateDigest = candidate.candidateDigest;
  core.strategy = proposal.strategy; core.executionPlan = executionPlan;
  core.assignment.providerByRole = proposal.providerByRole;
  core.offerDigestByProvider = proposal.offerDigestByProvider;
  core.artifacts = inputs.artifacts; core.artifactDigestByRole = inputs.artifacts.artifactDigestByRole;
  core.requesterIdentity = inputs.requesterIdentity; core.protectionEpoch = inputs.protectionEpoch;
  core.expiresAtMs = inputs.expiresAtMs; core.requestContractDigest = inputs.requestContractDigest;
  core.generationContract = inputs.generationContract;
  for (const auto& role : published.roles) {
    if (!core.assemblyByRole.emplace(role.selectedRole, role).second)
      throw std::invalid_argument("duplicate selected V3 execution role");
  }
  core.coreDigest = nativePlanningDigest(canonicalCore(core));
  core.validate();
  return core;
}

NativeProviderGrantView NativePlanSealer::grantView(
  const NativePlacementPlanCore& core,
  const NativeProviderPlanningView& provider,
  const NativeSecurityPolicySnapshot& security, const std::string& roleName)
{
  provider.validate();
  return grantViewForIdentity(core, provider.provider, provider.offerDigest, security, roleName);
}

NativeProviderGrantView NativePlanSealer::grantView(const NativePlacementPlanCore& core,
  const NativeAdmittedOfferV3& provider, const NativeSecurityPolicySnapshot& security,
  const std::string& roleName)
{
  const auto& offer = provider.observation();
  if (offer.requestId != core.requestId || offer.attempt != core.attempt ||
      offer.modelDigest != core.modelDigest || offer.service != core.executionPlan.serviceName ||
      (offer.graphDigest != core.graphDigest && offer.graphDigest != "sha256:" + std::string(64, '0')) ||
      offer.expiresAtMs < core.expiresAtMs || !offer.status)
    throw std::invalid_argument("grant offer is not bound to this request");
  return grantViewForIdentity(core, offer.provider, offer.offerDigest, security, roleName);
}

NativeSealedPlan NativePlanSealer::finalizeSecurity(
  const NativePlacementPlanCore& core,
  const std::vector<NativeGrantBinding>& grants,
  const NativeSecurityPolicySnapshot& security,
  std::optional<NativeGrantLeaseScope> grantLeaseScope)
{
  core.validate();
  if (!isDigest(security.policyDigest)) {
    throw std::invalid_argument("security policy digest is invalid");
  }
  NativeSealedPlan sealed{core, grants, security, {}, std::move(grantLeaseScope)};
  sealed.planDigest = nativePlanningDigest(canonicalSealed(sealed));
  sealed.validate();
  return sealed;
}

NativeSelectionProjectionV3 NativePlanSealer::project(
  const NativeSealedPlan& sealed, const std::string& provider,
  const NativeRoleProjectionInputs& inputs)
{
  sealed.validate();
  const auto requestedRole = inputs.executionRole.roleId;
  if (requestedRole.empty()) {
    throw std::invalid_argument("projection execution role is missing");
  }
  const auto assignment = sealed.core.assignment.providerByRole.find(requestedRole);
  if (assignment == sealed.core.assignment.providerByRole.end()) {
    throw std::invalid_argument("projection role is not present in sealed plan");
  }
  if (assignment->second != provider) {
    throw std::invalid_argument("projection role is assigned to another Provider");
  }
  const auto grant = std::find_if(sealed.grants.begin(), sealed.grants.end(),
    [&provider, &assignment] (const auto& item) {
      return item.provider == provider && item.role == assignment->first;
    });
  NativeSelectionProjectionV3 projection;
  projection.provider = provider;
  projection.requestId = sealed.core.requestId;
  projection.attempt = sealed.core.attempt;
  projection.planCoreDigest = sealed.core.coreDigest;
  projection.planDigest = sealed.planDigest;
  projection.ackClosedDigest = sealed.core.ackClosedDigest;
  projection.offerDigest = sealed.core.offerDigestByProvider.at(provider);
  projection.securityPolicySnapshotDigest = sealed.security.policyDigest;
  projection.selectedRole = sealed.core.assemblyByRole.at(assignment->first);
  projection.assembly = projection.selectedRole;
  projection.executionRole = inputs.executionRole;
  projection.dataflow = inputs.dataflow;
  projection.deviceBinding = inputs.deviceBinding;
  projection.deadlineMs = sealed.core.expiresAtMs;
  projection.requestContractDigest = sealed.core.requestContractDigest;
  projection.generationContract = sealed.core.generationContract;
  projection.groupCapabilityV1 = inputs.groupCapabilityV1;
  projection.conversationStateReference = inputs.conversationStateReference;
  projection.conversationTurnBinding = inputs.conversationTurnBinding;
  projection.hasGrantBinding = grant != sealed.grants.end();
  if (grant != sealed.grants.end()) {
    projection.grantName = grant->grantName;
    projection.grantDigest = grant->grantDigest;
  }
  projection.plan = sealed.core.executionPlan;
  projection.grantLeaseScope = sealed.grantLeaseScope;
  nativeSelectionProjectionV3ToJson(projection);
  return projection;
}

std::vector<std::uint8_t> NativePlanSealer::encode(
  const NativeSelectionProjectionV3& projection)
{
  const auto wire = nativeSelectionProjectionV3ToJson(projection);
  // Core's external assignment boundary is 4 MiB. Check the actual complete
  // serialization before publication, not an endpoint-count estimate.
  if (wire.size() > (4U << 20))
    throw std::invalid_argument("V3 Selection exceeds external assignment wire limit");
  return {wire.begin(), wire.end()};
}

} // namespace ndnsf::di
