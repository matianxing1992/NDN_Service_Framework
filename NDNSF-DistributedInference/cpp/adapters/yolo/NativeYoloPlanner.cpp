#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace ndnsf::di::yolo {
namespace {

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

NativeYoloComponentSplit::NativeYoloComponentSplit(
  std::vector<NativeYoloComponentSpec> candidates)
  : m_candidates(std::move(candidates))
{
  if (m_candidates.empty()) {
    throw std::invalid_argument("YOLO native splitter has no registered candidates");
  }
  std::set<std::string> ids;
  for (const auto& candidate : m_candidates) {
    if (candidate.candidateId.empty() || !ids.insert(candidate.candidateId).second ||
        candidate.roles.empty() || candidate.inputIngressRole.empty() ||
        candidate.resultEgressRole.empty() ||
        std::set<std::string>(candidate.roles.begin(), candidate.roles.end()).size() !=
          candidate.roles.size()) {
      throw std::invalid_argument("YOLO native candidate identity is invalid");
    }
    if (!candidate.candidateDigest.empty() && !isDigest(candidate.candidateDigest)) {
      throw std::invalid_argument("YOLO native candidate digest is invalid");
    }
  }
}

NativeStrategyIdentity NativeYoloComponentSplit::identity() const
{
  return {"native-yolo-component-split", "1",
          nativePlanningDigest("yolo-component-split|1")};
}

std::vector<NativeSplitCandidate>
NativeYoloComponentSplit::enumerate(const NativeModelDescriptor& model,
                                    const NativeGraphSnapshot& graph,
                                    const NativeCandidateBudget& budget) const
{
  budget.validate();
  model.validate();
  graph.validate(model);
  if (model.adapterId.find("yolo") == std::string::npos &&
      model.modelName.find("YOLO") == std::string::npos &&
      model.modelName.find("Yolo") == std::string::npos) {
    throw std::invalid_argument("YOLO native splitter received another adapter");
  }
  std::vector<const NativeYoloComponentSpec*> ordered;
  ordered.reserve(m_candidates.size());
  for (const auto& candidate : m_candidates) ordered.push_back(&candidate);
  std::stable_sort(ordered.begin(), ordered.end(), [] (auto left, auto right) {
    if (left->priority != right->priority) return left->priority < right->priority;
    return left->candidateId < right->candidateId;
  });
  if (ordered.size() > budget.maxCandidates) ordered.resize(budget.maxCandidates);

  std::vector<NativeSplitCandidate> result;
  result.reserve(ordered.size());
  for (const auto* spec : ordered) {
    if (!contains(spec->roles, spec->inputIngressRole) ||
        !contains(spec->roles, spec->resultEgressRole)) {
      throw std::invalid_argument("YOLO candidate ingress/egress is undeclared");
    }
    std::map<std::string, std::string> ownerByNode;
    if (spec->roles.size() == 1 && spec->roles.front() == "FullModel" &&
        spec->nodeNamesByRole.empty()) {
      for (const auto& node : graph.nodes) ownerByNode.emplace(node.id, spec->roles.front());
    }
    else {
      if (spec->nodeNamesByRole.size() != spec->roles.size()) {
        throw std::invalid_argument("YOLO semantic role partition is incomplete");
      }
      for (const auto& role : spec->roles) {
        const auto names = spec->nodeNamesByRole.find(role);
        if (names == spec->nodeNamesByRole.end() || names->second.empty()) {
          throw std::invalid_argument("YOLO semantic role node set is empty");
        }
        for (const auto& node : names->second) {
          if (node.empty() || !ownerByNode.emplace(node, role).second) {
            throw std::invalid_argument("YOLO semantic role assigns a node twice");
          }
        }
      }
      if (ownerByNode.size() != graph.nodes.size()) {
        throw std::invalid_argument("YOLO semantic role partition does not cover graph");
      }
      for (const auto& node : graph.nodes) {
        if (!ownerByNode.count(node.id)) {
          throw std::invalid_argument("YOLO semantic role names do not match graph");
        }
      }
    }

    NativeSplitCandidate candidate;
    candidate.source = "PRE_SPLIT";
    candidate.splitter = identity();
    candidate.model = model;
    candidate.graphDigest = graph.graphDigest;
    candidate.executionPlan.version = 1;
    candidate.executionPlan.modelName = model.modelName;
    candidate.executionPlan.modelFamily = "yolo";
    candidate.executionPlan.modelFormat = model.modelFormat;
    candidate.executionPlan.plannerKind = "native-yolo-component";
    candidate.executionPlan.executionPolicy = "DATA_DRIVEN_V2";
    candidate.executionPlan.roles = spec->roles;
    candidate.nodeRoles = ownerByNode;
    candidate.selectionPriority = spec->priority;
    candidate.inputIngressRole = spec->inputIngressRole;
    candidate.resultEgressRole = spec->resultEgressRole;
    candidate.mergeKind = spec->mergeKind;
    std::uint64_t knownBytes = 0;
    for (const auto& edge : graph.edges) {
      const auto size = edge.tensor.estimatedBytes.value_or(0);
      if (size > std::numeric_limits<std::uint64_t>::max() - knownBytes)
        throw std::invalid_argument("YOLO graph tensor byte estimate overflows");
      knownBytes += size;
      const auto& producerRole = ownerByNode.at(edge.producer);
      std::set<std::string> consumerRoles;
      for (const auto& consumer : edge.consumers) {
        if (ownerByNode.at(consumer) != producerRole) consumerRoles.insert(ownerByNode.at(consumer));
      }
      if (consumerRoles.empty()) continue;
      if (!contains(graph.legalCutEdges, edge.id))
        throw std::invalid_argument("YOLO candidate crosses an illegal tensor edge");
      candidate.crossPartitionTensors.push_back(edge.id);
      for (const auto& consumerRole : consumerRoles) {
        NativeDependencySpec dependency;
        dependency.producers = {producerRole};
        dependency.consumers = {consumerRole};
        dependency.keyScope = "native-yolo-activation";
        dependency.topicPrefix = "/NDNSF/DI/YOLO";
        dependency.objectNameTemplate =
          "{producerProvider}/NDNSF/DI/YOLO/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
        dependency.tensors = {edge.id};
        dependency.operationKind = "ACTIVATION";
        candidate.executionPlan.dependencies.push_back(std::move(dependency));
      }
    }
    const auto roleBytes = std::max<std::uint64_t>(1, knownBytes / spec->roles.size());
    for (const auto& role : spec->roles) {
      candidate.fragmentsByRole[role] = nativePlanningDigest(
        "yolo-fragment|" + spec->candidateId + "|" + role + "|" + graph.graphDigest);
      candidate.artifactsByRole[role] = {candidate.fragmentsByRole[role]};
      candidate.requirementsByRole[role] = {
        {"onnxruntime-cpu", "onnxruntime-cuda", "onnxruntime"},
        roleBytes, 256ULL * 1024ULL * 1024ULL,
        256ULL * 1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL, 1.0};
      candidate.tensorDegreesByRole[role] = 1;
      candidate.rankArtifactDigestsByRole[role] = candidate.artifactsByRole.at(role);
    }
    candidate.candidateDigest = spec->candidateDigest.empty()
      ? nativePlanningDigest("yolo-candidate|" + spec->candidateId + "|" +
                             model.contentDigest + "|" + graph.graphDigest)
      : spec->candidateDigest;
    candidate.validate(graph);
    result.push_back(std::move(candidate));
  }
  return result;
}

} // namespace ndnsf::di::yolo
