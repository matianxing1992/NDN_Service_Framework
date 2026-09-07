#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanSealer.hpp"

#include <algorithm>
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

std::string quote(const std::string& value)
{
  std::ostringstream out;
  out << '"';
  for (const auto c : value) {
    if (c == '"' || c == '\\') out << '\\';
    out << c;
  }
  out << '"';
  return out.str();
}

std::string canonicalCore(const NativePlacementPlanCore& core)
{
  std::ostringstream out;
  out << "request=" << core.requestId << "|attempt=" << core.attempt
      << "|model=" << core.modelDigest << "|graph=" << core.graphDigest
      << "|ack=" << core.ackClosedDigest << "|candidate=" << core.candidateDigest
      << "|strategy=" << core.strategy.name << ':' << core.strategy.version << ':'
      << core.strategy.configurationDigest << "|roles=";
  for (const auto& role : core.executionPlan.roles) out << role << ';';
  out << "|assignment=";
  for (const auto& item : core.assignment.providerByRole) {
    out << item.first << '=' << item.second << ';';
  }
  out << "|artifacts=";
  for (const auto& item : core.artifactDigestByRole) {
    out << item.first << '=' << item.second << ';';
  }
  return out.str();
}

} // namespace

void NativePlacementPlanCore::validate() const
{
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
      artifactDigestByRole.size() != roles.size()) {
    throw std::invalid_argument("native placement plan core role cover is incomplete");
  }
  for (const auto& role : roles) {
    const auto provider = assignment.providerByRole.find(role);
    const auto artifact = artifactDigestByRole.find(role);
    if (provider == assignment.providerByRole.end() || provider->second.empty() ||
        artifact == artifactDigestByRole.end() || !isDigest(artifact->second)) {
      throw std::invalid_argument("native placement plan core role binding is invalid");
    }
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
        grant.recipient.empty() || !isDigest(grant.grantDigest) ||
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
}

NativePlacementPlanCore NativePlanSealer::sealCore(
  const NativePlanningSnapshot& snapshot,
  const NativePlacementProposal& proposal)
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
  core.graphDigest = snapshot.graph.graphDigest;
  core.ackClosedDigest = snapshot.ackClosedDigest;
  core.candidateDigest = proposal.candidateDigest;
  core.strategy = proposal.strategy;
  core.executionPlan = proposal.executionPlan;
  core.assignment = proposal.assignment;
  for (const auto& role : core.executionPlan.roles) {
    core.artifactDigestByRole.emplace(role, nativePlanningDigest("role-artifact|" + role));
  }
  core.coreDigest = nativePlanningDigest(canonicalCore(core));
  core.validate();
  return core;
}

NativeProviderGrantView NativePlanSealer::grantView(
  const NativePlacementPlanCore& core,
  const NativeProviderPlanningView& provider,
  const NativeSecurityPolicySnapshot& security)
{
  core.validate();
  provider.validate();
  if (!isDigest(security.policyDigest)) {
    throw std::invalid_argument("security policy digest is invalid");
  }
  const auto role = std::find_if(core.assignment.providerByRole.begin(),
                                core.assignment.providerByRole.end(),
                                [&provider] (const auto& item) {
                                  return item.second == provider.provider;
                                });
  if (role == core.assignment.providerByRole.end()) {
    throw std::invalid_argument("provider is not assigned by the plan");
  }
  return {provider.provider, role->first, core.coreDigest, security.policyDigest,
          core.modelDigest, core.graphDigest, core.artifactDigestByRole.at(role->first)};
}

NativeSealedPlan NativePlanSealer::finalizeSecurity(
  const NativePlacementPlanCore& core,
  const std::vector<NativeGrantBinding>& grants,
  const NativeSecurityPolicySnapshot& security)
{
  core.validate();
  if (!isDigest(security.policyDigest)) {
    throw std::invalid_argument("security policy digest is invalid");
  }
  NativeSealedPlan sealed{core, grants, security, {}};
  std::ostringstream canonical;
  canonical << canonicalCore(core) << "|policy=" << security.policyDigest;
  for (const auto& grant : grants) {
    canonical << "|grant=" << grant.provider << ':' << grant.role << ':'
              << grant.grantName << ':' << grant.grantDigest << ':' << grant.recipient;
  }
  sealed.planDigest = nativePlanningDigest(canonical.str());
  sealed.validate();
  return sealed;
}

NativeSelectionProjectionV3 NativePlanSealer::project(
  const NativeSealedPlan& sealed, const std::string& provider)
{
  sealed.validate();
  const auto assignment = std::find_if(sealed.core.assignment.providerByRole.begin(),
                                       sealed.core.assignment.providerByRole.end(),
                                       [&provider] (const auto& item) {
                                         return item.second == provider;
                                       });
  if (assignment == sealed.core.assignment.providerByRole.end()) {
    throw std::invalid_argument("provider is not present in sealed plan");
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
  projection.offerDigest = provider;
  projection.securityPolicySnapshotDigest = sealed.security.policyDigest;
  projection.selectedRole.role = assignment->first;
  projection.selectedRole.selectedRole = assignment->first;
  projection.selectedRole.backend = "onnxruntime-cpu";
  projection.selectedRole.artifactDigest = sealed.core.artifactDigestByRole.at(assignment->first);
  projection.selectedRole.graphDigest = sealed.core.graphDigest;
  projection.selectedRole.adapterId = sealed.core.executionPlan.modelFamily;
  projection.selectedRole.adapterVersion = "1";
  projection.selectedRole.roleKind = sealed.core.executionPlan.modelFamily;
  projection.executionRole = {assignment->first, assignment->first, 0, 0, 0,
                              "onnxruntime-cpu", sealed.core.executionPlan.modelFamily, "1"};
  projection.hasGrantBinding = grant != sealed.grants.end();
  if (grant != sealed.grants.end()) {
    projection.grantName = grant->grantName;
    projection.grantDigest = grant->grantDigest;
  }
  projection.plan = sealed.core.executionPlan;
  return projection;
}

std::vector<std::uint8_t> NativePlanSealer::encode(
  const NativeSelectionProjectionV3& projection)
{
  if (projection.provider.empty() || projection.requestId.empty() ||
      !isDigest(projection.planDigest) || projection.attempt == 0 ||
      projection.selectedRole.role.empty()) {
    throw std::invalid_argument("native selection projection is incomplete");
  }
  std::ostringstream json;
  json << "{\"provider\":" << quote(projection.provider)
       << ",\"request_id\":" << quote(projection.requestId)
       << ",\"attempt\":" << projection.attempt
       << ",\"plan_digest\":" << quote(projection.planDigest)
       << ",\"plan_core_digest\":" << quote(projection.planCoreDigest)
       << ",\"ack_closed_digest\":" << quote(projection.ackClosedDigest)
       << ",\"selected_role\":{\"role\":"
       << quote(projection.selectedRole.role) << "}}";
  const auto value = json.str();
  return {value.begin(), value.end()};
}

} // namespace ndnsf::di
