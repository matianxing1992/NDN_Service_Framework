#include "NDNSF-DistributedInference/cpp/adapters/yolo/NativeYoloPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

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

std::set<std::string> stringSet(const NativeJson& value)
{
  if (!value.is_array()) throw std::invalid_argument("YOLO semantic set must be an array");
  const auto items = value.get<std::vector<std::string>>();
  return {items.begin(), items.end()};
}

using RoleEdges = std::map<std::pair<std::string, std::string>, std::set<std::string>>;

RoleEdges declaredRoleEdges(const NativeJson& values, const char* tensors)
{
  if (!values.is_array()) throw std::invalid_argument("YOLO semantic dependencies must be an array");
  RoleEdges result;
  for (const auto& item : values) {
    const auto key = std::make_pair(item.at("fromRole").get<std::string>(),
                                    item.at("toRole").get<std::string>());
    if (!result.emplace(key, stringSet(item.at(tensors))).second)
      throw std::invalid_argument("YOLO semantic dependency pair is duplicated");
  }
  return result;
}

// Consume the registered partition against facts obtained from actual ONNX
// bytes. The graph IDs and semantic names are deliberately separate domains.
void bindSemanticPartition(NativeYoloComponentSpec& spec, const NativeJson& partition,
                           const NativeOnnxGraphInspection& inspection)
{
  const auto& graph = inspection.graph;
  spec.nodeNamesByRole.clear();
  if (spec.candidateId == "atomic-v1") {
    if (spec.roles.size() != 1)
      throw std::invalid_argument("YOLO atomic candidate requires one role");
    spec.nodeNamesByRole[spec.roles.front()] = graph.topologicalOrder;
    return;
  }
  const auto& roleSets = partition.at("roleNodeSets");
  if (!roleSets.is_object() || roleSets.size() != spec.roles.size() ||
      inspection.nodeNames.size() != graph.topologicalOrder.size())
    throw std::invalid_argument("YOLO semantic role partition is incomplete");
  std::map<std::string, std::string> ownerByName, nameByNode, ownerByNode;
  for (const auto& role : spec.roles) {
    const auto& names = roleSets.at(role);
    if (!names.is_array() || names.empty())
      throw std::invalid_argument("YOLO semantic role node set is invalid");
    for (const auto& item : names) {
      const auto name = item.get<std::string>();
      if (name.empty() || !ownerByName.emplace(name, role).second)
        throw std::invalid_argument("YOLO semantic role assigns a node twice");
    }
  }
  const std::set<std::string> actualNames(inspection.nodeNames.begin(), inspection.nodeNames.end());
  if (actualNames.size() != ownerByName.size())
    throw std::invalid_argument("YOLO semantic partition does not cover graph names");
  for (std::size_t i = 0; i < graph.topologicalOrder.size(); ++i) {
    const auto& name = inspection.nodeNames[i];
    const auto found = ownerByName.find(name);
    if (found == ownerByName.end())
      throw std::invalid_argument("YOLO semantic partition contains foreign names");
    const auto& node = graph.topologicalOrder[i];
    nameByNode.emplace(node, name);
    ownerByNode.emplace(node, found->second);
    spec.nodeNamesByRole[found->second].push_back(node);
  }

  const auto& interfaces = partition.at("tensorInterfaces");
  if (!interfaces.is_array())
    throw std::invalid_argument("YOLO tensor interfaces must be an array");
  std::map<std::string, NativeJson> declared;
  for (const auto& item : interfaces)
    if (!declared.emplace(item.at("edgeId").get<std::string>(), item).second)
      throw std::invalid_argument("YOLO tensor interface is duplicated");
  RoleEdges actualDependencies;
  for (const auto& edge : graph.edges) {
    const auto& producerRole = ownerByNode.at(edge.producer);
    std::set<std::string> consumerNames, consumerRoles;
    for (const auto& consumer : edge.consumers) {
      const auto& role = ownerByNode.at(consumer);
      if (role != producerRole) {
        consumerNames.insert(nameByNode.at(consumer));
        consumerRoles.insert(role);
        actualDependencies[{producerRole, role}].insert(edge.id);
      }
    }
    if (consumerRoles.empty()) continue;
    const auto found = declared.find(edge.id);
    if (found == declared.end())
      throw std::invalid_argument("YOLO tensor interface coverage differs from graph");
    const auto& item = found->second;
    auto shape = NativeJson::array();
    for (const auto& dimension : edge.tensor.shape)
      std::visit([&](const auto& value) { shape.push_back(value); }, dimension);
    if (item.at("producerNode") != nameByNode.at(edge.producer) ||
        item.at("producerRole") != producerRole ||
        stringSet(item.at("consumerNodes")) != consumerNames ||
        stringSet(item.at("consumerRoles")) != consumerRoles ||
        item.at("dtype") != edge.tensor.dtype || item.at("shape") != shape)
      throw std::invalid_argument("YOLO tensor interface differs from actual graph");
    declared.erase(found);
  }
  if (!declared.empty() ||
      declaredRoleEdges(partition.at("dependencyEdges"), "tensorEdges") != actualDependencies ||
      declaredRoleEdges(partition.at("safeCuts"), "boundaryTensors") != actualDependencies)
    throw std::invalid_argument("YOLO dependencies or safe cuts differ from actual graph");

  const auto metadata = nativeParseJson(inspection.graphMetadataJson);
  const auto inputs = stringSet(metadata.at("inputs"));
  const auto outputs = stringSet(metadata.at("outputs"));
  std::map<std::string, std::string> producers;
  std::map<std::string, std::set<std::string>> consumers;
  for (const auto& node : metadata.at("nodes")) {
    const auto name = node.at("name").get<std::string>();
    for (const auto& tensor : stringSet(node.at("outputs"))) producers[tensor] = name;
    for (const auto& tensor : stringSet(node.at("inputs"))) consumers[tensor].insert(name);
  }
  std::map<std::string, std::set<std::string>> roleInputs, roleOutputs;
  for (const auto& node : metadata.at("nodes")) {
    const auto& role = ownerByName.at(node.at("name").get<std::string>());
    for (const auto& tensor : stringSet(node.at("inputs"))) {
      const auto producer = producers.find(tensor);
      if (inputs.count(tensor) || (producer != producers.end() && ownerByName.at(producer->second) != role))
        roleInputs[role].insert(tensor);
    }
    for (const auto& tensor : stringSet(node.at("outputs")))
      if (outputs.count(tensor) || std::any_of(consumers[tensor].begin(), consumers[tensor].end(),
          [&](const auto& name) { return ownerByName.at(name) != role; }))
        roleOutputs[role].insert(tensor);
  }
  const auto endpoints = [](const NativeJson& values) {
    if (!values.is_array()) throw std::invalid_argument("YOLO role endpoints must be an array");
    std::set<std::string> result;
    for (const auto& item : values)
      result.insert(nativeCanonicalJson(NativeJson{{"name", item.at("name")},
        {"dtype", item.value("dtype", NativeJson("unknown"))},
        {"shape", item.value("shape", NativeJson::array())}}));
    return result;
  };
  const auto actualEndpoints = [&](const std::set<std::string>& tensors) {
    auto result = NativeJson::array();
    for (const auto& tensor : tensors) {
      const auto info = metadata.at("tensors").value(tensor, NativeJson::object());
      result.push_back(NativeJson{{"name", tensor}, {"dtype", info.value("dtype", NativeJson("unknown"))},
        {"shape", info.value("shape", NativeJson::array())}});
    }
    return endpoints(result);
  };
  const auto& roleInterfaces = partition.at("roleInterfaces");
  if (!roleInterfaces.is_object() || roleInterfaces.size() != spec.roles.size())
    throw std::invalid_argument("YOLO role interface coverage differs from graph");
  for (const auto& role : spec.roles) {
    const auto& item = roleInterfaces.at(role);
    if (endpoints(item.at("inputs")) != actualEndpoints(roleInputs[role]) ||
        endpoints(item.at("outputs")) != actualEndpoints(roleOutputs[role]))
      throw std::invalid_argument("YOLO role interface differs from actual graph");
  }
}

} // namespace

NativeYoloComponentSplit::NativeYoloComponentSplit(
  std::vector<NativeYoloComponentSpec> candidates, std::string postprocessingJson)
  : m_candidates(std::move(candidates))
  , m_postprocessingJson(std::move(postprocessingJson))
{
  if (!nativeParseJson(m_postprocessingJson).is_object())
    throw std::invalid_argument("YOLO postprocessing must be an object");
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
    if (!isDigest(candidate.candidateDigest)) {
      throw std::invalid_argument("YOLO native candidate digest is invalid");
    }
  }
}

NativeStrategyIdentity NativeYoloComponentSplit::identity() const
{
  return {"native-yolo-component-split", "1",
          nativePlanningDigest("yolo-component-split|1")};
}

NativeYoloComponentSplit NativeYoloComponentSplit::fromOnnxCatalog(
  const NativeModelDescriptor& model, const NativeCanonicalSource& source,
  const NativeAssemblyControl& control, std::vector<NativeYoloCatalogComponent> candidates,
  std::string postprocessingJson)
{
  const auto inspected = inspectNativeOnnxPlanningGraph(source, model, control);
  std::vector<NativeYoloComponentSpec> bound;
  for (auto& candidate : candidates) {
    control.requireActive();
    bindSemanticPartition(candidate.component,
      nativeParseJson(candidate.semanticPartitionJson), inspected);
    bound.push_back(std::move(candidate.component));
  }
  NativeYoloComponentSplit result(std::move(bound), std::move(postprocessingJson));
  result.m_catalogModelDigest = model.modelDigest();
  result.m_catalogGraph = inspected.graph;
  control.requireActive();
  if (std::chrono::steady_clock::now() >= control.deadline)
    throw std::runtime_error("ASSEMBLY_TIMEOUT");
  return result;
}

std::vector<NativeSplitCandidate>
NativeYoloComponentSplit::enumerate(const NativeModelDescriptor& model,
                                    const NativeGraphSnapshot& suppliedGraph,
                                    const NativeCandidateBudget& budget) const
{
  budget.validate();
  model.validate();
  if (m_catalogGraph && (model.modelDigest() != m_catalogModelDigest ||
      suppliedGraph.graphDigest != m_catalogGraph->graphDigest))
    throw std::invalid_argument("YOLO catalog splitter received a foreign model or graph");
  const auto& graph = m_catalogGraph ? *m_catalogGraph : suppliedGraph;
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
    candidate.mergeKind = contains(spec->roles, "Merge") ? spec->mergeKind : "";
    candidate.postprocessingJson = contains(spec->roles, "Merge") ? m_postprocessingJson : "{}";
    std::uint64_t knownBytes = 0;
    std::uint64_t crossedBytes = 0;
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
      crossedBytes += size; // Bounded by the already overflow-checked total.
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
      std::vector<std::string> roleNodes;
      for (const auto& node : graph.topologicalOrder)
        if (ownerByNode.at(node) == role) roleNodes.push_back(node);
      candidate.fragmentsByRole[role] = nativePlanningDigest(nativeCanonicalJson(NativeJson{
        {"candidate", spec->candidateDigest}, {"graph", graph.graphDigest},
        {"role", role}, {"nodes", roleNodes}}));
      candidate.artifactsByRole[role] = {candidate.fragmentsByRole[role]};
      candidate.requirementsByRole[role] = {
        {"onnxruntime-cpu", "onnxruntime-cuda"},
        roleBytes, 256ULL * 1024ULL * 1024ULL, 0,
        256ULL * 1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL, 1.1};
    }
    candidate.estimatedCosts = {{"role_count", std::uint64_t(spec->roles.size())},
      {"known_transfer_bytes", crossedBytes}};
    candidate.candidateDigest = candidate.computedDigest();
    candidate.validate(graph);
    result.push_back(std::move(candidate));
  }
  return result;
}

} // namespace ndnsf::di::yolo
