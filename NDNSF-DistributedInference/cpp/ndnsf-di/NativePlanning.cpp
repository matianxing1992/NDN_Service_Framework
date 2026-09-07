#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <algorithm>
#include <cmath>
#include <openssl/sha.h>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::di {
namespace {

void requireDigest(const std::string& value, const char* field)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0 ||
      !std::all_of(value.begin() + 7, value.end(), [] (char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) {
    throw std::invalid_argument(std::string(field) + " must be a lowercase sha256 digest");
  }
}

bool isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool contains(const std::vector<std::string>& values, const std::string& value)
{
  return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

void NativeStrategyIdentity::validate() const
{
  if (name.empty() || version.empty()) throw std::invalid_argument("strategy identity is incomplete");
  requireDigest(configurationDigest, "strategy configurationDigest");
}

void NativeModelDescriptor::validate() const
{
  if (modelName.empty() || modelFormat.empty() || precision.empty() ||
      adapterId.empty() || adapterVersion.empty()) {
    throw std::invalid_argument("model descriptor is incomplete");
  }
  requireDigest(contentDigest, "model contentDigest");
  requireDigest(semanticsDigest, "model semanticsDigest");
  requireDigest(graphDigest, "model graphDigest");
}

void NativeGraphSnapshot::validate(const NativeModelDescriptor& model) const
{
  requireDigest(graphDigest, "graph graphDigest");
  if (graphDigest != model.graphDigest || nodes.empty() ||
      topologicalOrder.size() != nodes.size()) {
    throw std::invalid_argument("graph snapshot identity or topological order is invalid");
  }
  std::set<std::string> nodeIds;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].id.empty() || !nodeIds.insert(nodes[i].id).second ||
        nodes[i].ordinal != i || topologicalOrder[i] != nodes[i].id) {
      throw std::invalid_argument("graph node order or identity is invalid");
    }
  }
  std::set<std::string> cuts(legalCutEdges.begin(), legalCutEdges.end());
  if (cuts.size() != legalCutEdges.size()) throw std::invalid_argument("duplicate graph cut edge");
}

void NativeCandidateBudget::validate() const
{
  if (maxCandidates == 0 || maxCandidates > 1024 || maxPolicyMs == 0 ||
      maxPolicyMs > 60'000 || maxReentries > 16) {
    throw std::invalid_argument("native candidate budget is out of range");
  }
}

void NativeProviderPlanningView::validate() const
{
  if (provider.empty() || !preparationAccepted || !executionAllowed ||
      !isDigest(offerDigest) || resourceSequence == 0 || backends.empty() ||
      std::any_of(residencyDigests.begin(), residencyDigests.end(),
                  [] (const auto& value) { return !isDigest(value); })) {
    throw std::invalid_argument("provider planning view is not executable");
  }
}

void NativePlanningSnapshot::validate() const
{
  model.validate();
  graph.validate(model);
  if (requestId.empty() || attempt == 0 || ackClosedDigest.empty() || offers.empty()) {
    throw std::invalid_argument("planning snapshot is incomplete");
  }
  for (const auto& offer : offers) offer.validate();
  if (deadline <= std::chrono::steady_clock::now()) {
    throw std::invalid_argument("planning snapshot deadline has expired");
  }
}

void NativeSplitCandidate::validate(const NativeGraphSnapshot& graph) const
{
  splitter.validate();
  model.validate();
  if (graphDigest != graph.graphDigest || executionPlan.roles.empty() ||
      candidateDigest.empty()) {
    throw std::invalid_argument("split candidate identity is incomplete");
  }
  std::set<std::string> roles(executionPlan.roles.begin(), executionPlan.roles.end());
  if (roles.size() != executionPlan.roles.size() || roles.empty() ||
      fragmentsByRole.size() != roles.size() || requirementsByRole.size() != roles.size()) {
    throw std::invalid_argument("split candidate role cover is incomplete");
  }
  for (const auto& role : roles) {
    if (fragmentsByRole.at(role).empty() || artifactsByRole.at(role).empty() ||
        requirementsByRole.at(role).backends.empty()) {
      throw std::invalid_argument("split candidate role has no artifact or backend");
    }
  }
}

void NativePlacementProposal::validate(const NativePlanningSnapshot& snapshot,
                                       const NativeSplitCandidate& candidate) const
{
  if (requestId != snapshot.requestId || attempt != snapshot.attempt ||
      modelDigest != snapshot.model.contentDigest || graphDigest != snapshot.graph.graphDigest ||
      candidateDigest != candidate.candidateDigest || executionPlan.roles != candidate.executionPlan.roles) {
    throw std::invalid_argument("placement proposal is not bound to the planning snapshot");
  }
  strategy.validate();
  candidate.validate(snapshot.graph);
  if (assignment.providerByRole.size() != executionPlan.roles.size()) {
    throw std::invalid_argument("placement proposal does not cover every role");
  }
  for (const auto& role : executionPlan.roles) {
    const auto it = assignment.providerByRole.find(role);
    if (it == assignment.providerByRole.end() || it->second.empty()) {
      throw std::invalid_argument("placement proposal has an unassigned role");
    }
  }
}

std::string nativePlanningDigest(const std::string& canonical)
{
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size(), digest);
  std::ostringstream out;
  out << "sha256:" << std::hex;
  for (const auto value : digest) {
    out.width(2); out.fill('0'); out << static_cast<unsigned>(value);
  }
  return out.str();
}

NativePreSplitFirstPlacement::NativePreSplitFirstPlacement(NativeStrategyIdentity identity)
  : m_identity(std::move(identity))
{
  m_identity.validate();
}

NativeStrategyIdentity NativePreSplitFirstPlacement::identity() const
{
  return m_identity;
}

NativePlacementProposal
NativePreSplitFirstPlacement::propose(const NativePlanningSnapshot& snapshot,
                                      const NativeSplitCandidate& candidate) const
{
  snapshot.validate();
  candidate.validate(snapshot.graph);
  if (candidate.executionPlan.roles.empty()) throw std::invalid_argument("candidate has no roles");

  std::vector<const NativeProviderPlanningView*> eligible;
  for (const auto& offer : snapshot.offers) {
    bool hasBackend = true;
    for (const auto& req : candidate.requirementsByRole) {
      if (!contains(offer.acceptedRoles, req.first) ||
          std::none_of(req.second.backends.begin(), req.second.backends.end(),
                       [&offer] (const auto& backend) { return contains(offer.backends, backend); })) {
        hasBackend = false;
        break;
      }
      const auto required = static_cast<double>(req.second.weightBytes + req.second.workspaceBytes +
                                                req.second.activationBytes + req.second.transientBytes) *
                            req.second.safetyMargin;
      if (required > static_cast<double>(offer.freeBytes)) hasBackend = false;
    }
    if (hasBackend) eligible.push_back(&offer);
  }
  if (eligible.empty()) throw std::runtime_error("no eligible provider for native split candidate");
  std::stable_sort(eligible.begin(), eligible.end(), [] (auto left, auto right) {
    if (left->residencyDigests.size() != right->residencyDigests.size())
      return left->residencyDigests.size() > right->residencyDigests.size();
    if (left->freeBytes != right->freeBytes) return left->freeBytes > right->freeBytes;
    return left->provider < right->provider;
  });

  NativePlacementProposal result;
  result.requestId = snapshot.requestId;
  result.attempt = snapshot.attempt;
  result.modelDigest = snapshot.model.contentDigest;
  result.graphDigest = snapshot.graph.graphDigest;
  result.candidateDigest = candidate.candidateDigest;
  result.strategy = m_identity;
  result.executionPlan = candidate.executionPlan;
  for (const auto& role : candidate.executionPlan.roles) {
    // A single provider assignment is deliberate: the existing pre-split
    // strategy does not invent rank/device fan-out or bypass lease authority.
    result.assignment.providerByRole.emplace(role, eligible.front()->provider);
  }
  result.validate(snapshot, candidate);
  return result;
}

void NativeAdapterRegistry::registerAdapter(std::shared_ptr<const NativeModelAdapter> adapter)
{
  if (m_frozen) throw std::logic_error("native adapter registry is frozen");
  if (!adapter || adapter->adapterId().empty()) throw std::invalid_argument("invalid native adapter");
  auto [it, inserted] = m_adapters.emplace(adapter->adapterId(), std::move(adapter));
  if (!inserted) throw std::invalid_argument("native adapter is already registered");
}

void NativeAdapterRegistry::freeze()
{
  if (m_adapters.empty()) throw std::invalid_argument("native adapter registry is empty");
  m_frozen = true;
}

std::shared_ptr<const NativeModelAdapter>
NativeAdapterRegistry::find(const std::string& adapterId) const
{
  const auto it = m_adapters.find(adapterId);
  return it == m_adapters.end() ? nullptr : it->second;
}

} // namespace ndnsf::di
