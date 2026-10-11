#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/FixedAssignmentWork.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV4.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePlacementObservationExpired.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePhaseTiming.hpp"

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

std::string calculateProbeScopeDigest(const NativeOfferBindingContext& context)
{
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", context.requestId}, {"attempt", context.attempt},
    {"requester_identity", context.requesterIdentity},
    {"deadline_ms", context.deadlineMs},
    {"service", context.serviceName}, {"model_identity_hash", context.modelDigest},
    {"graph_digest", context.graphDigest},
    {"candidate_set_digest", context.candidateSetDigest},
    {"candidate_artifact_set_digest", context.candidateArtifactSetDigest},
    {"rtt_pairs", context.allowedRttPairs},
    {"goodput_edges", context.allowedGoodputEdges}}));
}

std::string canonicalCore(const NativePlacementPlanCore& core)
{
  auto roles = NativeJson::array();
  if (!core.fixedCoreOwner) for (const auto& role : core.executionPlan.roles) {
    const auto assembly = core.roleContracts().find(role);
    if (assembly == core.roleContracts().end()) throw std::invalid_argument("missing sealed assembly role");
    roles.push_back(nativeAssemblyJson(assembly->second));
  }
  const auto strategy = nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"name", core.strategy.name}, {"version", core.strategy.version},
    {"state", core.strategy.configurationDigest}}));
  auto value = NativeJson{
    {"request_id", core.requestId}, {"attempt", core.attempt},
    {"model_digest", core.modelDigest}, {"graph_digest", core.graphDigest},
    {"roles", roles}, {"provider_by_role", core.assignment.providerByRole},
    {"dependencies", nativeDependenciesJson(core.executionPlan.dependencies)},
    {"ack_closed_digest", core.ackClosedDigest}, {"strategy_digest", strategy},
    {"candidate_digest", core.candidateDigest}, {"request_contract_digest", core.requestContractDigest},
    {"generation_contract", core.generationContract.enabled ? nativeGenerationJson(core.generationContract) : NativeJson(nullptr)}};
  if (!core.placementObservationSnapshotDigest.empty()) {
    value["placement_observation_snapshot_digest"] = core.placementObservationSnapshotDigest;
  }
  if (core.placementDecisionV4)
    value["placement_decision_v4"] = *core.placementDecisionV4;
  if (core.fixedCoreOwner) {
    // Keep the original canonical root bytes, without reconstructing the
    // immutable role array. Object iteration uses the same sorted key order.
    std::string result = "{";
    bool first = true;
    for (auto item = value.begin(); item != value.end(); ++item) {
      if (!first) result += ',';
      first = false;
      result += nativeCanonicalJson(NativeJson(item.key()));
      result += ':';
      result += item.key() == "roles" ? core.fixedCoreOwner->canonicalRoles() : nativeCanonicalJson(item.value());
    }
    result += '}';
    return result;
  }
  return nativeCanonicalJson(value);
}

void validatePlacementDecisionV4(const NativeJson& decision,
                                 const std::string& snapshotDigest)
{
  if (!decision.is_object() ||
      decision.value("schema", std::string{}) != "ndnsf-placement-decision-v4" ||
      decision.value("policy_version", std::string{}) != "role-dag-beam-v1" ||
      !decision.contains("snapshot_digest") ||
      !decision.contains("status") || !decision.at("status").is_string() ||
      !decision.contains("fallback_reason") ||
      !decision.at("fallback_reason").is_string())
    throw std::invalid_argument("V4 placement decision identity is incomplete");
  const auto& boundSnapshot = decision.at("snapshot_digest");
  if (snapshotDigest.empty()) {
    if (!boundSnapshot.is_null() ||
        decision.at("status").get<std::string>() != "BASELINE_FALLBACK")
      throw std::invalid_argument("V4 baseline decision has a foreign snapshot binding");
  }
  else if (!boundSnapshot.is_string() ||
           boundSnapshot.get<std::string>() != snapshotDigest ||
           !isDigest(boundSnapshot.get<std::string>())) {
    throw std::invalid_argument("V4 placement decision snapshot digest differs from core");
  }
  const auto status = decision.at("status").get<std::string>();
  const auto reason = decision.at("fallback_reason").get<std::string>();
  if ((status == "SCORED" && !reason.empty()) ||
      (status == "BASELINE_FALLBACK" && reason.empty()) ||
      (status != "SCORED" && status != "BASELINE_FALLBACK"))
    throw std::invalid_argument("V4 placement decision status/reason is inconsistent");
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

namespace {
// The owned sealCore constructors may use this only immediately after setting
// their digest. Public callers must also verify the digest against the content.
void validateCoreFields(const NativePlacementPlanCore& core, bool validateAssemblies = true,
                        bool validateArtifacts = true)
{
  if (core.fixedCoreOwner && (!core.assemblyByRole.empty() || !core.fixedCoreOwner->matches(core)))
    throw std::invalid_argument("fixed core owner differs from its value bindings");
  if (validateArtifacts) core.artifacts.validate();
  if (core.artifacts.requestId != core.requestId || core.artifacts.attempt != core.attempt ||
      !isDigest(core.sourceContentDigest) || core.artifacts.modelDigest != core.sourceContentDigest ||
      core.artifacts.graphDigest != core.graphDigest ||
      !isDigest(core.artifacts.canonicalGraphDigest) ||
      core.artifacts.artifactDigestByRole != core.artifactDigestByRole ||
      core.requesterIdentity.empty() || core.requesterIdentity.front() != '/' ||
      core.protectionEpoch.empty() || core.expiresAtMs == 0) {
    throw std::invalid_argument("native sealed artifact/request binding is incomplete");
  }
  if (core.requestId.empty() || core.attempt == 0 || !isDigest(core.modelDigest) ||
      !isDigest(core.graphDigest) || !isDigest(core.ackClosedDigest) ||
      !isDigest(core.candidateDigest) || core.executionPlan.roles.empty() ||
      (!core.placementObservationSnapshotDigest.empty() &&
       !isDigest(core.placementObservationSnapshotDigest)) ||
      !isDigest(core.coreDigest)) {
    throw std::invalid_argument("native placement plan core identity is incomplete");
  }
  core.strategy.validate();
  if (core.placementDecisionV4)
    validatePlacementDecisionV4(*core.placementDecisionV4,
                                core.placementObservationSnapshotDigest);
  std::set<std::string> roles(core.executionPlan.roles.begin(), core.executionPlan.roles.end());
  if (roles.size() != core.executionPlan.roles.size() ||
      core.assignment.providerByRole.size() != roles.size() ||
      core.artifactDigestByRole.size() != roles.size() ||
      core.roleContracts().size() != roles.size() ||
      core.offerDigestByProvider.empty()) {
    throw std::invalid_argument("native placement plan core role cover is incomplete");
  }
  for (const auto& role : roles) {
    const auto provider = core.assignment.providerByRole.find(role);
    const auto artifact = core.artifactDigestByRole.find(role);
    if (provider == core.assignment.providerByRole.end() || provider->second.empty() ||
        artifact == core.artifactDigestByRole.end() || !isDigest(artifact->second)) {
      throw std::invalid_argument("native placement plan core role binding is invalid");
    }
    const auto offer = core.offerDigestByProvider.find(provider->second);
    if (offer == core.offerDigestByProvider.end() || !isDigest(offer->second)) {
      throw std::invalid_argument("native placement plan core offer binding is invalid");
    }
    const auto assembly = core.roleContracts().find(role);
    if (assembly == core.roleContracts().end() || assembly->second.selectedRole != role ||
        assembly->second.artifactDigest != artifact->second ||
        assembly->second.graphDigest != core.artifacts.canonicalGraphDigest ||
        assembly->second.modelManifestDigest != core.artifacts.manifestDigest ||
        assembly->second.protectionEpoch != core.protectionEpoch) {
      throw std::invalid_argument("native sealed assembly differs from authenticated artifact context");
    }
    if (assembly->second.role != role &&
        assembly->second.role + "#" + std::to_string(assembly->second.rank) != role) {
      throw std::invalid_argument("native assembly logical role/rank differs from assignment");
    }
    if (validateAssemblies && !core.fixedCoreOwner) validateNativeAssembly(assembly->second);
  }
}
} // namespace

NativeFixedPlacementCoreV1::NativeFixedPlacementCoreV1(const NativePlacementPlanCore& core)
  : m_roles(core.roleContracts()), m_execution(core.executionPlan),
    m_providers(core.assignment.providerByRole), m_model(core.modelDigest),
    m_source(core.sourceContentDigest), m_graph(core.graphDigest), m_candidate(core.candidateDigest)
{
  ++detail::fixedAssignmentWork().placementBuilds;
  for (auto& dependency : m_execution.dependencies) {
    if (dependency.operationKind == "TOKEN_FEEDBACK") dependency.keyScope = "fixed-token-feedback";
    dependency.expectedSegments = dependency.expectedBytes = 0;
  }
  const auto& generation = core.generationContract;
  m_generation.enabled = generation.enabled;
  m_generation.mode = generation.mode;
  m_generation.tokenInputName = generation.tokenInputName;
  m_generation.stateInputNames = generation.stateInputNames;
  m_generation.stateOutputNames = generation.stateOutputNames;
  m_generation.stateSuccessorMap = generation.stateSuccessorMap;
  m_generation.positionInputPolicy = generation.positionInputPolicy;
  m_generation.attentionMaskInputName = generation.attentionMaskInputName;
  m_generation.positionIdsInputName = generation.positionIdsInputName;
  m_generation.cachePositionInputName = generation.cachePositionInputName;
  m_generation.eosTokenIds = generation.eosTokenIds;
  m_generation.tokenizerDigest = generation.tokenizerDigest;
  m_generation.streamingOperationStride = generation.streamingOperationStride;
  m_canonicalRoles = "[";
  for (const auto& name : m_execution.roles) {
    const auto wire = nativeCanonicalJson(nativeAssemblyJson(m_roles.at(name)));
    if (m_canonicalRoles.size() > 1) m_canonicalRoles += ',';
    m_canonicalRoles += wire;
    m_roleDigests.emplace(name, nativePlanningDigest(wire));
    const auto& role = m_roles.at(name);
    if (role.mergeKind != "NATIVE_POSTPROCESS" &&
        nativePlanningDigest(canonicalNativeOnnxRecipeJson(role)) != role.recipeDigest)
      m_canonicalRecipes = false;
  }
  m_canonicalRoles += ']';
}

bool NativeFixedPlacementCoreV1::matches(const NativePlacementPlanCore& core) const
{
  const auto& execution = core.executionPlan;
  if (m_model != core.modelDigest || m_source != core.sourceContentDigest || m_graph != core.graphDigest ||
      m_candidate != core.candidateDigest || m_providers != core.assignment.providerByRole ||
      std::tie(m_execution.version, m_execution.serviceName, m_execution.modelName, m_execution.modelFamily,
        m_execution.modelFormat, m_execution.plannerKind, m_execution.executionPolicy, m_execution.streamingOperationStride,
        m_execution.roles) !=
      std::tie(execution.version, execution.serviceName, execution.modelName, execution.modelFamily,
        execution.modelFormat, execution.plannerKind, execution.executionPolicy, execution.streamingOperationStride,
        execution.roles) ||
      !nativeFixedGenerationInterfacesEqualV1(m_generation, core.generationContract) ||
      m_execution.dependencies.size() != execution.dependencies.size() || m_roles.size() != core.roleContracts().size())
    return false;
  for (std::size_t index = 0; index < execution.dependencies.size(); ++index)
    if (!nativeFixedDependencyContractsEqualV1(m_execution.dependencies[index], execution.dependencies[index])) return false;
  if (&core.roleContracts() == &m_roles) return true;
  for (const auto& [name, role] : m_roles) {
    const auto found = core.roleContracts().find(name);
    if (found == core.roleContracts().end() || role.materializedRole != found->second.materializedRole ||
        !nativeSelectionRoleContractsEqualV3(role, found->second)) return false;
  }
  return true;
}

NativePlacementPlanCore NativePlanSealer::shareFixedCore(NativePlacementPlanCore core,
  std::shared_ptr<const NativeFixedPlacementCoreV1> previous)
{
  if (core.fixedCoreOwner) { core.validate(); return core; }
  if (previous && previous->matches(core)) {
    core.fixedCoreOwner = std::move(previous);
    core.assemblyByRole.clear();
    core.validate();
    return core;
  }
  core.validate();
  // Copy the public values into independent storage; moving them could retain
  // caller-owned mutable node aliases, even inside a const outer allocation.
  core.fixedCoreOwner = std::shared_ptr<const NativeFixedPlacementCoreV1>(new const NativeFixedPlacementCoreV1(core));
  core.assemblyByRole.clear();
  return core;
}

void NativePlacementPlanCore::validate() const
{
  validateCoreFields(*this);
  if (nativePlanningDigest(canonicalCore(*this)) != coreDigest) {
    throw std::invalid_argument("native plan core was modified after sealing");
  }
}

namespace {
// Call only after validating this sealed value's unchanged Core. Keep every
// security check here shared with the fully checked public validation entry.
void validateSecurityForValidatedCore(const NativeSealedPlan& sealed)
{
  const auto& core = sealed.core;
  const auto& security = sealed.security;
  const auto& grants = sealed.grants;
  const auto& planDigest = sealed.planDigest;
  const auto& grantLeaseScope = sealed.grantLeaseScope;
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
      nativePlanningDigest(canonicalSealed(sealed)) != planDigest) {
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
} // namespace

void NativeSealedPlan::validate() const
{
  core.validate();
  validateSecurityForValidatedCore(*this);
}

NativeOwnedSealedPlan::NativeOwnedSealedPlan(std::shared_ptr<const NativeSealedPlan> value)
  : m_value(std::move(value))
{}

const NativeSealedPlan& NativeOwnedSealedPlan::value() const
{
  if (!m_value) throw std::logic_error("owned sealed plan is empty");
  return *m_value;
}

NativeSealedPlan NativeOwnedSealedPlan::snapshot() const
{
  auto result = value();
  if (result.core.fixedCoreOwner) {
    result.core.assemblyByRole = result.core.roleContracts();
    result.core.fixedCoreOwner.reset();
  }
  return result;
}

NativeSealedPlan NativeOwnedSealedPlan::invocationContext() const
{
  return value();
}

namespace {
NativeProviderGrantView grantViewForValidatedIdentity(const NativePlacementPlanCore& core,
  const std::string& providerName, const std::string& offerDigest,
  const NativeSecurityPolicySnapshot& security, const std::string& roleName)
{
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

NativeProviderGrantView grantViewForIdentity(const NativePlacementPlanCore& core,
  const std::string& providerName, const std::string& offerDigest,
  const NativeSecurityPolicySnapshot& security, const std::string& roleName)
{
  core.validate();
  return grantViewForValidatedIdentity(core, providerName, offerDigest, security, roleName);
}

void validateGrantOfferBinding(const NativePlacementPlanCore& core,
  const NativeAdmittedOfferV3& provider)
{
  const auto& offer = provider.observation();
  if (offer.requestId != core.requestId || offer.attempt != core.attempt ||
      offer.modelDigest != core.modelDigest || offer.service != core.executionPlan.serviceName ||
      (offer.graphDigest != core.graphDigest && offer.graphDigest != "sha256:" + std::string(64, '0')) ||
      offer.expiresAtMs < core.expiresAtMs || !offer.status)
    throw std::invalid_argument("grant offer is not bound to this request");
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
  for (const auto& item : core.roleContracts()) {
    if (item.second.adapterId != snapshot.model.adapterId ||
        item.second.adapterVersion != snapshot.model.adapterVersion) {
      throw std::invalid_argument("native assembly adapter differs from inspected model");
    }
  }
  core.coreDigest = nativePlanningDigest(canonicalCore(core));
  validateCoreFields(core);
  return core;
}

void NativePlanSealer::validateObservationSnapshotBinding(
  const NativeOfferBindingContext& context,
  const NativePlanSealingInputs& inputs,
  const std::string& ackClosedDigest,
  std::uint64_t nowMs)
{
  const auto& snapshot = context.placementObservationSnapshot;
  if (context.placementObservationSnapshotDigest.empty()) {
    if (!inputs.placementObservationSnapshotDigest.empty() || !snapshot.is_null())
      throw std::invalid_argument("unbound placement observation snapshot");
    return;
  }
  if (context.requestId.empty() || context.attempt == 0 ||
      context.requesterIdentity.empty() || context.serviceName.empty() ||
      !isDigest(context.modelDigest) || !isDigest(context.graphDigest) ||
      !isDigest(ackClosedDigest) || !isDigest(context.candidateSetDigest) ||
      !isDigest(context.candidateArtifactSetDigest) ||
      !isDigest(context.probeScopeDigest) ||
      context.probeScopeDigest != calculateProbeScopeDigest(context) ||
      context.candidateProviders.empty() ||
      context.candidateProviders.size() > 16 ||
      !std::is_sorted(context.candidateProviders.begin(), context.candidateProviders.end()) ||
      std::adjacent_find(context.candidateProviders.begin(), context.candidateProviders.end()) !=
        context.candidateProviders.end() || context.candidateArtifacts.empty() ||
      context.candidateArtifacts.size() > MAX_NATIVE_PLACEMENT_CANDIDATE_ARTIFACTS)
    throw std::invalid_argument("placement observation request scope is incomplete");
  const auto matches = [&] (const char* key, const std::string& value) {
    return snapshot.contains(key) && snapshot.at(key).is_string() &&
      snapshot.at(key).get<std::string>() == value;
  };
  if (!snapshot.is_object() || !isDigest(context.placementObservationSnapshotDigest) ||
      inputs.placementObservationSnapshotDigest !=
        context.placementObservationSnapshotDigest ||
      nativePlanningDigest(nativePlacementObservationCanonicalJson(
        snapshot, context.preparedPlanningTemplate.get())) !=
        context.placementObservationSnapshotDigest ||
      !matches("schema", "ndnsf-placement-observation-snapshot-v4") ||
      !matches("request_id", context.requestId) ||
      !matches("requester_identity", context.requesterIdentity) ||
      !matches("service", context.serviceName) ||
      !matches("model_digest", context.modelDigest) ||
      !matches("graph_digest", context.graphDigest) ||
      !matches("ack_closed_digest", ackClosedDigest) ||
      !matches("candidate_set_digest", context.candidateSetDigest) ||
      !matches("candidate_artifact_set_digest", context.candidateArtifactSetDigest) ||
      !matches("probe_scope_digest", context.probeScopeDigest) ||
      !snapshot.contains("attempt") || !snapshot.at("attempt").is_number_unsigned() ||
      snapshot.at("attempt").get<std::uint64_t>() != context.attempt ||
      !snapshot.contains("provider_roster") ||
      snapshot.at("provider_roster") != NativeJson(context.candidateProviders) ||
      !snapshot.contains("rtt_pairs") ||
      snapshot.at("rtt_pairs") != NativeJson(context.allowedRttPairs) ||
      !snapshot.contains("goodput_edges") ||
      snapshot.at("goodput_edges") != NativeJson(context.allowedGoodputEdges) ||
      !snapshot.contains("candidate_artifacts") ||
      nativeCanonicalJson(snapshot.at("candidate_artifacts")) !=
        nativeCanonicalJson(NativeJson(context.candidateArtifacts)) ||
      !snapshot.contains("prepared_role_graph") ||
      !snapshot.at("prepared_role_graph").is_array() ||
      snapshot.at("prepared_role_graph").empty() ||
      !snapshot.contains("provider_offers") ||
      !snapshot.at("provider_offers").is_array() ||
      snapshot.at("provider_offers").size() != context.candidateProviders.size() ||
      !snapshot.contains("requester_provider_samples") ||
      !snapshot.at("requester_provider_samples").is_array() ||
      snapshot.at("requester_provider_samples").size() !=
        context.candidateProviders.size() ||
      !snapshot.contains("plan_deadline_ms") ||
      !snapshot.at("plan_deadline_ms").is_number_unsigned() ||
      snapshot.at("plan_deadline_ms").get<std::uint64_t>() != context.deadlineMs ||
      !snapshot.contains("captured_at_ms") ||
      !snapshot.at("captured_at_ms").is_number_unsigned() ||
      snapshot.at("captured_at_ms").get<std::uint64_t>() > nowMs ||
      !snapshot.contains("expires_at_ms") ||
      !snapshot.at("expires_at_ms").is_number_unsigned() ||
      snapshot.at("expires_at_ms").get<std::uint64_t>() > context.deadlineMs ||
      inputs.expiresAtMs <= snapshot.at("captured_at_ms").get<std::uint64_t>() ||
      inputs.expiresAtMs > context.deadlineMs || inputs.expiresAtMs <= nowMs)
    throw std::invalid_argument("placement observation snapshot binding is invalid");

  const auto& providerOffers = snapshot.at("provider_offers");
  for (std::size_t i = 0; i < context.candidateProviders.size(); ++i) {
    const auto& offer = providerOffers[i];
    if (!offer.is_object() || offer.value("provider", std::string{}) !=
          context.candidateProviders[i] ||
        offer.value("signer_identity", std::string{}) !=
          context.candidateProviders[i] ||
        !isDigest(offer.value("offer_digest", std::string{})) ||
        !isDigest(offer.value("ack_wire_digest", std::string{})) ||
        offer.value("signer_key_locator", std::string{}).empty())
      throw std::invalid_argument("placement observation provider roster binding is invalid");
  }
  // Structural, scope and roster errors retain their original rejection.
  // Only an otherwise valid expired snapshot permits observation-free replanning.
  if (snapshot.at("expires_at_ms").get<std::uint64_t>() <= nowMs)
    throw detail::NativePlacementObservationExpired();
}


NativePlacementPlanCore NativePlanSealer::sealCore(
  const NativeInspectedModel& model, const NativeSplitCandidate& candidate,
  const NativeRolePlacementProposalV3& proposal, const NativeExecutionPlan& executionPlan,
  const std::vector<NativeAdmittedOfferV3>& offers, const std::string& expectedAckClosedDigest,
  const NativePlanSealingInputs& inputs)
{
  auto phaseStarted = detail::nativePhaseStarted();
  if (inputs.borrowedPreparedRoles && !inputs.assemblyByRole.empty())
    throw std::invalid_argument("sealing inputs mix borrowed and value roles");
  std::vector<NativeSelectionRoleV3> preparedStorage;
  if (!inputs.borrowedPreparedRoles)
    for (const auto& item : inputs.assemblyByRole) preparedStorage.push_back(item.second);
  const auto& prepared = inputs.borrowedPreparedRoles ? *inputs.borrowedPreparedRoles : preparedStorage;
  NativeRequestPreparation::validateRoles(model, candidate, prepared);
  detail::logNativeWorkPhase(proposal.context.requestId, "seal_prepared_validation", phaseStarted);
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (now < 0) throw std::invalid_argument("invalid sealing clock");
  phaseStarted = detail::nativePhaseStarted();
  if (inputs.previousFixedCore)
    validateNativeRolePlacementWithFixedCore(proposal, prepared, *inputs.previousFixedCore, offers,
      static_cast<std::uint64_t>(now));
  else validateNativeRolePlacement(proposal, prepared, offers, static_cast<std::uint64_t>(now));
  detail::logNativeWorkPhase(proposal.context.requestId, "seal_original_placement_validation", phaseStarted);
  phaseStarted = detail::nativePhaseStarted();
  const auto& context = proposal.context;
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
  core.placementObservationSnapshotDigest = inputs.placementObservationSnapshotDigest;
  core.placementDecisionV4 = proposal.placementDecisionV4;
  const auto& previous = inputs.previousFixedCore;
  bool reusePublished = previous && NativeRequestPreparation::canReusePublishedRolesForCheckedCandidate(
    model, candidate, proposal.roles, inputs.artifacts, *previous);
  if (reusePublished) {
    core.fixedCoreOwner = previous;
    if (!previous->matches(core)) {
      core.fixedCoreOwner.reset();
      reusePublished = false;
    }
  }
  std::optional<NativeRolePlacementProposalV3> published;
  if (!reusePublished) {
    published.emplace(proposal);
    published->roles = NativeRequestPreparation::bindPublishedRolesForCheckedCandidate(
      model, candidate, proposal.roles, inputs.artifacts);
  }
  detail::logNativeWorkPhase(context.requestId, "seal_publication_rebind", phaseStarted);
  phaseStarted = detail::nativePhaseStarted();
  if (reusePublished)
    validateNativePublishedRolePlacement(proposal, *previous, offers, static_cast<std::uint64_t>(now));
  else validateNativeRolePlacement(*published, published->roles, offers, static_cast<std::uint64_t>(now));
  detail::logNativeWorkPhase(context.requestId, "seal_published_placement_validation", phaseStarted);
  phaseStarted = detail::nativePhaseStarted();
  std::vector<std::string> selectedOrder;
  for (const auto& role : proposal.roles) selectedOrder.push_back(role.selectedRole);
  if (executionPlan.roles != selectedOrder)
    throw std::invalid_argument("execution role order differs from the V3 proposal");
  if (proposal.ackClosedDigest != expectedAckClosedDigest || !isDigest(expectedAckClosedDigest) ||
      context.requestId != inputs.artifacts.requestId || context.attempt != inputs.artifacts.attempt ||
      context.modelDigest != model.descriptor.intentDigest() || context.graphDigest != model.graph.graphDigest ||
      inputs.artifacts.modelDigest != model.descriptor.contentDigest ||
      (inputs.placementObservationSnapshotDigest.empty() &&
       inputs.expiresAtMs != context.deadlineMs) || executionPlan.serviceName != context.serviceName ||
      executionPlan.modelName != model.descriptor.modelName ||
      inputs.artifacts.canonicalGraphDigest != model.canonicalGraphDigest || !isDigest(candidate.candidateDigest))
    throw std::invalid_argument("V3 sealing inputs differ from the request or inspected model");
  if (!reusePublished) for (const auto& role : published->roles) {
    if (!core.assemblyByRole.emplace(role.selectedRole, role).second)
      throw std::invalid_argument("duplicate selected V3 execution role");
  }
  core.reusedFixedCore = reusePublished;
  core.coreDigest = nativePlanningDigest(canonicalCore(core));
  // Publication binding validated these artifacts and placement validated
  // every published assembly above. Only value copies followed, with no
  // extension callback. Retain every independent Core binding check.
  validateCoreFields(core, false, false);
  detail::logNativeWorkPhase(context.requestId, "seal_core_construct_digest", phaseStarted);
  phaseStarted = detail::nativePhaseStarted();
  // Check freshness at the completed sealing boundary, after local validation
  // and digest work, rather than with the earlier placement timestamp.
  const auto sealedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (sealedAt < 0) throw std::invalid_argument("invalid sealing clock");
  validateObservationSnapshotBinding(context, inputs, proposal.ackClosedDigest,
    static_cast<std::uint64_t>(sealedAt));
  detail::logNativeWorkPhase(context.requestId, "seal_observation_binding", phaseStarted);
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
  validateGrantOfferBinding(core, provider);
  const auto& offer = provider.observation();
  return grantViewForIdentity(core, offer.provider, offer.offerDigest, security, roleName);
}

NativeProviderGrantView NativePlanSealer::grantViewFromValidatedCore(
  const NativePlacementPlanCore& core, const NativeAdmittedOfferV3& provider,
  const NativeSecurityPolicySnapshot& security, const std::string& roleName)
{
  validateGrantOfferBinding(core, provider);
  const auto& offer = provider.observation();
  return grantViewForValidatedIdentity(core, offer.provider, offer.offerDigest, security, roleName);
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
  validateSecurityForValidatedCore(sealed);
  return sealed;
}

NativeOwnedSealedPlan NativePlanSealer::finalizeSecurityOwned(
  const NativePlacementPlanCore& core,
  const std::vector<NativeGrantBinding>& grants,
  const NativeSecurityPolicySnapshot& security,
  std::optional<NativeGrantLeaseScope> grantLeaseScope)
{
  // Copy even a caller-moved lease scope before freezing: moving its map could
  // retain the caller's mutable node aliases. The other inputs are value-copied
  // by the checked constructor, and its local result has no external aliases.
  return NativeOwnedSealedPlan(std::make_shared<const NativeSealedPlan>(
    finalizeSecurity(core, grants, security, grantLeaseScope)));
}

namespace {
// Only the checked public entry points below may call this helper, after
// validating their sealed input. Role/provider and serializer checks remain.
NativeSelectionProjectionV3 makeProjection(
  const NativeSealedPlan& sealed, const std::string& provider,
  const NativeRoleProjectionInputs& inputs,
  const NativeFixedRoleAssignmentV1* fixedAssignment = nullptr)
{
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
  projection.fixedProviderByRole = sealed.core.assignment.providerByRole;
  projection.requestId = sealed.core.requestId;
  projection.attempt = sealed.core.attempt;
  projection.planCoreDigest = sealed.core.coreDigest;
  projection.planDigest = sealed.planDigest;
  projection.ackClosedDigest = sealed.core.ackClosedDigest;
  projection.offerDigest = sealed.core.offerDigestByProvider.at(provider);
  projection.securityPolicySnapshotDigest = sealed.security.policyDigest;
  const auto& roleContract = sealed.core.roleContracts().at(assignment->first);
  if (fixedAssignment) {
    if (fixedAssignment->provider() != provider ||
        !nativeSelectionRoleContractsEqualV3(roleContract, *fixedAssignment->roleOwner()))
      throw std::invalid_argument("cached role differs from checked sealed core");
    projection.fixedRoleOwner = fixedAssignment->roleOwner();
  }
  else if (sealed.core.fixedCoreOwner) {
    projection.fixedRoleOwner = std::shared_ptr<const NativeSelectionRoleV3>(sealed.core.fixedCoreOwner, &roleContract);
  }
  else {
    projection.selectedRole = roleContract;
    projection.assembly = projection.selectedRole;
  }
  projection.compactAssembly = true;
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
  return projection;
}
} // namespace

NativeSelectionProjectionV3 NativePlanSealer::projectInvocation(
  const NativeOwnedSealedPlan& sealed, const std::string& provider,
  const NativeRoleProjectionInputs& inputs, const NativeFixedRoleAssignmentV1* fixedAssignment)
{
  return makeProjection(sealed.value(), provider, inputs, fixedAssignment);
}

NativeSelectionProjectionV3 NativePlanSealer::project(
  const NativeSealedPlan& sealed, const std::string& provider,
  const NativeRoleProjectionInputs& inputs)
{
  sealed.validate();
  auto projection = makeProjection(sealed, provider, inputs);
  nativeSelectionProjectionV3ToJson(projection);
  return projection;
}

std::vector<std::uint8_t> NativePlanSealer::projectAndEncode(
  const NativeSealedPlan& sealed, const std::string& provider,
  const NativeRoleProjectionInputs& inputs)
{
  sealed.validate();
  return encode(makeProjection(sealed, provider, inputs));
}

std::map<std::string, std::vector<std::uint8_t>> NativePlanSealer::projectAndEncodeAll(
  const NativeSealedPlan& sealed,
  const std::map<std::string, NativeRoleProjectionInputs>& inputsByRole)
{
  sealed.validate();
  return projectAndEncodeAllFromValidatedSealed(sealed, inputsByRole);
}

std::map<std::string, std::vector<std::uint8_t>> NativePlanSealer::projectAndEncodeAll(
  const NativeOwnedSealedPlan& sealed,
  const std::map<std::string, NativeRoleProjectionInputs>& inputsByRole)
{
  return projectAndEncodeAllFromValidatedSealed(sealed.value(), inputsByRole);
}

std::map<std::string, std::vector<std::uint8_t>>
NativePlanSealer::projectAndEncodeAllFromValidatedSealed(
  const NativeSealedPlan& sealed,
  const std::map<std::string, NativeRoleProjectionInputs>& inputsByRole)
{
  const auto& assignments = sealed.core.assignment.providerByRole;
  if (inputsByRole.size() != assignments.size())
    throw std::invalid_argument("projection inputs differ from assigned role set");
  std::map<std::string, std::vector<std::uint8_t>> result;
  for (const auto& assignment : assignments) {
    const auto found = inputsByRole.find(assignment.first);
    if (found == inputsByRole.end() || found->second.executionRole.roleId != assignment.first)
      throw std::invalid_argument("projection input key differs from assigned execution role");
    result.emplace(assignment.first,
      encode(makeProjection(sealed, assignment.second, found->second)));
  }
  return result;
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
