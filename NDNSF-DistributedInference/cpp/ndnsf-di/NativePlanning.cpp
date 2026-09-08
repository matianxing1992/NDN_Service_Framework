#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

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

bool canPlaceRole(const NativeProviderPlanningView& offer, const std::string& role,
                  const NativeRoleResourceRequirement& requirement)
{
  if (!contains(offer.acceptedRoles, role) ||
      std::none_of(requirement.backends.begin(), requirement.backends.end(),
                   [&offer] (const auto& backend) { return contains(offer.backends, backend); })) {
    return false;
  }
  // Promote before addition: a wrapped byte sum must never admit an offer.
  const long double bytes = static_cast<long double>(requirement.weightBytes) +
    requirement.workspaceBytes + requirement.activationBytes + requirement.transientBytes;
  return bytes * requirement.safetyMargin <= static_cast<long double>(offer.freeBytes);
}

} // namespace

void NativeStrategyIdentity::validate() const
{
  if (name.empty() || version.empty()) throw std::invalid_argument("strategy identity is incomplete");
  requireDigest(configurationDigest, "strategy configurationDigest");
}

void NativeAdapterDescriptor::validate() const
{
  if (name.empty() || version.empty() || abi.empty() || modelFormats.empty() ||
      tasks.empty() || backends.empty() || precisions.empty())
    throw std::invalid_argument("model adapter descriptor is incomplete");
  for (const auto* value : {&stateDigest, &inputSchemaDigest, &optionsSchemaDigest,
                            &resultSchemaDigest, &graphSchemaDigest, &splitSchemaDigest, &stateSchemaDigest})
    requireDigest(*value, "adapter schema/state digest");
}

std::string NativeAdapterDescriptor::canonicalJson() const
{
  validate();
  return nativeCanonicalJson(NativeJson{
    {"name", name}, {"version", version}, {"state_digest", stateDigest}, {"abi", abi},
    {"model_formats", modelFormats}, {"tasks", tasks}, {"backends", backends}, {"precisions", precisions},
    {"input_schema_digest", inputSchemaDigest}, {"options_schema_digest", optionsSchemaDigest},
    {"result_schema_digest", resultSchemaDigest}, {"graph_schema_digest", graphSchemaDigest},
    {"split_schema_digest", splitSchemaDigest}, {"state_schema_digest", stateSchemaDigest},
    {"graph_inspectable", graphInspectable}, {"splittable", splittable},
    {"deterministic_analysis", deterministicAnalysis}});
}

std::string NativeAdapterDescriptor::descriptorDigest() const
{
  return nativePlanningDigest(canonicalJson());
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
  adapter.validate();
  if (adapterId != adapter.name || adapterVersion != adapter.version ||
      std::find(adapter.modelFormats.begin(), adapter.modelFormats.end(), modelFormat) == adapter.modelFormats.end() ||
      std::find(adapter.precisions.begin(), adapter.precisions.end(), precision) == adapter.precisions.end())
    throw std::invalid_argument("model descriptor is incompatible with its adapter");
}

std::string NativeModelDescriptor::canonicalJson() const
{
  validate();
  return nativeCanonicalJson(NativeJson{
    {"model_name", modelName}, {"content_digest", contentDigest}, {"semantics_digest", semanticsDigest},
    {"graph_digest", graphDigest}, {"model_format", modelFormat}, {"precision", precision},
    {"adapter", nativeParseJson(adapter.canonicalJson())}, {"source_revision", sourceRevision}});
}

std::string NativeModelDescriptor::modelDigest() const
{
  return nativePlanningDigest(canonicalJson());
}

void NativeTensorContract::validate() const
{
  if (name.empty() || dtype.empty()) throw std::invalid_argument("incomplete tensor contract");
  // Preserve the graph owner's integer/symbolic representation. Assembly and
  // runtime shape contracts apply their constraints at their own boundaries.
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
  for (const auto& tensor : modelInputs) tensor.validate();
  for (const auto& tensor : modelOutputs) tensor.validate();
  std::map<std::string, std::size_t> position;
  for (std::size_t i = 0; i < nodes.size(); ++i) position.emplace(nodes[i].id, i);
  std::set<std::string> edgeIds;
  for (const auto& edge : edges) {
    if (edge.id.empty() || !edgeIds.insert(edge.id).second || edge.tensor.name != edge.id ||
        !position.count(edge.producer) || edge.consumers.empty())
      throw std::invalid_argument("invalid graph tensor edge identity or producer");
    edge.tensor.validate();
    std::set<std::string> consumers;
    for (const auto& consumer : edge.consumers) {
      if (!position.count(consumer) || !consumers.insert(consumer).second ||
          position.at(consumer) <= position.at(edge.producer))
        throw std::invalid_argument("invalid graph tensor consumer or topological direction");
    }
  }
  for (const auto& cut : cuts) {
    if (!edgeIds.count(cut)) throw std::invalid_argument("graph cut does not refer to a tensor edge");
  }
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
  std::set<std::string> providers;
  for (const auto& offer : offers) {
    offer.validate();
    if (!providers.insert(offer.provider).second) {
      throw std::invalid_argument("planning snapshot contains duplicate Provider offers");
    }
  }
  if (deadline <= std::chrono::steady_clock::now()) {
    throw std::invalid_argument("planning snapshot deadline has expired");
  }
}

void NativeSplitCandidate::validate(const NativeGraphSnapshot& graph) const
{
  splitter.validate();
  model.validate();
  graph.validate(model);
  if (graphDigest != graph.graphDigest || executionPlan.roles.empty() ||
      !isDigest(candidateDigest)) {
    throw std::invalid_argument("split candidate identity is incomplete");
  }
  std::set<std::string> roles(executionPlan.roles.begin(), executionPlan.roles.end());
  if (roles.size() != executionPlan.roles.size() || roles.empty() ||
      fragmentsByRole.size() != roles.size() || artifactsByRole.size() != roles.size() ||
      requirementsByRole.size() != roles.size()) {
    throw std::invalid_argument("split candidate role cover is incomplete");
  }
  for (const auto& role : roles) {
    if (fragmentsByRole.count(role) == 0 || artifactsByRole.count(role) == 0 ||
        requirementsByRole.count(role) == 0) {
      throw std::invalid_argument("split candidate role cover contains foreign roles");
    }
    if (role.empty() || !isDigest(fragmentsByRole.at(role)) || artifactsByRole.at(role).empty() ||
        requirementsByRole.at(role).backends.empty()) {
      throw std::invalid_argument("split candidate role has no artifact or backend");
    }
    const auto margin = requirementsByRole.at(role).safetyMargin;
    if (!std::isfinite(margin) || margin < 1.0 ||
        std::any_of(artifactsByRole.at(role).begin(), artifactsByRole.at(role).end(),
                    [] (const auto& digest) { return !isDigest(digest); })) {
      throw std::invalid_argument("split candidate artifact or resource requirement is invalid");
    }
  }
  if (inputIngressRole.empty() != resultEgressRole.empty() ||
      (!inputIngressRole.empty() && (!roles.count(inputIngressRole) || !roles.count(resultEgressRole))))
    throw std::invalid_argument("split candidate ingress/egress role is undeclared");
  std::set<std::string> ownedRoles;
  if (nodeRoles.size() != graph.nodes.size())
    throw std::invalid_argument("split candidate does not partition every graph node");
  for (const auto& node : graph.nodes) {
    const auto owner = nodeRoles.find(node.id);
    if (owner == nodeRoles.end() || !roles.count(owner->second))
      throw std::invalid_argument("split candidate node owner is absent or undeclared");
    ownedRoles.insert(owner->second);
  }
  if (ownedRoles != roles)
    throw std::invalid_argument("split candidate has a role without graph nodes");
  const auto stateValid = [&](const auto& contracts) {
    if (contracts.size() != roles.size())
      throw std::invalid_argument("split candidate state I/O role cover is incomplete");
    for (const auto& role : roles) {
      const auto tensors = contracts.find(role);
      if (tensors == contracts.end() || tensors->second.empty())
        throw std::invalid_argument("split candidate state I/O is empty or has foreign roles");
      std::set<std::string> names;
      for (const auto& tensor : tensors->second) {
        tensor.validate();
        if (!names.insert(tensor.name).second)
          throw std::invalid_argument("split candidate state tensor names are duplicate");
      }
    }
  };
  if (!roleStateInputsByRole.empty() || !roleStateOutputsByRole.empty()) {
    stateValid(roleStateInputsByRole);
    stateValid(roleStateOutputsByRole);
  }
  const std::set<std::string> cuts(crossPartitionTensors.begin(), crossPartitionTensors.end());
  const std::set<std::string> legal(graph.legalCutEdges.begin(), graph.legalCutEdges.end());
  if (cuts.size() != crossPartitionTensors.size() ||
      !std::includes(legal.begin(), legal.end(), cuts.begin(), cuts.end()))
    throw std::invalid_argument("split candidate has duplicate or illegal cut tensors");
  std::set<std::string> dependencyTensors;
  std::map<std::string, std::set<std::string>> outgoing;
  std::map<std::string, std::size_t> incoming;
  for (const auto& role : roles) incoming[role] = 0;
  for (const auto& dependency : executionPlan.dependencies) {
    if (dependency.producers.empty() || dependency.consumers.empty() || dependency.tensors.empty())
      throw std::invalid_argument("split candidate dependency is incomplete");
    for (const auto& role : dependency.producers)
      if (!roles.count(role)) throw std::invalid_argument("split candidate dependency producer is undeclared");
    for (const auto& role : dependency.consumers)
      if (!roles.count(role)) throw std::invalid_argument("split candidate dependency consumer is undeclared");
    dependencyTensors.insert(dependency.tensors.begin(), dependency.tensors.end());
    for (const auto& producer : dependency.producers)
      for (const auto& consumer : dependency.consumers)
        if (outgoing[producer].insert(consumer).second) ++incoming.at(consumer);
  }
  if (dependencyTensors != cuts)
    throw std::invalid_argument("split candidate dependency tensors do not match its cuts");
  std::vector<std::string> ready;
  for (const auto& role : incoming) if (!role.second) ready.push_back(role.first);
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto role = ready.back(); ready.pop_back(); ++visited;
    for (const auto& consumer : outgoing[role])
      if (--incoming.at(consumer) == 0) ready.push_back(consumer);
  }
  if (visited != roles.size()) throw std::invalid_argument("split candidate role dependencies are cyclic");
  if (!tensorDegreesByRole.empty() || !rankArtifactDigestsByRole.empty()) {
    if (tensorDegreesByRole.size() != roles.size() || rankArtifactDigestsByRole.size() != roles.size())
      throw std::invalid_argument("split candidate rank metadata cover is incomplete");
    for (const auto& role : roles) {
      if (!tensorDegreesByRole.count(role) || !rankArtifactDigestsByRole.count(role))
        throw std::invalid_argument("split candidate rank metadata has foreign roles");
      const auto degree = tensorDegreesByRole.at(role);
      const auto& artifacts = rankArtifactDigestsByRole.at(role);
      const std::set<std::string> unique(artifacts.begin(), artifacts.end());
      if (!degree || artifacts.size() != degree || unique.size() != artifacts.size())
        throw std::invalid_argument("split candidate rank artifacts are incomplete or duplicate");
      for (const auto& artifact : artifacts)
        if (std::find(artifactsByRole.at(role).begin(), artifactsByRole.at(role).end(), artifact) == artifactsByRole.at(role).end())
          throw std::invalid_argument("split candidate rank artifact is absent from its role");
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
  std::set<std::string> usedProviders;
  for (const auto& role : executionPlan.roles) {
    const auto it = assignment.providerByRole.find(role);
    if (it == assignment.providerByRole.end() || it->second.empty()) {
      throw std::invalid_argument("placement proposal has an unassigned role");
    }
    const auto offer = std::find_if(snapshot.offers.begin(), snapshot.offers.end(),
      [&it] (const auto& view) { return view.provider == it->second; });
    if (offer == snapshot.offers.end() || !usedProviders.insert(it->second).second ||
        !canPlaceRole(*offer, role, candidate.requirementsByRole.at(role))) {
      throw std::invalid_argument("placement proposal requires distinct feasible Providers");
    }
  }
}

std::string nativePlanningDigest(const std::string& canonical)
{
  return nativePlanningDigest(reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size());
}

std::string nativePlanningDigest(const std::uint8_t* data, std::size_t size)
{
  if (!data && size) throw std::invalid_argument("native digest source is null");
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(data, size, digest);
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

  NativePlacementProposal result;
  result.requestId = snapshot.requestId;
  result.attempt = snapshot.attempt;
  result.modelDigest = snapshot.model.contentDigest;
  result.graphDigest = snapshot.graph.graphDigest;
  result.candidateDigest = candidate.candidateDigest;
  result.strategy = m_identity;
  result.executionPlan = candidate.executionPlan;
  std::set<std::string> usedProviders;
  auto roles = candidate.executionPlan.roles;
  std::sort(roles.begin(), roles.end());
  for (const auto& role : roles) {
    std::vector<const NativeProviderPlanningView*> eligible;
    for (const auto& offer : snapshot.offers) {
      if (usedProviders.count(offer.provider) == 0 &&
          canPlaceRole(offer, role, candidate.requirementsByRole.at(role))) {
        eligible.push_back(&offer);
      }
    }
    if (eligible.empty()) {
      throw std::runtime_error("no distinct feasible Provider for native role " + role);
    }
    // Digest hints rank canonical availability only, never device-ready reuse.
    const auto hasArtifacts = [&candidate, &role] (const auto* offer) {
      const auto& artifacts = candidate.artifactsByRole.at(role);
      return std::all_of(artifacts.begin(), artifacts.end(), [&offer] (const auto& digest) {
        return contains(offer->residencyDigests, digest);
      });
    };
    std::sort(eligible.begin(), eligible.end(), [&hasArtifacts] (auto left, auto right) {
      const bool leftHit = hasArtifacts(left), rightHit = hasArtifacts(right);
      if (leftHit != rightHit) return leftHit;
      if (left->freeBytes != right->freeBytes) return left->freeBytes > right->freeBytes;
      return left->provider < right->provider;
    });
    result.assignment.providerByRole.emplace(role, eligible.front()->provider);
    usedProviders.insert(eligible.front()->provider);
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
  // Latching an empty registry is legal: a consumer may hold no concrete
  // adapter yet (the spec182 installed-library boundary at T002-A). Callers
  // that need at least one adapter enforce it at the use site via find().
  m_frozen = true;
}

std::shared_ptr<const NativeModelAdapter>
NativeAdapterRegistry::find(const std::string& adapterId) const
{
  const auto it = m_adapters.find(adapterId);
  return it == m_adapters.end() ? nullptr : it->second;
}

} // namespace ndnsf::di
