#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <openssl/sha.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>

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
  const auto peak = requirement.estimatedPeakGpuMemoryBytes();
  return peak && *peak <= offer.freeBytes;
}

NativeJson redistributionJson(const RedistributionSpec& edge)
{
  return NativeJson{{"producer_ranks", edge.producerRanks}, {"consumer_ranks", edge.consumerRanks},
    {"tensor", edge.tensor}, {"operation", edge.operation}, {"epoch", edge.epoch},
    {"integrity_digest", edge.integrityDigest}, {"source_layout_digest", edge.sourceLayoutDigest},
    {"target_layout_digest", edge.targetLayoutDigest}, {"temporary_memory_bytes", edge.temporaryMemoryBytes},
    {"complete_output", edge.completeOutput}, {"axis", edge.axis}};
}

NativeJson tensorJson(const NativeTensorContract& tensor)
{
  tensor.validate();
  auto shape = NativeJson::array();
  for (const auto& dimension : tensor.shape)
    std::visit([&](const auto& value) { shape.push_back(value); }, dimension);
  return NativeJson{{"name", tensor.name}, {"dtype", tensor.dtype}, {"shape", shape},
    {"estimated_bytes", tensor.estimatedBytes ? NativeJson(*tensor.estimatedBytes) : NativeJson(nullptr)}};
}

} // namespace

void ExtensionControl::requireActive() const
{
  if (cancelled && cancelled())
    throw std::runtime_error("cooperative extension cancelled");
  if (deadline != std::chrono::steady_clock::time_point{} &&
      std::chrono::steady_clock::now() >= deadline)
    throw std::runtime_error("cooperative extension deadline exceeded");
}

void NativeStrategyIdentity::validate() const
{
  if (name.empty() || version.empty()) throw std::invalid_argument("strategy identity is incomplete");
  requireDigest(configurationDigest, "strategy configurationDigest");
}

void NativeRoleResourceRequirement::validate() const
{
  if (backends.empty() || !std::isfinite(safetyMargin) || safetyMargin < 1.0)
    throw std::invalid_argument("invalid role resource requirement");
}

std::string NativeRoleResourceRequirement::canonicalJson() const
{
  validate();
  const auto value = [](const auto& bytes) { return bytes ? NativeJson(*bytes) : NativeJson(nullptr); };
  return nativeCanonicalJson(NativeJson{
    {"backends", backends}, {"weight_bytes", value(weightBytes)},
    {"workspace_bytes", value(workspaceBytes)}, {"kv_bytes", value(kvBytes)},
    {"activation_bytes", value(activationBytes)}, {"transient_bytes", value(transientBytes)},
    {"safety_margin", safetyMargin}});
}

std::optional<std::uint64_t> NativeRoleResourceRequirement::estimatedPeakGpuMemoryBytes() const
{
  validate();
  const auto values = {weightBytes, workspaceBytes, kvBytes, activationBytes, transientBytes};
  if (std::any_of(values.begin(), values.end(), [](const auto& value) { return !value; }))
    return std::nullopt;
  std::uint64_t sum = 0;
  for (const auto& value : values) {
    if (*value > std::numeric_limits<std::uint64_t>::max() - sum)
      throw std::overflow_error("role memory budget exceeds uint64");
    sum += *value;
  }
  // Python converts the exact integer sum to binary64 before multiplication,
  // then truncates to an integer. Do not substitute long-double or ceil here.
  const double peak = static_cast<double>(sum) * safetyMargin;
  if (!std::isfinite(peak) || peak >= std::ldexp(1.0, 64))
    throw std::overflow_error("role memory peak exceeds uint64");
  return static_cast<std::uint64_t>(peak);
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

NativeModelDescriptor NativeModelDescriptor::fromCanonicalJson(const std::string& json)
{
  if (json.size() > 1024 * 1024) throw std::invalid_argument("model descriptor exceeds limit");
  const auto root = nativeParseJson(json);
  const auto& a = root.at("adapter");
  NativeModelDescriptor model;
  model.modelName = root.at("model_name").get<std::string>();
  model.contentDigest = root.at("content_digest").get<std::string>();
  model.semanticsDigest = root.at("semantics_digest").get<std::string>();
  model.graphDigest = root.at("graph_digest").get<std::string>();
  model.modelFormat = root.at("model_format").get<std::string>();
  model.precision = root.at("precision").get<std::string>();
  model.sourceRevision = root.at("source_revision").get<std::string>();
  auto& adapter = model.adapter;
  adapter.name = a.at("name").get<std::string>();
  adapter.version = a.at("version").get<std::string>();
  adapter.stateDigest = a.at("state_digest").get<std::string>();
  adapter.abi = a.at("abi").get<std::string>();
  adapter.modelFormats = a.at("model_formats").get<std::vector<std::string>>();
  adapter.tasks = a.at("tasks").get<std::vector<std::string>>();
  adapter.backends = a.at("backends").get<std::vector<std::string>>();
  adapter.precisions = a.at("precisions").get<std::vector<std::string>>();
  adapter.inputSchemaDigest = a.at("input_schema_digest").get<std::string>();
  adapter.optionsSchemaDigest = a.at("options_schema_digest").get<std::string>();
  adapter.resultSchemaDigest = a.at("result_schema_digest").get<std::string>();
  adapter.graphSchemaDigest = a.at("graph_schema_digest").get<std::string>();
  adapter.splitSchemaDigest = a.at("split_schema_digest").get<std::string>();
  adapter.stateSchemaDigest = a.at("state_schema_digest").get<std::string>();
  adapter.graphInspectable = a.at("graph_inspectable").get<bool>();
  adapter.splittable = a.at("splittable").get<bool>();
  adapter.deterministicAnalysis = a.at("deterministic_analysis").get<bool>();
  model.adapterId = adapter.name; model.adapterVersion = adapter.version;
  model.validate();
  if (nativeCanonicalJson(root) != model.canonicalJson())
    throw std::invalid_argument("model descriptor contains unknown or lossy fields");
  return model;
}

std::string NativeModelDescriptor::intentDigest() const
{
  validate();
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"model_name", modelName}, {"content_digest", contentDigest},
    {"semantics_digest", semanticsDigest},
    {"source_revision", sourceRevision.empty() ? NativeJson(nullptr) : NativeJson(sourceRevision)}}));
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
    if (nodes[i].id.empty() || nodes[i].opType.empty() || !nodeIds.insert(nodes[i].id).second ||
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

void NativeHybridPlan::validate() const
{
  if (!stages || tensorDegrees.size() != stages)
    throw std::invalid_argument("hybrid plan stage cover is incomplete");
  std::uint64_t count = 0;
  for (const auto degree : tensorDegrees) {
    if (!degree || degree > std::numeric_limits<std::uint64_t>::max() - count)
      throw std::invalid_argument("hybrid plan rank count is invalid");
    count += degree;
  }
  if (count != rankLabels.size()) throw std::invalid_argument("hybrid plan rank cover is incomplete");
  std::vector<std::size_t> stageOfRank;
  std::vector<std::uint64_t> offsets;
  for (std::size_t stage = 0; stage < tensorDegrees.size(); ++stage) {
    offsets.push_back(stageOfRank.size());
    for (std::uint64_t rank = 0; rank < tensorDegrees[stage]; ++rank) {
      if (rankLabels[stageOfRank.size()] != "S" + std::to_string(stage) + "R" + std::to_string(rank))
        throw std::invalid_argument("hybrid plan rank labels are not canonical");
      stageOfRank.push_back(stage);
    }
  }
  std::set<std::string> identities;
  std::set<std::size_t> boundaries;
  for (const auto& edge : redistributions) {
    const auto p = edge.producerRanks.size(), c = edge.consumerRanks.size();
    const bool operationMatches =
      (p == 1 && c > 1 && edge.operation == "SCATTER") ||
      (p > 1 && c == 1 && edge.operation == "GATHER") ||
      (p > 1 && c > 1 && edge.operation == "RESHARD") ||
      (p == 1 && c == 1 && edge.operation == "RESHARD" && edge.sourceLayoutDigest != edge.targetLayoutDigest);
    if (!operationMatches || edge.tensor.empty() || edge.epoch.empty() || !edge.completeOutput ||
        edge.axis < -16 || edge.axis >= 16)
      throw std::invalid_argument("invalid hybrid redistribution edge");
    requireDigest(edge.integrityDigest, "redistribution integrity");
    requireDigest(edge.sourceLayoutDigest, "redistribution source layout");
    requireDigest(edge.targetLayoutDigest, "redistribution target layout");
    const auto stageFor = [&](const auto& ranks) {
      std::set<std::uint64_t> unique(ranks.begin(), ranks.end());
      if (unique.empty() || unique.size() != ranks.size() || *unique.rbegin() >= count)
        throw std::invalid_argument("invalid redistribution rank set");
      const auto stage = stageOfRank[*unique.begin()];
      if (unique.size() != tensorDegrees[stage] || *unique.begin() != offsets[stage] ||
          *unique.rbegin() != offsets[stage] + tensorDegrees[stage] - 1)
        throw std::invalid_argument("redistribution stage rank cover is incomplete");
      return stage;
    };
    const auto producerStage = stageFor(edge.producerRanks);
    if (stageFor(edge.consumerRanks) != producerStage + 1)
      throw std::invalid_argument("redistribution must follow an adjacent stage boundary");
    auto identity = redistributionJson(edge);
    for (const auto* field : {"source_layout_digest", "target_layout_digest", "temporary_memory_bytes",
                              "complete_output", "axis"}) identity.erase(field);
    if (!identities.insert(nativeCanonicalJson(identity)).second)
      throw std::invalid_argument("duplicate redistribution edge");
    boundaries.insert(producerStage);
  }
  for (std::size_t stage = 0; stage + 1 < tensorDegrees.size(); ++stage)
    if (tensorDegrees[stage] != tensorDegrees[stage + 1] && !boundaries.count(stage))
      throw std::invalid_argument("degree-changing boundary omits redistribution");
}

std::string NativeHybridPlan::canonicalJson() const
{
  validate();
  auto edges = NativeJson::array();
  for (const auto& edge : redistributions) edges.push_back(redistributionJson(edge));
  return nativeCanonicalJson(NativeJson{{"stages", stages}, {"tensor_degrees", tensorDegrees},
    {"rank_labels", rankLabels}, {"redistributions", edges}});
}

std::string NativeSplitCandidate::canonicalJson() const
{
  splitter.validate();
  auto dependencies = NativeJson::array();
  // RoleExecutionPlan is model planning data. Runtime transport attributes are
  // sealed later and are intentionally outside the maintained candidate schema.
  for (const auto& edge : executionPlan.dependencies)
    for (const auto& producer : edge.producers)
      for (const auto& consumer : edge.consumers)
        dependencies.push_back(NativeJson{{"producer", producer}, {"consumer", consumer},
                                          {"tensor_edges", edge.tensors}});
  auto resources = NativeJson::object();
  for (const auto& role : requirementsByRole) resources[role.first] = nativeParseJson(role.second.canonicalJson());
  auto costs = NativeJson::object();
  for (const auto& item : estimatedCosts)
    std::visit([&](const auto& value) {
      if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::monostate>) costs[item.first] = nullptr;
      else costs[item.first] = value;
    }, item.second);
  const auto states = [](const auto& roles) {
    auto result = NativeJson::object();
    for (const auto& role : roles) {
      auto tensors = NativeJson::array();
      for (const auto& tensor : role.second) tensors.push_back(tensorJson(tensor));
      result[role.first] = std::move(tensors);
    }
    return result;
  };
  const auto postprocessing = nativeParseJson(postprocessingJson);
  if (!postprocessing.is_object()) throw std::invalid_argument("candidate postprocessing must be an object");
  return nativeCanonicalJson(NativeJson{
    {"source", source}, {"splitter", {{"name", splitter.name}, {"version", splitter.version},
      {"state_digest", splitter.configurationDigest}, {"deterministic", splitter.deterministic}}},
    {"model", nativeParseJson(model.canonicalJson())}, {"graph_digest", graphDigest},
    {"execution_plan", {{"roles", executionPlan.roles}, {"dependencies", dependencies}, {"node_roles", nodeRoles}}},
    {"fragments_by_role", fragmentsByRole}, {"artifacts_by_role", artifactsByRole},
    {"requirements_by_role", resources}, {"cross_partition_tensors", crossPartitionTensors},
    {"estimated_costs", costs}, {"tensor_degrees_by_role", tensorDegreesByRole},
    {"rank_artifact_digests_by_role", rankArtifactDigestsByRole},
    {"role_state_inputs_by_role", states(roleStateInputsByRole)},
    {"role_state_outputs_by_role", states(roleStateOutputsByRole)},
    {"hybrid_plan", hybridPlan ? nativeParseJson(hybridPlan->canonicalJson()) : NativeJson(nullptr)},
    {"selection_priority", selectionPriority}, {"input_ingress_role", inputIngressRole},
    {"result_egress_role", resultEgressRole}, {"merge_kind", mergeKind}, {"postprocessing", postprocessing}});
}

std::string NativeSplitCandidate::computedDigest() const
{
  return nativePlanningDigest(canonicalJson());
}

void NativeSplitCandidate::validate(const NativeGraphSnapshot& graph) const
{
  splitter.validate();
  model.validate();
  graph.validate(model);
  if ((source != "PRE_SPLIT" && source != "GENERATED") || selectionPriority < 0 ||
      (mergeKind != "" && mergeKind != "NATIVE_POSTPROCESS" && mergeKind != "ONNX_MERGE_GRAPH"))
    throw std::invalid_argument("invalid split candidate source, priority or merge kind");
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
    requirementsByRole.at(role).validate();
    if (std::any_of(artifactsByRole.at(role).begin(), artifactsByRole.at(role).end(),
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
  if (hybridPlan) {
    hybridPlan->validate();
    std::vector<std::uint64_t> degrees;
    for (const auto& role : executionPlan.roles) {
      if (!tensorDegreesByRole.count(role)) throw std::invalid_argument("hybrid plan requires tensor degrees");
      degrees.push_back(tensorDegreesByRole.at(role));
    }
    if (degrees != hybridPlan->tensorDegrees)
      throw std::invalid_argument("hybrid candidate degree vector mismatches its plan");
  }
  else if (std::any_of(tensorDegreesByRole.begin(), tensorDegreesByRole.end(),
                       [](const auto& role) { return role.second != 1; }))
    throw std::invalid_argument("hybrid candidate requires a sealed hybrid plan");
  if (candidateDigest != computedDigest())
    throw std::invalid_argument("split candidate digest does not bind its complete contract");
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
  std::map<std::string, std::uint64_t> reservedBytes;
  for (const auto& role : executionPlan.roles) {
    const auto it = assignment.providerByRole.find(role);
    if (it == assignment.providerByRole.end() || it->second.empty()) {
      throw std::invalid_argument("placement proposal has an unassigned role");
    }
    const auto offer = std::find_if(snapshot.offers.begin(), snapshot.offers.end(),
      [&it] (const auto& view) { return view.provider == it->second; });
    const auto peak = candidate.requirementsByRole.at(role).estimatedPeakGpuMemoryBytes();
    const auto reserved = reservedBytes.find(it->second);
    const auto alreadyReserved = reserved == reservedBytes.end() ? 0 : reserved->second;
    if (offer == snapshot.offers.end() || !peak || alreadyReserved > offer->freeBytes ||
        *peak > offer->freeBytes - alreadyReserved ||
        !canPlaceRole(*offer, role, candidate.requirementsByRole.at(role))) {
      throw std::invalid_argument("placement proposal requires feasible Providers");
    }
    if (alreadyReserved > std::numeric_limits<std::uint64_t>::max() - *peak)
      throw std::overflow_error("placement Provider memory reservation overflows uint64");
    reservedBytes[it->second] = alreadyReserved + *peak;
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
  std::map<std::string, std::uint64_t> reservedBytes;
  auto roles = candidate.executionPlan.roles;
  std::sort(roles.begin(), roles.end());
  for (const auto& role : roles) {
    std::vector<const NativeProviderPlanningView*> eligible;
    std::vector<const NativeProviderPlanningView*> distinct;
    const auto peak = candidate.requirementsByRole.at(role).estimatedPeakGpuMemoryBytes();
    for (const auto& offer : snapshot.offers) {
      const auto reserved = reservedBytes.find(offer.provider);
      const auto alreadyReserved = reserved == reservedBytes.end() ? 0 : reserved->second;
      const bool fits = peak && alreadyReserved <= offer.freeBytes &&
        *peak <= offer.freeBytes - alreadyReserved;
      if (fits && canPlaceRole(offer, role, candidate.requirementsByRole.at(role))) {
        eligible.push_back(&offer);
        if (!usedProviders.count(offer.provider)) distinct.push_back(&offer);
      }
    }
    // Spread roles when possible, but permit a smaller deployment to
    // co-locate roles on one capable Provider.
    if (!distinct.empty()) eligible.swap(distinct);
    if (eligible.empty()) {
      throw std::runtime_error("no feasible Provider for native role " + role);
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
    const auto selectedPeak = candidate.requirementsByRole.at(role).estimatedPeakGpuMemoryBytes();
    if (selectedPeak) {
      const auto provider = eligible.front()->provider;
      const auto reserved = reservedBytes.find(provider);
      const auto alreadyReserved = reserved == reservedBytes.end() ? 0 : reserved->second;
      if (alreadyReserved > std::numeric_limits<std::uint64_t>::max() - *selectedPeak)
        throw std::overflow_error("placement Provider memory reservation overflows uint64");
      reservedBytes[provider] = alreadyReserved + *selectedPeak;
    }
  }
  result.validate(snapshot, candidate);
  return result;
}

void NativeAdapterRegistry::replaceAdapter(
  std::shared_ptr<const NativeModelAdapter> adapter)
{
  if (m_frozen) throw std::logic_error("native adapter registry is frozen");
  if (!adapter || adapter->adapterId().empty())
    throw std::invalid_argument("invalid native adapter");
  const auto id = adapter->adapterId();
  const auto it = m_adapters.find(id);
  if (it == m_adapters.end())
    throw std::out_of_range("native adapter is not registered: " + id);
  it->second = std::move(adapter);
}

void NativePlacementStrategyRegistry::registerStrategy(
  std::string id, std::shared_ptr<const CooperativePlacementStrategy> strategy)
{
  if (m_frozen) throw std::logic_error("placement strategy registry is frozen");
  if (id.empty() || !strategy)
    throw std::invalid_argument("invalid cooperative placement strategy");
  const auto identity = strategy->identity();
  identity.validate();
  if (id != identity.name)
    throw std::invalid_argument("placement strategy registry id does not match identity");
  if (!m_strategies.emplace(std::move(id), std::move(strategy)).second)
    throw std::invalid_argument("placement strategy is already registered");
}

void NativePlacementStrategyRegistry::replaceStrategy(
  std::string id, std::shared_ptr<const CooperativePlacementStrategy> strategy)
{
  if (m_frozen) throw std::logic_error("placement strategy registry is frozen");
  if (id.empty() || !strategy)
    throw std::invalid_argument("invalid cooperative placement strategy");
  const auto identity = strategy->identity();
  identity.validate();
  if (id != identity.name)
    throw std::invalid_argument("placement strategy registry id does not match identity");
  const auto it = m_strategies.find(id);
  if (it == m_strategies.end())
    throw std::out_of_range("placement strategy is not registered: " + id);
  it->second = std::move(strategy);
}

void NativePlacementStrategyRegistry::freeze()
{
  m_frozen = true;
}

std::shared_ptr<const CooperativePlacementStrategy>
NativePlacementStrategyRegistry::find(const std::string& id) const
{
  const auto it = m_strategies.find(id);
  return it == m_strategies.end() ? nullptr : it->second;
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
