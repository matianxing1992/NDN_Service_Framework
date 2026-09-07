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
  static const char hex[] = "0123456789abcdef";
  std::ostringstream out;
  out << '"';
  for (const auto c : value) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        // JSON string bodies cannot carry raw control characters; emit the
        // canonical \u00XX form. Bytes at or above 0x20 pass through raw,
        // keeping UTF-8 payloads intact (same rule as the canonical encoder).
        if (byte < 0x20) {
          out << "\\u00" << hex[byte >> 4] << hex[byte & 0x0F];
        } else {
          out << c;
        }
    }
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
  out << "|offers=";
  for (const auto& item : core.offerDigestByProvider) {
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
  artifacts.validate();
  if (artifacts.requestId != requestId || artifacts.attempt != attempt ||
      artifacts.modelDigest != modelDigest || artifacts.graphDigest != graphDigest ||
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
  std::set<std::string> providers;
  if (roles.size() != executionPlan.roles.size() ||
      assignment.providerByRole.size() != roles.size() ||
      artifactDigestByRole.size() != roles.size() ||
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
    if (!providers.insert(provider->second).second) {
      throw std::invalid_argument("native plan requires one role per Provider");
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
  if (core.offerDigestByProvider.at(provider.provider) != provider.offerDigest) {
    throw std::invalid_argument("grant view offer differs from the sealed ACK offer");
  }
  if (security.requireProtectedArtifacts == (core.protectionEpoch == "plaintext-v1")) {
    throw std::invalid_argument("grant view protection epoch disagrees with policy");
  }
  return {provider.provider, role->first, core.coreDigest, security.policyDigest,
          core.modelDigest, core.graphDigest, core.artifactDigestByRole.at(role->first),
          core.requesterIdentity, core.requestId, core.attempt, core.artifacts.manifestDigest,
          core.protectionEpoch, core.expiresAtMs};
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
  projection.offerDigest = sealed.core.offerDigestByProvider.at(provider);
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
