#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {

std::vector<std::string>
stringArrayFromJson(const boost::property_tree::ptree& node, const std::string& key)
{
  std::vector<std::string> values;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return values;
  }
  for (const auto& item : child.get()) {
    values.push_back(item.second.get_value<std::string>());
  }
  return values;
}

namespace {

std::vector<std::uint64_t>
uintArrayFromJson(const boost::property_tree::ptree& node, const std::string& key)
{
  std::vector<std::uint64_t> values;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return values;
  }
  for (const auto& item : *child) {
    values.push_back(item.second.get_value<std::uint64_t>());
  }
  return values;
}

std::vector<NativeAssemblyTensorContractV3>
assemblyTensorContractsFromJson(const boost::property_tree::ptree& node,
                                const std::string& key)
{
  std::vector<NativeAssemblyTensorContractV3> values;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return values;
  }
  for (const auto& item : *child) {
    NativeAssemblyTensorContractV3 value;
    value.name = item.second.get<std::string>("name", "");
    value.dtype = item.second.get<std::string>("dtype", "");
    value.shape = stringArrayFromJson(item.second, "shape");
    if (value.name.empty() || value.dtype.empty()) {
      throw std::invalid_argument("invalid V3 assembly tensor contract");
    }
    values.push_back(std::move(value));
  }
  return values;
}

bool
sameAssemblyTensorContracts(
  const std::vector<NativeAssemblyTensorContractV3>& left,
  const std::vector<NativeAssemblyTensorContractV3>& right)
{
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index].name != right[index].name ||
        left[index].dtype != right[index].dtype ||
        left[index].shape != right[index].shape) {
      return false;
    }
  }
  return true;
}

bool
isSha256Digest(const std::string& value)
{
  return value.size() == 71 && value.rfind("sha256:", 0) == 0 &&
         std::all_of(value.begin() + 7, value.end(), [] (unsigned char ch) {
           return std::isxdigit(ch) != 0;
         });
}

bool
hasCompleteAdapterIdentity(const std::string& adapterId,
                           const std::string& adapterVersion)
{
  return adapterId.empty() == adapterVersion.empty();
}

bool
isCpuBackend(const std::string& backend)
{
  return backend == "cpu" ||
         (backend.size() > 4 &&
          backend.compare(backend.size() - 4, 4, "-cpu") == 0);
}

NativeSelectionRoleV3
selectionRoleFromV3Json(const boost::property_tree::ptree& node,
                        const std::string& selectedRole)
{
  NativeSelectionRoleV3 role;
  role.role = node.get<std::string>("role", "");
  role.selectedRole = selectedRole;
  role.rank = node.get<std::uint64_t>("rank", 0);
  role.layerBegin = node.get<std::uint64_t>("layer_begin", 0);
  role.layerEnd = node.get<std::uint64_t>("layer_end", 0);
  role.backend = node.get<std::string>("backend", "");
  role.deviceSet = stringArrayFromJson(node, "device_set");
  role.requiredDeviceMemoryMb =
    node.get<std::uint64_t>("required_device_memory_mb", 0);
  role.artifactDigest = node.get<std::string>("artifact_digest", "");
  role.recipeDigest = node.get<std::string>("recipe_digest", "");
  role.roleKind = node.get<std::string>("role_kind", "");
  role.adapterId = node.get<std::string>("adapter_id", "");
  role.adapterVersion = node.get<std::string>("adapter_version", "");
  role.modelManifestDigest = node.get<std::string>("model_manifest_digest", "");
  role.artifactProfileDigest = node.get<std::string>("artifact_profile_digest", "");
  role.graphDigest = node.get<std::string>("graph_digest", "");
  role.canonicalInitializerDigest = node.get<std::string>(
    "canonical_initializer_digest", "");
  role.adapterDescriptorDigest = node.get<std::string>(
    "adapter_descriptor_digest", "");
  role.assemblerDescriptorDigest = node.get<std::string>(
    "assembler_descriptor_digest", "");
  role.backendAbi = node.get<std::string>("backend_abi", "");
  role.nodeIndices = uintArrayFromJson(node, "node_indices");
  role.expectedInputs = assemblyTensorContractsFromJson(node, "expected_inputs");
  role.expectedOutputs = assemblyTensorContractsFromJson(node, "expected_outputs");
  role.precision = node.get<std::string>("precision", "");
  role.quantization = node.get<std::string>("quantization", "");
  role.layout = node.get<std::string>("layout", "");
  role.padding = node.get<std::string>("padding", "");
  role.protectionEpoch = node.get<std::string>(
    "protection_epoch", "plaintext-v1");
  if (const auto envelope = node.get_child_optional("resource_envelope")) {
    role.maxSourceBytes = envelope->get<std::uint64_t>("maxSourceBytes", 0);
    role.maxAssembledBytes = envelope->get<std::uint64_t>("maxAssembledBytes", 0);
    role.maxNodes = envelope->get<std::uint64_t>("maxNodes", 0);
  }
  const bool hasAssemblyIdentity =
    !role.modelManifestDigest.empty() || !role.artifactProfileDigest.empty() ||
    !role.graphDigest.empty() || !role.canonicalInitializerDigest.empty() ||
    !role.adapterDescriptorDigest.empty() ||
    !role.assemblerDescriptorDigest.empty() || !role.backendAbi.empty();
  const bool completeAssemblyIdentity =
    isSha256Digest(role.modelManifestDigest) &&
    isSha256Digest(role.artifactProfileDigest) &&
    isSha256Digest(role.graphDigest) &&
    isSha256Digest(role.canonicalInitializerDigest) &&
    isSha256Digest(role.adapterDescriptorDigest) &&
    isSha256Digest(role.assemblerDescriptorDigest) && !role.backendAbi.empty();
  const std::set<std::uint64_t> uniqueNodes(
    role.nodeIndices.begin(), role.nodeIndices.end());
  const bool completeAssemblyRecipe =
    !role.nodeIndices.empty() && uniqueNodes.size() == role.nodeIndices.size() &&
    std::is_sorted(role.nodeIndices.begin(), role.nodeIndices.end()) &&
    !role.expectedInputs.empty() && !role.expectedOutputs.empty() &&
    !role.precision.empty() && !role.quantization.empty() &&
    !role.layout.empty() && !role.padding.empty() &&
    role.maxSourceBytes > 0 && role.maxAssembledBytes > 0 && role.maxNodes > 0;
  if (role.role.empty() || role.selectedRole.empty() ||
      role.layerEnd <= role.layerBegin || role.backend.empty() ||
      (isCpuBackend(role.backend) && !role.deviceSet.empty()) ||
      (!isCpuBackend(role.backend) && role.deviceSet.size() != 1) ||
      !isSha256Digest(role.artifactDigest) ||
      !isSha256Digest(role.recipeDigest) ||
      (role.roleKind != "PIPELINE_RANGE" &&
       role.roleKind != "TENSOR_RANK" &&
       role.roleKind != "HYBRID_RANK" &&
       role.roleKind != "COMPONENT_SET") ||
      !hasCompleteAdapterIdentity(role.adapterId, role.adapterVersion) ||
      role.protectionEpoch.empty()) {
    throw std::invalid_argument(
      "V3 Selection projection contains an incomplete local role");
  }
  if (hasAssemblyIdentity &&
      (!completeAssemblyIdentity || !completeAssemblyRecipe)) {
    throw std::invalid_argument(
      "V3 Selection projection contains an incomplete assembly identity");
  }
  return role;
}

bool
sameAssembly(const NativeSelectionRoleV3& left,
             const NativeSelectionRoleV3& right)
{
  return left.role == right.role && left.selectedRole == right.selectedRole &&
         left.rank == right.rank && left.layerBegin == right.layerBegin &&
         left.layerEnd == right.layerEnd && left.backend == right.backend &&
         left.deviceSet == right.deviceSet &&
         left.protectionEpoch == right.protectionEpoch &&
         left.requiredDeviceMemoryMb == right.requiredDeviceMemoryMb &&
         left.artifactDigest == right.artifactDigest &&
         left.recipeDigest == right.recipeDigest &&
         left.roleKind == right.roleKind && left.adapterId == right.adapterId &&
         left.adapterVersion == right.adapterVersion &&
         left.modelManifestDigest == right.modelManifestDigest &&
         left.artifactProfileDigest == right.artifactProfileDigest &&
         left.graphDigest == right.graphDigest &&
         left.canonicalInitializerDigest == right.canonicalInitializerDigest &&
         left.adapterDescriptorDigest == right.adapterDescriptorDigest &&
         left.assemblerDescriptorDigest == right.assemblerDescriptorDigest &&
         left.backendAbi == right.backendAbi &&
         left.nodeIndices == right.nodeIndices &&
         sameAssemblyTensorContracts(left.expectedInputs, right.expectedInputs) &&
         sameAssemblyTensorContracts(left.expectedOutputs, right.expectedOutputs) &&
         left.precision == right.precision &&
         left.quantization == right.quantization && left.layout == right.layout &&
         left.padding == right.padding &&
         left.maxSourceBytes == right.maxSourceBytes &&
         left.maxAssembledBytes == right.maxAssembledBytes &&
         left.maxNodes == right.maxNodes;
}

NativeExecutionRoleV3
executionRoleFromV3Json(const boost::property_tree::ptree& node)
{
  NativeExecutionRoleV3 role;
  role.roleId = node.get<std::string>("role_id", "");
  role.stageId = node.get<std::string>("stage_id", "");
  role.rank = node.get<std::uint64_t>("rank", 0);
  role.layerBegin = node.get<std::uint64_t>("layer_begin", 0);
  role.layerEnd = node.get<std::uint64_t>("layer_end", 0);
  role.backend = node.get<std::string>("backend", "");
  role.adapterId = node.get<std::string>("adapter_id", "");
  role.adapterVersion = node.get<std::string>("adapter_version", "");
  if (role.roleId.empty() || role.stageId.empty() ||
      role.layerEnd <= role.layerBegin || role.backend.empty() ||
      !hasCompleteAdapterIdentity(role.adapterId, role.adapterVersion)) {
    throw std::invalid_argument("invalid V3 execution role");
  }
  return role;
}

NativeTensorEndpointV3
tensorEndpointFromV3Json(const boost::property_tree::ptree& node)
{
  NativeTensorEndpointV3 endpoint;
  endpoint.producerNamespace =
    node.get<std::string>("producer_namespace", "");
  endpoint.requester = node.get<std::string>("requester", "");
  endpoint.requestId = node.get<std::string>("request_id", "");
  endpoint.attempt = node.get<std::uint64_t>("attempt", 0);
  endpoint.planDigest = node.get<std::string>("plan_digest", "");
  endpoint.groupId = node.get<std::string>("group_id", "");
  endpoint.groupEpoch = node.get<std::string>("group_epoch", "");
  endpoint.operation = node.get<std::string>("operation", "");
  endpoint.round = node.get<std::uint64_t>("round", 0);
  endpoint.sourceKind = node.get<std::string>("source_kind", "");
  endpoint.producerRole = node.get<std::string>("producer_role", "");
  endpoint.producerRank = node.get<std::uint64_t>("producer_rank", 0);
  endpoint.consumerRole = node.get<std::string>("consumer_role", "");
  endpoint.consumerRoles = stringArrayFromJson(node, "consumer_roles");
  if (endpoint.consumerRoles.empty() && !endpoint.consumerRole.empty()) {
    endpoint.consumerRoles = {endpoint.consumerRole};
  }
  endpoint.tensorId = node.get<std::string>("tensor_id", "");
  endpoint.tensorDigest = node.get<std::string>("tensor_digest", "");
  endpoint.layoutDigest = node.get<std::string>("layout_digest", "");
  endpoint.targetLayoutDigest = node.get<std::string>(
    "target_layout_digest", endpoint.layoutDigest);
  endpoint.microbatch = node.get<std::uint64_t>("microbatch", 0);
  endpoint.segmentCount = node.get<std::size_t>("segment_count", 0);
  endpoint.manifestDigest = node.get<std::string>("manifest_digest", "");
  endpoint.securityProfile = node.get<std::string>("security_profile", "");
  endpoint.noProgressDeadlineMs =
    node.get<std::uint64_t>("no_progress_deadline_ms", 0);
  endpoint.hardDeadlineMs =
    node.get<std::uint64_t>("hard_deadline_ms", 0);
  endpoint.endpointDigest = node.get<std::string>("endpoint_digest", "");
  const bool roleSource = endpoint.sourceKind == "ROLE";
  const bool applicationInput = endpoint.sourceKind == "APPLICATION_INPUT";
  if (endpoint.producerNamespace.empty() ||
      endpoint.producerNamespace.front() != '/' || endpoint.requester.empty() ||
      endpoint.requester.front() != '/' || endpoint.requestId.empty() ||
      endpoint.attempt == 0 || !isSha256Digest(endpoint.planDigest) ||
      endpoint.groupId.empty() || endpoint.groupEpoch.empty() ||
      endpoint.operation.empty() || (!roleSource && !applicationInput) ||
      (roleSource && endpoint.producerRole.empty()) ||
      (applicationInput && !endpoint.producerRole.empty()) ||
      endpoint.consumerRole.empty() || endpoint.consumerRoles.empty() ||
      std::find(endpoint.consumerRoles.begin(), endpoint.consumerRoles.end(),
                endpoint.consumerRole) == endpoint.consumerRoles.end() ||
      std::set<std::string>(endpoint.consumerRoles.begin(),
                            endpoint.consumerRoles.end()).size() !=
        endpoint.consumerRoles.size() || endpoint.tensorId.empty() ||
      !isSha256Digest(endpoint.tensorDigest) ||
      !isSha256Digest(endpoint.layoutDigest) ||
      !isSha256Digest(endpoint.targetLayoutDigest) ||
      endpoint.segmentCount == 0 ||
      !isSha256Digest(endpoint.manifestDigest) ||
      endpoint.securityProfile.empty() || endpoint.noProgressDeadlineMs == 0 ||
      endpoint.hardDeadlineMs == 0 ||
      !isSha256Digest(endpoint.endpointDigest)) {
    throw std::invalid_argument("invalid V3 tensor endpoint");
  }
  return endpoint;
}

std::vector<NativeTensorEndpointV3>
tensorEndpointArrayFromV3Json(const boost::property_tree::ptree& node,
                              const std::string& key)
{
  std::vector<NativeTensorEndpointV3> endpoints;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return endpoints;
  }
  for (const auto& item : *child) {
    endpoints.push_back(tensorEndpointFromV3Json(item.second));
  }
  return endpoints;
}

NativeReadinessPredicateV3
readinessFromV3Json(const boost::property_tree::ptree& node)
{
  NativeReadinessPredicateV3 readiness;
  readiness.mode = node.get<std::string>("mode", "");
  readiness.endpointDigests = stringArrayFromJson(node, "endpoint_digests");
  readiness.quorum = node.get<std::size_t>("quorum", 0);
  const std::set<std::string> unique(
    readiness.endpointDigests.begin(), readiness.endpointDigests.end());
  if ((readiness.mode != "ALL" && readiness.mode != "ANY" &&
       readiness.mode != "QUORUM") || readiness.endpointDigests.empty() ||
      unique.size() != readiness.endpointDigests.size() ||
      std::any_of(readiness.endpointDigests.begin(),
                  readiness.endpointDigests.end(),
                  [] (const std::string& value) {
                    return !isSha256Digest(value);
                  }) ||
      (readiness.mode == "QUORUM" &&
       (readiness.quorum == 0 ||
        readiness.quorum > readiness.endpointDigests.size())) ||
      (readiness.mode != "QUORUM" && readiness.quorum != 0)) {
    throw std::invalid_argument("invalid V3 readiness predicate");
  }
  return readiness;
}

NativeRoleDataflowContractV3
dataflowFromV3Json(const boost::property_tree::ptree& node)
{
  NativeRoleDataflowContractV3 dataflow;
  dataflow.requestId = node.get<std::string>("request_id", "");
  dataflow.attempt = node.get<std::uint64_t>("attempt", 0);
  dataflow.planDigest = node.get<std::string>("plan_digest", "");
  dataflow.role = node.get<std::string>("role", "");
  dataflow.mayPublish = tensorEndpointArrayFromV3Json(node, "may_publish");
  dataflow.mustFetch = tensorEndpointArrayFromV3Json(node, "must_fetch");
  if (const auto waitFor = node.get_child_optional("wait_for")) {
    for (const auto& item : *waitFor) {
      dataflow.waitFor.push_back(readinessFromV3Json(item.second));
    }
  }
  dataflow.terminalResponseOwner =
    node.get<bool>("terminal_response_owner", false);
  dataflow.dataflowDigest = node.get<std::string>("dataflow_digest", "");
  if (dataflow.requestId.empty() || dataflow.attempt == 0 ||
      !isSha256Digest(dataflow.planDigest) || dataflow.role.empty() ||
      !isSha256Digest(dataflow.dataflowDigest)) {
    throw std::invalid_argument("invalid V3 role dataflow contract");
  }

  std::set<std::string> endpointDigests;
  for (const auto& endpoint : dataflow.mayPublish) {
    if (!endpointDigests.insert(endpoint.endpointDigest).second ||
        endpoint.requestId != dataflow.requestId ||
        endpoint.attempt != dataflow.attempt ||
        endpoint.planDigest != dataflow.planDigest ||
        endpoint.producerRole != dataflow.role) {
      throw std::invalid_argument("invalid V3 mayPublish endpoint binding");
    }
  }
  std::set<std::string> mustFetchDigests;
  for (const auto& endpoint : dataflow.mustFetch) {
    if (!endpointDigests.insert(endpoint.endpointDigest).second ||
        !mustFetchDigests.insert(endpoint.endpointDigest).second ||
        endpoint.requestId != dataflow.requestId ||
        endpoint.attempt != dataflow.attempt ||
        endpoint.planDigest != dataflow.planDigest ||
        endpoint.consumerRole != dataflow.role) {
      throw std::invalid_argument("invalid V3 mustFetch endpoint binding");
    }
  }
  for (const auto& readiness : dataflow.waitFor) {
    for (const auto& endpointDigest : readiness.endpointDigests) {
      if (mustFetchDigests.count(endpointDigest) == 0) {
        throw std::invalid_argument(
          "V3 waitFor references an undeclared mustFetch endpoint");
      }
    }
  }
  return dataflow;
}

NativeDeviceBindingV3
deviceBindingFromV3Json(const boost::property_tree::ptree& node)
{
  NativeDeviceBindingV3 binding;
  binding.mode = node.get<std::string>("mode", "");
  binding.provider = node.get<std::string>("provider", "");
  binding.role = node.get<std::string>("role", "");
  binding.offerDigest = node.get<std::string>("offer_digest", "");
  binding.topologyProfileDigest =
    node.get<std::string>("topology_profile_digest", "");
  binding.resourceSnapshotDigest =
    node.get<std::string>("resource_snapshot_digest", "");
  binding.resourceSequence = node.get<std::uint64_t>("resource_sequence", 0);
  binding.offerScopedDeviceHandle =
    node.get<std::string>("offer_scoped_device_handle", "");
  binding.sharingPolicy = node.get<std::string>("sharing_policy", "");
  if ((binding.mode != "CPU" && binding.mode != "SINGLE_DEVICE") ||
      binding.provider.empty() || binding.role.empty() ||
      !isSha256Digest(binding.offerDigest) ||
      !isSha256Digest(binding.topologyProfileDigest) ||
      !isSha256Digest(binding.resourceSnapshotDigest) ||
      binding.resourceSequence == 0 || binding.sharingPolicy.empty() ||
      (binding.mode == "CPU" && !binding.offerScopedDeviceHandle.empty()) ||
      (binding.mode == "SINGLE_DEVICE" &&
       binding.offerScopedDeviceHandle.empty())) {
    throw std::invalid_argument("invalid V3 device binding");
  }
  return binding;
}

bool
sameTensorEndpoint(const NativeTensorEndpointV3& left,
                   const NativeTensorEndpointV3& right)
{
  return left.producerNamespace == right.producerNamespace &&
         left.requester == right.requester && left.requestId == right.requestId &&
         left.attempt == right.attempt && left.planDigest == right.planDigest &&
         left.groupId == right.groupId && left.groupEpoch == right.groupEpoch &&
         left.operation == right.operation && left.round == right.round &&
         left.sourceKind == right.sourceKind &&
         left.producerRole == right.producerRole &&
         left.producerRank == right.producerRank &&
         left.consumerRoles == right.consumerRoles &&
         left.tensorId == right.tensorId &&
         left.tensorDigest == right.tensorDigest &&
         left.layoutDigest == right.layoutDigest &&
         left.targetLayoutDigest == right.targetLayoutDigest &&
         left.microbatch == right.microbatch &&
         left.segmentCount == right.segmentCount &&
         left.manifestDigest == right.manifestDigest &&
         left.securityProfile == right.securityProfile &&
         left.noProgressDeadlineMs == right.noProgressDeadlineMs &&
         left.hardDeadlineMs == right.hardDeadlineMs &&
         left.endpointDigest == right.endpointDigest;
}

RedistributionSpec
redistributionFromV3Json(const boost::property_tree::ptree& node)
{
  RedistributionSpec spec;
  spec.producerRanks = uintArrayFromJson(node, "producerRanks");
  spec.consumerRanks = uintArrayFromJson(node, "consumerRanks");
  spec.tensor = node.get<std::string>("tensor", "");
  spec.operation = node.get<std::string>("operation", "");
  spec.epoch = node.get<std::string>("epoch", "");
  spec.integrityDigest = node.get<std::string>("integrityDigest", "");
  spec.sourceLayoutDigest = node.get<std::string>("sourceLayoutDigest", "");
  spec.targetLayoutDigest = node.get<std::string>("targetLayoutDigest", "");
  spec.axis = node.get<std::int64_t>("axis", -1);
  spec.temporaryMemoryBytes =
    node.get<std::size_t>("temporaryMemoryBytes", 0);
  spec.completeOutput = node.get<bool>("completeOutput", false);

  const std::set<std::uint64_t> producers(
    spec.producerRanks.begin(), spec.producerRanks.end());
  const std::set<std::uint64_t> consumers(
    spec.consumerRanks.begin(), spec.consumerRanks.end());
  std::vector<std::uint64_t> overlap;
  std::set_intersection(producers.begin(), producers.end(),
                        consumers.begin(), consumers.end(),
                        std::back_inserter(overlap));
  const bool duplicateRank = producers.size() != spec.producerRanks.size() ||
                             consumers.size() != spec.consumerRanks.size();
  const bool validOperationForRankShape =
    (spec.producerRanks.size() == 1 && spec.consumerRanks.size() > 1 &&
     spec.operation == "SCATTER") ||
    (spec.producerRanks.size() > 1 && spec.consumerRanks.size() == 1 &&
     spec.operation == "GATHER") ||
    (spec.producerRanks.size() > 1 && spec.consumerRanks.size() > 1 &&
     spec.operation == "RESHARD") ||
    (spec.producerRanks.size() == 1 && spec.consumerRanks.size() == 1 &&
     spec.sourceLayoutDigest != spec.targetLayoutDigest &&
     spec.operation == "RESHARD");
  if (spec.producerRanks.empty() || spec.consumerRanks.empty() ||
      duplicateRank || !overlap.empty() || spec.tensor.empty() ||
      (spec.operation != "GATHER" && spec.operation != "SCATTER" &&
       spec.operation != "RESHARD") || !validOperationForRankShape ||
      spec.epoch.empty() ||
      !isSha256Digest(spec.integrityDigest) ||
      !isSha256Digest(spec.sourceLayoutDigest) ||
      !isSha256Digest(spec.targetLayoutDigest) || spec.axis < -16 ||
      spec.axis >= 16 || !spec.completeOutput) {
    throw std::invalid_argument("invalid V3 redistribution edge");
  }
  return spec;
}

std::string
redistributionIdentity(const RedistributionSpec& spec)
{
  std::ostringstream key;
  key << spec.tensor << '\n' << spec.operation << '\n' << spec.epoch << '\n'
      << spec.integrityDigest << '\n' << spec.sourceLayoutDigest << '\n'
      << spec.targetLayoutDigest << '\n';
  for (const auto rank : spec.producerRanks) {
    key << 'p' << rank << ',';
  }
  key << '\n';
  for (const auto rank : spec.consumerRanks) {
    key << 'c' << rank << ',';
  }
  return key.str();
}

NativeDependencySpec
dependencyFromV3Json(const boost::property_tree::ptree& dep)
{
  NativeDependencySpec spec;
  spec.producers = stringArrayFromJson(dep, "producers");
  spec.consumers = stringArrayFromJson(dep, "consumers");
  spec.keyScope = dep.get<std::string>("key_scope", "");
  spec.topicPrefix = dep.get<std::string>("topic_prefix", "");
  spec.objectNameTemplate = dep.get<std::string>("object_name_template", "");
  spec.expectedSegments = dep.get<std::size_t>("expected_segments", 0);
  spec.expectedBytes = dep.get<std::size_t>("expected_bytes", 0);
  spec.tensors = stringArrayFromJson(dep, "tensors");
  spec.useNdnsfDataV1 =
    dep.get<std::string>("transportProfile", "COLLAB_LARGE_V1") ==
    "NDNSF_DATA_V1";
  spec.collectiveOperationIndex =
    dep.get<std::uint64_t>("collectiveOperationIndex", 0);
  spec.collectiveProducerRank =
    dep.get<std::string>("collectiveProducerRank", "");
  spec.collectiveSourceLayoutDigest =
    dep.get<std::string>("collectiveSourceLayoutDigest", "");
  spec.collectiveTargetLayoutDigest =
    dep.get<std::string>("collectiveTargetLayoutDigest", "");
  spec.collectiveTensorDigest =
    dep.get<std::string>("collectiveTensorDigest", "");
  if (const auto redistributions = dep.get_child_optional("redistributions")) {
    std::set<std::string> redistributionIdentities;
    for (const auto& item : *redistributions) {
      auto redistribution = redistributionFromV3Json(item.second);
      if (!redistributionIdentities.insert(
            redistributionIdentity(redistribution)).second) {
        throw std::invalid_argument(
          "V3 Selection dependency has duplicate redistribution");
      }
      spec.redistributions.push_back(std::move(redistribution));
    }
  }
  if (spec.producers.empty() || spec.consumers.empty() ||
      spec.keyScope.empty() || spec.topicPrefix.empty() ||
      spec.objectNameTemplate.empty()) {
    throw std::invalid_argument("V3 Selection dependency is incomplete");
  }
  if (!spec.redistributions.empty() && !spec.useNdnsfDataV1) {
    throw std::invalid_argument(
      "V3 redistribution requires NDNSF_DATA_V1 transport");
  }
  for (const auto& redistribution : spec.redistributions) {
    const bool tensorDeclared = std::find(
      spec.tensors.begin(), spec.tensors.end(), redistribution.tensor) !=
      spec.tensors.end();
    const bool sourceLayoutBound =
      spec.collectiveSourceLayoutDigest.empty() ||
      redistribution.sourceLayoutDigest == spec.collectiveSourceLayoutDigest;
    const bool targetLayoutBound =
      spec.collectiveTargetLayoutDigest.empty() ||
      redistribution.targetLayoutDigest == spec.collectiveTargetLayoutDigest;
    if (redistribution.producerRanks.size() != spec.producers.size() ||
        redistribution.consumerRanks.size() != spec.consumers.size() ||
        !tensorDeclared || !sourceLayoutBound || !targetLayoutBound) {
      throw std::invalid_argument(
        "V3 redistribution does not bind the dependency rank/layout contract");
    }
  }
  const bool degreeChanges = spec.producers.size() != spec.consumers.size();
  const bool layoutChanges =
    !spec.collectiveSourceLayoutDigest.empty() &&
    !spec.collectiveTargetLayoutDigest.empty() &&
    spec.collectiveSourceLayoutDigest != spec.collectiveTargetLayoutDigest;
  if ((degreeChanges || layoutChanges) && spec.redistributions.empty()) {
    throw std::invalid_argument(
      "V3 Selection dependency requires explicit redistribution");
  }
  return spec;
}

SegmentNamingSpec
segmentNamingFromJson(const boost::property_tree::ptree& dependency,
                      std::size_t expectedSegments)
{
  SegmentNamingSpec spec;
  spec.staticSegmentCount = expectedSegments;
  spec.dynamicFallback = expectedSegments == 0;

  const auto segmentNaming = dependency.get_child_optional("segmentNaming");
  if (!segmentNaming) {
    return spec;
  }

  spec.mode = segmentNaming->get<std::string>("mode", spec.mode);
  spec.staticSegmentCount =
    segmentNaming->get<std::size_t>("staticSegmentCount", spec.staticSegmentCount);
  spec.dynamicFallback =
    segmentNaming->get<bool>("dynamicFallback", spec.dynamicFallback);
  if (spec.staticSegmentCount == 0) {
    spec.dynamicFallback = true;
  }
  return spec;
}

} // namespace

std::map<std::string, NativeExecutionPlan>
nativeExecutionPlansByServiceFromJson(std::istream& input)
{
  boost::property_tree::ptree root;
  boost::property_tree::read_json(input, root);
  const auto version = root.get<int>("version", 0);
  if (version != 1 && version != 2) {
    throw std::invalid_argument("unsupported native execution plan version");
  }

  std::map<std::string, NativeExecutionPlan> plans;
  const auto services = root.get_child_optional("services");
  if (!services) {
    throw std::invalid_argument("native execution plan missing services");
  }

  for (const auto& serviceNode : services.get()) {
    const auto& service = serviceNode.second;
    const auto serviceName = service.get<std::string>("service", "");
    if (serviceName.empty()) {
      throw std::invalid_argument("native execution plan service missing name");
    }

    NativeExecutionPlan plan;
    plan.version = version;
    plan.serviceName = serviceName;
    plan.modelName = service.get<std::string>("model", "");
    plan.modelFamily = service.get<std::string>("modelFamily", "generic-onnx");
    plan.modelFormat = service.get<std::string>("modelFormat", "unknown");
    plan.plannerKind = service.get<std::string>("plannerKind", "onnx-dag");
    plan.executionPolicy = service.get<std::string>(
      "executionPolicy", "DATA_DRIVEN_V2");
    if (plan.executionPolicy != "DATA_DRIVEN_V2" &&
        plan.executionPolicy != "LEGACY_READY_SET_V1") {
      throw std::invalid_argument(
        "unsupported native execution plan executionPolicy");
    }
    plan.roles = stringArrayFromJson(service, "roles");
    const auto dependencies = service.get_child_optional("dependencies");
    if (dependencies) {
      for (const auto& depNode : dependencies.get()) {
        const auto& dep = depNode.second;
        NativeDependencySpec spec;
        spec.producers = stringArrayFromJson(dep, "producers");
        spec.consumers = stringArrayFromJson(dep, "consumers");
        spec.keyScope = dep.get<std::string>("keyScope", "");
        spec.topicPrefix = dep.get<std::string>("topicPrefix", "");
        spec.objectNameTemplate = dep.get<std::string>("objectNameTemplate", "");
        spec.expectedSegments = dep.get<std::size_t>("expectedSegments", 0);
        spec.expectedBytes = dep.get<std::size_t>("expectedBytes", 0);
        spec.useNdnsfDataV1 =
          dep.get<std::string>("transportProfile", "COLLAB_LARGE_V1") ==
          "NDNSF_DATA_V1";
        spec.collectiveOperationIndex =
          dep.get<std::uint64_t>("collectiveOperationIndex", 0);
        spec.collectiveProducerRank =
          dep.get<std::string>("collectiveProducerRank", "");
        spec.collectiveSourceLayoutDigest =
          dep.get<std::string>("collectiveSourceLayoutDigest", "");
        spec.collectiveTargetLayoutDigest =
          dep.get<std::string>("collectiveTargetLayoutDigest", "");
        spec.collectiveTensorDigest =
          dep.get<std::string>("collectiveTensorDigest", "");
        if (const auto redistributions = dep.get_child_optional("redistributions")) {
          for (const auto& item : *redistributions) {
            spec.redistributions.push_back(redistributionFromV3Json(item.second));
          }
        }
        if (!spec.redistributions.empty() && !spec.useNdnsfDataV1) {
          throw std::invalid_argument(
            "native execution plan redistribution requires NDNSF_DATA_V1");
        }
        spec.tensors = stringArrayFromJson(dep, "tensors");
        spec.segmentNaming = segmentNamingFromJson(dep, spec.expectedSegments);
        if (spec.keyScope.empty()) {
          throw std::invalid_argument(
            "native execution plan dependency missing keyScope");
        }
        plan.dependencies.push_back(std::move(spec));
      }
    }
    plans.emplace(serviceName, std::move(plan));
  }
  return plans;
}

NativeExecutionPlan
nativeExecutionPlanForServiceFromJson(std::istream& input, const std::string& serviceName)
{
  auto plans = nativeExecutionPlansByServiceFromJson(input);
  const auto found = plans.find(serviceName);
  if (found == plans.end()) {
    throw std::out_of_range("native execution plan has no service: " + serviceName);
  }
  return found->second;
}

NativeSelectionProjectionV3
nativeSelectionProjectionV3FromJson(std::istream& input,
                                    const std::string& selectedRole)
{
  boost::property_tree::ptree root;
  boost::property_tree::read_json(input, root);
  if (root.get<std::string>("schema", "") != "ndnsf-di-selection-v3" ||
      root.get<int>("schema_version", 0) != 3) {
    throw std::invalid_argument("V3 Selection projection schema mismatch");
  }

  NativeSelectionProjectionV3 projection;
  projection.provider = root.get<std::string>("provider", "");
  projection.requestId = root.get<std::string>("request_id", "");
  projection.attempt = root.get<std::uint64_t>("attempt", 0);
  projection.planCoreDigest = root.get<std::string>("plan_core_digest", "");
  projection.planDigest = root.get<std::string>("plan_digest", "");
  projection.ackClosedDigest =
    root.get<std::string>("ack_closed_digest", "");
  projection.offerDigest = root.get<std::string>("offer_digest", "");
  projection.securityPolicySnapshotDigest =
    root.get<std::string>("security_policy_snapshot_digest", "");
  projection.deadlineMs = root.get<std::uint64_t>("deadline_ms", 0);
  projection.groupCapabilityV1 =
    root.get<std::string>("group_capability_v1", "");
  if (projection.provider.empty() || projection.requestId.empty() ||
      projection.attempt == 0 || projection.deadlineMs == 0 ||
      !isSha256Digest(projection.planCoreDigest) ||
      !isSha256Digest(projection.planDigest) ||
      !isSha256Digest(projection.ackClosedDigest) ||
      !isSha256Digest(projection.offerDigest) ||
      !isSha256Digest(projection.securityPolicySnapshotDigest)) {
    throw std::invalid_argument("V3 Selection projection binding is incomplete");
  }

  projection.plan.version = 3;
  projection.plan.executionPolicy = "DATA_DRIVEN_V2";
  std::set<std::string> roleSet;
  const auto appendRole = [&] (const std::string& role) {
    if (!role.empty() && roleSet.insert(role).second) {
      projection.plan.roles.push_back(role);
    }
  };

  if (const auto dependencies = root.get_child_optional("dependencies")) {
    for (const auto& item : *dependencies) {
      auto dependency = dependencyFromV3Json(item.second);
      for (const auto& role : dependency.producers) {
        appendRole(role);
      }
      for (const auto& role : dependency.consumers) {
        appendRole(role);
      }
      projection.plan.dependencies.push_back(std::move(dependency));
    }
  }

  const auto roles = root.get_child_optional("roles");
  if (!roles || roles->size() != 1) {
    throw std::invalid_argument(
      "V3 Selection projection must contain exactly one local role");
  }
  const auto& roleNode = roles->front().second;
  const auto logicalRole = roleNode.get<std::string>("role", "");
  const auto rank = roleNode.get<std::uint64_t>("rank", 0);
  const auto rankedRole = logicalRole + "#" + std::to_string(rank);
  const auto planRole = roleSet.count(logicalRole) != 0
    ? logicalRole
    : roleSet.count(rankedRole) != 0
      ? rankedRole
      : rank == 0 ? logicalRole : rankedRole;
  if (!selectedRole.empty() && selectedRole != logicalRole &&
      selectedRole != planRole) {
    throw std::invalid_argument(
      "V3 Selection projection does not contain the requested role");
  }
  projection.selectedRole = selectionRoleFromV3Json(roleNode, planRole);
  appendRole(planRole);

  const auto executionRole = root.get_child_optional("execution_role");
  const auto assembly = root.get_child_optional("assembly");
  const auto dataflow = root.get_child_optional("dataflow");
  const auto deviceBinding = root.get_child_optional("device_binding");
  if (!executionRole || !assembly || !dataflow || !deviceBinding) {
    throw std::invalid_argument(
      "V3 Selection projection is missing its complete role contract");
  }
  projection.executionRole = executionRoleFromV3Json(*executionRole);
  projection.assembly = selectionRoleFromV3Json(*assembly, planRole);
  projection.dataflow = dataflowFromV3Json(*dataflow);
  projection.deviceBinding = deviceBindingFromV3Json(*deviceBinding);

  const auto grantBinding = root.get_child_optional("grant_binding");
  const bool isProtected =
    projection.selectedRole.protectionEpoch != "plaintext-v1";
  if (isProtected && !grantBinding) {
    throw std::invalid_argument(
      "protected V3 Selection requires its Provider grant binding");
  }
  if (!isProtected && grantBinding) {
    throw std::invalid_argument(
      "plaintext V3 Selection must not carry a grant binding");
  }
  if (grantBinding) {
    projection.hasGrantBinding = true;
    projection.grantName = grantBinding->get<std::string>("grant_name", "");
    projection.grantDigest = grantBinding->get<std::string>("grant_digest", "");
    const auto grantProvider =
      grantBinding->get<std::string>("provider", "");
    const auto grantRequestId =
      grantBinding->get<std::string>("request_id", "");
    const auto grantAttempt =
      grantBinding->get<std::uint64_t>("attempt", 0);
    const auto grantPlanCoreDigest =
      grantBinding->get<std::string>("plan_core_digest", "");
    const auto grantPolicyDigest = grantBinding->get<std::string>(
      "security_policy_snapshot_digest", "");
    const auto grantProtectionEpoch =
      grantBinding->get<std::string>("protection_epoch", "");
    if (projection.grantName.empty() || projection.grantName.front() != '/' ||
        !isSha256Digest(projection.grantDigest) ||
        grantProvider != projection.provider ||
        grantRequestId != projection.requestId ||
        grantAttempt != projection.attempt ||
        grantPlanCoreDigest != projection.planCoreDigest ||
        grantPolicyDigest != projection.securityPolicySnapshotDigest ||
        grantProtectionEpoch != projection.selectedRole.protectionEpoch) {
      throw std::invalid_argument(
        "V3 Selection Provider grant binding mismatch");
    }
  }

  if (!sameAssembly(projection.selectedRole, projection.assembly) ||
      projection.executionRole.roleId != planRole ||
      projection.executionRole.stageId != projection.selectedRole.role ||
      projection.executionRole.rank != projection.selectedRole.rank ||
      projection.executionRole.layerBegin != projection.selectedRole.layerBegin ||
      projection.executionRole.layerEnd != projection.selectedRole.layerEnd ||
      projection.executionRole.backend != projection.selectedRole.backend ||
      projection.executionRole.adapterId != projection.selectedRole.adapterId ||
      projection.executionRole.adapterVersion !=
        projection.selectedRole.adapterVersion ||
      projection.dataflow.requestId != projection.requestId ||
      projection.dataflow.attempt != projection.attempt ||
      projection.dataflow.planDigest != projection.planDigest ||
      projection.dataflow.role != planRole ||
      projection.deviceBinding.provider != projection.provider ||
      projection.deviceBinding.role != planRole ||
      projection.deviceBinding.offerDigest != projection.offerDigest ||
      (isCpuBackend(projection.selectedRole.backend) &&
       projection.deviceBinding.mode != "CPU") ||
      (!isCpuBackend(projection.selectedRole.backend) &&
       (projection.deviceBinding.mode != "SINGLE_DEVICE" ||
        projection.selectedRole.deviceSet.front() !=
          projection.deviceBinding.offerScopedDeviceHandle))) {
    throw std::invalid_argument(
      "V3 Selection role/assembly/dataflow/device binding mismatch");
  }
  return projection;
}

void
validateNativeSelectionProjectionSetV3(
  const std::vector<NativeSelectionProjectionV3>& projections)
{
  if (projections.empty()) {
    throw std::invalid_argument("V3 projection set is empty");
  }
  const auto& first = projections.front();
  std::set<std::string> providers;
  std::set<std::string> roles;
  std::map<std::string, std::vector<const NativeTensorEndpointV3*>> publishers;
  std::map<std::string, std::set<std::string>> edges;
  std::map<std::string, std::size_t> incoming;
  std::size_t terminalOwners = 0;

  for (const auto& projection : projections) {
    const auto& role = projection.executionRole.roleId;
    if (projection.requestId != first.requestId ||
        projection.attempt != first.attempt ||
        projection.planDigest != first.planDigest ||
        !providers.insert(projection.provider).second ||
        !roles.insert(role).second ||
        projection.selectedRole.selectedRole != role ||
        projection.dataflow.role != role) {
      throw std::invalid_argument(
        "V3 projection set violates one-role/one-Provider ownership");
    }
    incoming.emplace(role, 0);
    terminalOwners += projection.dataflow.terminalResponseOwner ? 1 : 0;
    for (const auto& endpoint : projection.dataflow.mayPublish) {
      publishers[endpoint.endpointDigest].push_back(&endpoint);
    }
  }
  if (terminalOwners != 1) {
    throw std::invalid_argument(
      "V3 projection set must have exactly one terminal Response owner");
  }

  for (const auto& projection : projections) {
    const auto& consumer = projection.executionRole.roleId;
    for (const auto& endpoint : projection.dataflow.mustFetch) {
      if (endpoint.sourceKind == "APPLICATION_INPUT") {
        continue;
      }
      const auto found = publishers.find(endpoint.endpointDigest);
      if (found == publishers.end() || found->second.size() != 1 ||
          endpoint.producerRole.empty() ||
          roles.count(endpoint.producerRole) == 0 ||
          !sameTensorEndpoint(endpoint, *found->second.front())) {
        throw std::invalid_argument(
          "V3 mustFetch has no exact single mayPublish owner");
      }
      if (edges[endpoint.producerRole].insert(consumer).second) {
        ++incoming[consumer];
      }
    }
  }

  std::vector<std::string> ready;
  for (const auto& item : incoming) {
    if (item.second == 0) {
      ready.push_back(item.first);
    }
  }
  std::sort(ready.begin(), ready.end());
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto role = ready.front();
    ready.erase(ready.begin());
    ++visited;
    for (const auto& consumer : edges[role]) {
      auto& degree = incoming[consumer];
      if (--degree == 0) {
        ready.push_back(consumer);
        std::sort(ready.begin(), ready.end());
      }
    }
  }
  if (visited != roles.size()) {
    throw std::invalid_argument("V3 role dataflow contains a cycle");
  }
}

RoleSpec
roleSpecFromSelectionProjectionV3(
  const NativeSelectionProjectionV3& projection,
  const std::string& localProvider)
{
  const auto& dataflow = projection.dataflow;
  if (dataflow.role.empty() ||
      dataflow.role != projection.executionRole.roleId ||
      dataflow.requestId != projection.requestId ||
      dataflow.attempt != projection.attempt ||
      dataflow.planDigest != projection.planDigest ||
      (!localProvider.empty() && projection.provider != localProvider)) {
    throw std::invalid_argument(
      "V3 Selection dataflow cannot be projected for this Provider");
  }

  auto makeEdge = [&projection, &dataflow, &localProvider](
                    const NativeTensorEndpointV3& endpoint,
                    bool output) {
    if (endpoint.requestId != dataflow.requestId ||
        endpoint.attempt != dataflow.attempt ||
        endpoint.planDigest != dataflow.planDigest ||
        endpoint.securityProfile != "NDNSF_DATA_V1" ||
        (output && endpoint.producerRole != dataflow.role) ||
        (!output && endpoint.consumerRole != dataflow.role)) {
      throw std::invalid_argument(
        "V3 tensor endpoint is outside the local role authority");
    }
    DependencyEdge edge;
    // groupId is the runtime dependency scope. tensorId remains the adapter
    // tensor identity and is carried separately in `tensors`.
    edge.scope = endpoint.groupId;
    edge.producerRole = endpoint.producerRole;
    edge.consumerRole = endpoint.consumerRole;
    edge.consumerRoles = endpoint.consumerRoles;
    edge.plannedDataName = tensorObjectNamePrefix(endpoint);
    edge.expectedSegments = 0;
    edge.expectedBytes = 0;
    edge.tensors = {endpoint.tensorId};
    edge.requestId = projection.requestId;
    edge.attemptEpoch = projection.attempt;
    edge.useNdnsfDataV1 = true;
    edge.collectiveOperationIndex = endpoint.round;
    edge.collectiveProducerRank = std::to_string(endpoint.producerRank);
    edge.collectiveSourceLayoutDigest = endpoint.layoutDigest;
    edge.collectiveTargetLayoutDigest = endpoint.targetLayoutDigest;
    edge.collectiveTensorDigest = endpoint.tensorDigest;
    edge.transportScope = endpoint.groupId;
    edge.producerProvider = output ?
      (localProvider.empty() ? projection.provider : localProvider) :
      endpoint.producerNamespace;
    edge.topicPrefix = "/tensor";
    edge.declaredByV3 = true;
    edge.manifestDataName = tensorObjectManifestName(endpoint);
    edge.maxSegments = endpoint.segmentCount;
    edge.endpointDigest = endpoint.endpointDigest;
    edge.planDigest = endpoint.planDigest;
    edge.manifestContractDigest = endpoint.manifestDigest;
    edge.tensorDigest = endpoint.tensorDigest;
    edge.layoutDigest = endpoint.layoutDigest;
    edge.securityProfile = endpoint.securityProfile;
    edge.operationKind = endpoint.operation;
    edge.round = endpoint.round;
    edge.microbatch = endpoint.microbatch;
    edge.noProgressDeadlineMs = endpoint.noProgressDeadlineMs;
    edge.hardDeadlineMs = endpoint.hardDeadlineMs;
    return edge;
  };

  RoleSpec spec;
  spec.role = dataflow.role;
  spec.requestId = projection.requestId;
  spec.attemptEpoch = projection.attempt;
  spec.outputs.reserve(dataflow.mayPublish.size());
  for (const auto& endpoint : dataflow.mayPublish) {
    spec.outputs.push_back(makeEdge(endpoint, true));
  }
  spec.inputs.reserve(dataflow.mustFetch.size());
  for (const auto& endpoint : dataflow.mustFetch) {
    spec.inputs.push_back(makeEdge(endpoint, false));
  }
  return spec;
}

} // namespace ndnsf::di
