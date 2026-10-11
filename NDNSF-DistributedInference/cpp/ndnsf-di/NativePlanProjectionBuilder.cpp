#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanProjectionBuilder.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePhaseTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include <ndn-cxx/encoding/tlv.hpp>
#include <algorithm>
#include <cctype>
#include <set>
#include <limits>
#include <tuple>

namespace ndnsf::di {
class NativeStaticProjectionLayout final
{
  friend class NativePlanProjectionBuilder;
  using TensorGeometry = std::tuple<std::string, std::string,
    decltype(NativeAssemblyTensorContractV3{}.shape)>;
  using RoleGeometry = std::tuple<std::string, std::uint64_t,
    std::vector<TensorGeometry>, std::vector<TensorGeometry>>;
  using TransferGeometry = std::tuple<std::string, std::string,
    std::vector<std::uint64_t>, std::vector<std::uint64_t>,
    std::string, std::string, std::string>;
  using DependencyGeometry = std::tuple<std::vector<std::string>, std::vector<std::string>,
    std::vector<std::string>, std::string, std::string, std::vector<TransferGeometry>>;
  using Identity = std::tuple<std::string, std::string, std::string,
    std::map<std::string, std::string>, std::map<std::string, RoleGeometry>,
    std::vector<DependencyGeometry>, std::vector<std::string>, std::vector<std::string>>;
  bool matches(const NativePlacementPlanCore& core) const
  {
    if (std::get<0>(identity) != core.candidateDigest || std::get<1>(identity) != core.modelDigest ||
        std::get<2>(identity) != core.graphDigest || std::get<3>(identity) != core.assignment.providerByRole ||
        std::get<6>(identity) != core.generationContract.stateInputNames ||
        std::get<7>(identity) != core.generationContract.stateOutputNames)
      return false;
    const auto& roles = std::get<4>(identity);
    if (roles.size() != core.roleContracts().size()) return false;
    const auto sameTensors = [](const auto& cached, const auto& current) {
      if (cached.size() != current.size()) return false;
      for (std::size_t i = 0; i < current.size(); ++i)
        if (cached[i] != std::tie(current[i].name, current[i].dtype, current[i].shape)) return false;
      return true;
    };
    for (const auto& role : core.roleContracts()) {
      const auto found = roles.find(role.first);
      if (found == roles.end()) return false;
      const auto& cached = found->second;
      if (std::get<0>(cached) != role.second.role || std::get<1>(cached) != role.second.rank ||
          !sameTensors(std::get<2>(cached), role.second.expectedInputs) ||
          !sameTensors(std::get<3>(cached), role.second.expectedOutputs)) return false;
    }
    const auto& dependencies = std::get<5>(identity);
    if (dependencies.size() != core.executionPlan.dependencies.size()) return false;
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
      const auto& cached = dependencies[i];
      const auto& current = core.executionPlan.dependencies[i];
      if (std::get<4>(cached) != current.operationKind) return false;
      if (current.operationKind == "TOKEN_FEEDBACK") continue;
      if (std::get<0>(cached) != current.producers || std::get<1>(cached) != current.consumers ||
          std::get<2>(cached) != current.tensors || std::get<3>(cached) != current.keyScope) return false;
      const auto& transfers = std::get<5>(cached);
      if (transfers.size() != current.redistributions.size()) return false;
      for (std::size_t j = 0; j < transfers.size(); ++j) {
        const auto& transfer = current.redistributions[j];
        if (transfers[j] != std::tie(transfer.tensor, transfer.operation, transfer.producerRanks,
            transfer.consumerRanks, transfer.integrityDigest, transfer.sourceLayoutDigest,
            transfer.targetLayoutDigest)) return false;
      }
    }
    return true;
  }
  static Identity identityFrom(const NativePlacementPlanCore& core)
  {
    std::map<std::string, RoleGeometry> roles;
    const auto tensors = [](const auto& contracts) {
      std::vector<TensorGeometry> result;
      result.reserve(contracts.size());
      for (const auto& tensor : contracts) result.emplace_back(tensor.name, tensor.dtype, tensor.shape);
      return result;
    };
    for (const auto& role : core.roleContracts())
      roles.emplace(role.first, RoleGeometry{role.second.role, role.second.rank,
        tensors(role.second.expectedInputs), tensors(role.second.expectedOutputs)});
    std::vector<DependencyGeometry> dependencies;
    dependencies.reserve(core.executionPlan.dependencies.size());
    for (const auto& dependency : core.executionPlan.dependencies) {
      if (dependency.operationKind == "TOKEN_FEEDBACK") {
        // Preserve indexes, but feedback is authorized/expanded by the group
        // builder and is never a base readiness endpoint. Its scope is fresh
        // per request and cannot invalidate the reusable activation layout.
        dependencies.emplace_back(std::vector<std::string>{}, std::vector<std::string>{},
          std::vector<std::string>{}, "", "TOKEN_FEEDBACK", std::vector<TransferGeometry>{});
        continue;
      }
      std::vector<TransferGeometry> transfers;
      for (const auto& transfer : dependency.redistributions)
        transfers.emplace_back(transfer.tensor, transfer.operation, transfer.producerRanks,
          transfer.consumerRanks, transfer.integrityDigest, transfer.sourceLayoutDigest,
          transfer.targetLayoutDigest);
      // Request topic/name and size forecasts are refreshed outside geometry.
      dependencies.emplace_back(dependency.producers, dependency.consumers, dependency.tensors,
        dependency.keyScope, dependency.operationKind, std::move(transfers));
    }
    return {core.candidateDigest, core.modelDigest, core.graphDigest,
      core.assignment.providerByRole, std::move(roles), std::move(dependencies),
      core.generationContract.stateInputNames, core.generationContract.stateOutputNames};
  }
  Identity identity;
  std::map<std::size_t, std::vector<NativeTensorEndpointV3>> endpoints;
};
namespace {
NativeTensorEndpointV3 staticEndpoint(const NativeTensorEndpointV3& source)
{
  NativeTensorEndpointV3 e;
  e.operation = source.operation; e.sourceKind = source.sourceKind;
  e.producerRole = source.producerRole; e.producerRank = source.producerRank;
  e.consumerRoles = source.consumerRoles; e.tensorId = source.tensorId;
  e.tensorDigest = source.tensorDigest; e.bundleTensorNames = source.bundleTensorNames;
  e.layoutDigest = source.layoutDigest; e.targetLayoutDigest = source.targetLayoutDigest;
  e.microbatch = source.microbatch;
  return e;
}
std::uint64_t fixedBundleBytes(const NativeSelectionRoleV3& producer,
                              const std::vector<std::string>& names)
{
  try {
    if (names.empty()) return 0;
    std::vector<TensorBundleEncodingMember> members;
    members.reserve(names.size());
    for (const auto& name : names) {
      const NativeAssemblyTensorContractV3* found = nullptr;
      const auto match = [&](const auto& contracts) {
        for (const auto& contract : contracts) {
          if (contract.name != name) continue;
          if (found && (found->dtype != contract.dtype || found->shape != contract.shape))
            throw std::invalid_argument("ambiguous tensor prefetch shape");
          found = &contract;
        }
      };
      match(producer.expectedOutputs);
      match(producer.expectedInputs);
      if (!found) return 0;
      std::uint64_t bytes = found->dtype == "float32" ? 4 :
        found->dtype == "float16" ? 2 : found->dtype == "int64" ? 8 :
        (found->dtype == "bool" || found->dtype == "uint8") ? 1 : 0;
      if (bytes == 0) return 0;
      for (const auto& dimension : found->shape) {
        if (!std::holds_alternative<std::int64_t>(dimension)) return 0;
        const auto value = std::get<std::int64_t>(dimension);
        if (value < 0 || (bytes != 0 && static_cast<std::uint64_t>(value) >
            std::numeric_limits<std::uint64_t>::max() / bytes)) return 0;
        bytes *= static_cast<std::uint64_t>(value);
      }
      members.push_back({name.size(), found->shape.size(), bytes});
    }
    return tensorBundleEncodedSize(members);
  }
  catch (const std::exception&) { return 0; }
}
std::string hash(const NativeJson& value) { return nativePlanningDigest(nativeCanonicalJson(value)); }
bool digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool cpu(std::string value)
{
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return false;
  value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value == "cpu" || (value.size() >= 4 && value.substr(value.size() - 4) == "-cpu");
}
void certifyEndpoint(NativeTensorEndpointV3& endpoint)
{
  if (endpoint.sourceKind == "ROLE")
    endpoint.manifestDigest = hash(NativeJson{{"requestId", endpoint.requestId}, {"attempt", endpoint.attempt},
      {"planDigest", endpoint.planDigest}, {"group", endpoint.groupId}, {"epoch", endpoint.groupEpoch},
      {"operation", endpoint.operation}, {"round", endpoint.round}, {"producer", endpoint.producerRole},
      {"consumers", endpoint.consumerRoles}, {"tensor", endpoint.tensorId},
      {"bundleTensorNames", endpoint.bundleTensorNames},
      {"tensorDigest", endpoint.tensorDigest}});
  auto value = nativeEndpointJson(endpoint);
  value.erase("endpoint_digest"); value.erase("consumer_role");
  endpoint.endpointDigest = hash(value);
}
}

std::map<std::string, NativeRoleProjectionInputs> NativePlanProjectionBuilder::build(
  const NativeSealedPlan& sealed, const NativeSplitCandidate& candidate,
  const std::vector<NativeAdmittedOfferV3>& offers, const NativeProjectionContext& context)
{
  sealed.validate();
  return buildFromValidatedSealed(sealed, candidate, offers, context);
}

std::map<std::string, NativeRoleProjectionInputs> NativePlanProjectionBuilder::build(
  const NativeOwnedSealedPlan& sealed, const NativeSplitCandidate& candidate,
  const std::vector<NativeAdmittedOfferV3>& offers, const NativeProjectionContext& context)
{
  return buildFromValidatedSealed(sealed.value(), candidate, offers, context);
}

std::map<std::string, NativeRoleProjectionInputs>
NativePlanProjectionBuilder::buildFromValidatedSealed(
  const NativeSealedPlan& sealed, const NativeSplitCandidate& candidate,
  const std::vector<NativeAdmittedOfferV3>& offers, const NativeProjectionContext& context)
{
  const auto& core = sealed.core;
  if (candidate.computedDigest() != candidate.candidateDigest || candidate.candidateDigest != core.candidateDigest ||
      candidate.model.intentDigest() != core.modelDigest ||
      candidate.model.contentDigest != core.sourceContentDigest || candidate.graphDigest != core.graphDigest ||
      !context.nowMs || context.nowMs >= core.expiresAtMs || !context.noProgressMs || !context.maxSegments)
    throw std::invalid_argument("projection candidate or request bounds differ from sealed plan");
  std::map<std::string, const NativeAdmittedOfferV3*> admitted;
  for (const auto& offer : offers)
    if (!admitted.emplace(offer.observation().provider, &offer).second)
      throw std::invalid_argument("projection contains duplicate admitted providers");
  std::map<std::string, NativeRoleProjectionInputs> result;
  for (const auto& assignment : core.assignment.providerByRole) {
    const auto& role = assignment.first;
    const auto& spec = core.roleContracts().at(role);
    const auto found = admitted.find(assignment.second);
    if (found == admitted.end()) throw std::invalid_argument("projection has no admitted assigned provider");
    // The same Provider may own several roles.  Validate the grant view
    // against this exact role so a valid grant for the first co-located role
    // cannot accidentally stand in for another role's artifact identity.
    NativePlanSealer::grantViewFromValidatedCore(core, *found->second, sealed.security, role);
    const auto& offer = found->second->observation();
    if (offer.capturedAtMs > context.nowMs || offer.expiresAtMs <= context.nowMs)
      throw std::invalid_argument("projection offer is outside the frozen time window");
    auto& value = result[role];
    value.executionRole = {role, spec.role, spec.rank, spec.layerBegin, spec.layerEnd,
      spec.backend, spec.adapterId, spec.adapterVersion};
    value.dataflow.requestId = core.requestId; value.dataflow.attempt = core.attempt;
    value.dataflow.planDigest = sealed.planDigest; value.dataflow.role = role;
    auto resources = NativeJson::array();
    std::uint64_t sequence = 1;
    for (const auto& resource : offer.resources) {
      sequence = std::max(sequence, resource.resourceSequence);
      resources.push_back(NativeJson{{"device", resource.device}, {"total_memory_mb", resource.totalMemoryMb},
        {"free_memory_mb", resource.freeMemoryMb}, {"active_requests", resource.activeRequests},
        {"resource_sequence", resource.resourceSequence}, {"captured_at_ms", resource.capturedAtMs},
        {"topology_digest", resource.topologyDigest}});
    }
    const bool onCpu = cpu(spec.backend);
    if ((onCpu && !spec.deviceSet.empty()) || (!onCpu && (spec.deviceSet.size() != 1 ||
        std::find(offer.devices.begin(), offer.devices.end(), spec.deviceSet.front()) == offer.devices.end())))
      throw std::invalid_argument("projection device differs from admitted topology");
    value.deviceBinding = {onCpu ? "CPU" : "SINGLE_DEVICE", assignment.second, role, offer.offerDigest,
      offer.topologyDigest, hash(resources), sequence, onCpu ? "" : spec.deviceSet.front(), "EXCLUSIVE_ROLE"};
  }
  for (const auto& capability : context.groupCapabilitiesByRole) {
    if (!result.count(capability.first)) throw std::invalid_argument("projection capability has a foreign role");
    result.at(capability.first).groupCapabilityV1 = capability.second;
  }
  std::shared_ptr<const NativeStaticProjectionLayout> cachedLayout;
  std::shared_ptr<NativeStaticProjectionLayout> newLayout;
  const auto identityTiming = detail::nativePhaseStarted();
  if (context.layoutOwner) {
    if (*context.layoutOwner && (*context.layoutOwner)->matches(core))
      cachedLayout = *context.layoutOwner;
    else {
      newLayout = std::make_shared<NativeStaticProjectionLayout>();
      newLayout->identity = NativeStaticProjectionLayout::identityFrom(core);
    }
  }
  detail::logNativeWorkPhase(core.requestId, "projection_layout_identity", identityTiming);
  const auto geometryTiming = detail::nativePhaseStarted();
  const auto selected = [&](const std::string& stage) {
    for (const auto& item : core.roleContracts())
      if (item.second.role == stage && item.second.rank == 0) return item.first;
    throw std::invalid_argument("projection ingress or egress stage has no selected role");
  };
  const auto endpointBase = [&] {
    NativeTensorEndpointV3 e;
    e.requester = core.requesterIdentity;
    e.requestId = core.requestId; e.attempt = core.attempt; e.planDigest = sealed.planDigest;
    e.namingOperationStride = core.generationContract.enabled
      ? core.generationContract.streamingOperationStride : 0;
    e.securityProfile = "NDNSF_DATA_V1"; e.noProgressDeadlineMs = context.noProgressMs;
    e.hardDeadlineMs = core.expiresAtMs - context.nowMs;
    return e;
  };
  if (!candidate.inputIngressRole.empty()) {
    if (!digest(context.logicalInputDigest) || !digest(context.inputLayoutDigest))
      throw std::invalid_argument("projection application input identity is missing");
    auto e = endpointBase(); e.producerNamespace = core.requesterIdentity;
    e.namingOperationStride = 0;
    e.groupId = "application-input"; e.groupEpoch = "attempt-" + std::to_string(core.attempt);
    e.operation = "APPLICATION_INPUT"; e.sourceKind = "APPLICATION_INPUT";
    e.consumerRole = selected(candidate.inputIngressRole); e.consumerRoles = {e.consumerRole};
    e.tensorId = "application-input"; e.tensorDigest = e.manifestDigest = context.logicalInputDigest;
    e.layoutDigest = e.targetLayoutDigest = context.inputLayoutDigest; e.segmentCount = 1;
    certifyEndpoint(e); result.at(e.consumerRole).dataflow.mustFetch.push_back(std::move(e));
  }
  std::set<std::string> outgoing;
  std::set<std::size_t> consumedBindings;
  for (std::size_t index = 0; index < core.executionPlan.dependencies.size(); ++index) {
    const auto& dependency = core.executionPlan.dependencies[index];
    const bool feedback = dependency.operationKind == "TOKEN_FEEDBACK";
    // A same-role feedback loop is authenticated local control; it creates
    // no published Data and stays outside the network endpoint lists.
    if (feedback && dependency.producers.size() == 1 &&
        dependency.producers == dependency.consumers) {
      if (!core.generationContract.enabled || core.executionPlan.roles.size() != 1)
        throw std::invalid_argument("self feedback requires the authenticated single-role local plan");
      continue;
    }
    const auto bound = context.dependencies.find(index);
    if (bound == context.dependencies.end() || bound->second.groupId.empty() || bound->second.groupEpoch.empty() ||
        dependency.producers.empty() || dependency.consumers.empty() || dependency.tensors.empty())
      throw std::invalid_argument("projection dependency group or tensor identity is missing");
    consumedBindings.insert(index);
    const auto& binding = bound->second;
    std::set<std::string> producerProviders;
    for (const auto& role : dependency.producers) producerProviders.insert(core.assignment.providerByRole.at(role));
    if (producerProviders.size() != binding.producerNamespaces.size())
      throw std::invalid_argument("projection namespace binding has a foreign provider cover");
    if (std::set<std::string>(dependency.producers.begin(), dependency.producers.end()).size() != dependency.producers.size() ||
        std::set<std::string>(dependency.consumers.begin(), dependency.consumers.end()).size() != dependency.consumers.size())
      throw std::invalid_argument("projection dependency repeats an owner");
    // Namespaces are current bindings even when geometry is reused.
    for (const auto& producer : dependency.producers) {
      if (!result.count(producer)) throw std::invalid_argument("projection dependency has a foreign producer");
      if (!feedback) outgoing.insert(producer);
      const auto prefix = binding.producerNamespaces.find(core.assignment.providerByRole.at(producer));
      if (prefix == binding.producerNamespaces.end() || prefix->second.empty() || prefix->second.front() != '/')
        throw std::invalid_argument("projection dependency has no bound producer namespace");
      if (ndn::Name(prefix->second) != ndn::Name(core.assignment.providerByRole.at(producer)))
        throw std::invalid_argument("projection tensor namespace differs from its actual Producer");
    }
    const auto appendEndpoint = [&](NativeTensorEndpointV3 e) {
      const auto& provider = core.assignment.providerByRole.at(e.producerRole);
      e.requester = core.requesterIdentity;
      e.namingOperationStride = core.generationContract.enabled ?
        core.generationContract.streamingOperationStride : 0;
      e.requestId = core.requestId; e.attempt = core.attempt; e.planDigest = sealed.planDigest;
      e.securityProfile = "NDNSF_DATA_V1"; e.noProgressDeadlineMs = context.noProgressMs;
      e.hardDeadlineMs = core.expiresAtMs - context.nowMs;
      e.producerNamespace = binding.producerNamespaces.at(provider);
      e.groupId = binding.groupId; e.groupEpoch = binding.groupEpoch; e.round = binding.operationIndex;
      e.segmentCount = context.maxSegments; e.inlineFirstSegment = context.inlineFirstSegment;
      e.prefetchBundleBytes = e.prefetchSegmentBytes = 0;
      const auto chunk = context.tensorSegmentBytesByProvider.find(provider);
      if (!e.bundleTensorNames.empty() && dependency.redistributions.empty() && dependency.producers.size() == 1 &&
          dependency.consumers.size() == 1 &&
          (dependency.operationKind == "ACTIVATION" || dependency.operationKind == "PIPELINE") &&
          chunk != context.tensorSegmentBytesByProvider.end() && chunk->second != 0 &&
          chunk->second <= ndn::MAX_NDN_PACKET_SIZE &&
          std::all_of(e.consumerRoles.begin(), e.consumerRoles.end(), [&](const auto& consumer) {
            const auto declared = context.tensorSegmentBytesByProvider.find(core.assignment.providerByRole.at(consumer));
            return declared != context.tensorSegmentBytesByProvider.end() && declared->second != 0 &&
              declared->second <= ndn::MAX_NDN_PACKET_SIZE;
          })) {
        auto bytes = fixedBundleBytes(core.roleContracts().at(e.producerRole), e.bundleTensorNames);
        if (bytes == 0) {
          const auto estimate = context.tensorPrefetchByDependency.find(index);
          if (estimate != context.tensorPrefetchByDependency.end() && estimate->second.tensorNames == e.bundleTensorNames)
            bytes = estimate->second.bundleBytes;
        }
        if (bytes != 0 && 1 + (bytes - 1) / chunk->second <= e.segmentCount) {
          e.prefetchBundleBytes = bytes; e.prefetchSegmentBytes = chunk->second;
        }
      }
      e.consumerRole = e.consumerRoles.front();
      result.at(e.producerRole).dataflow.mayPublish.push_back(e);
      for (const auto& consumer : e.consumerRoles) {
        if (!result.count(consumer)) throw std::invalid_argument("projection dependency has a foreign consumer");
        e.consumerRole = consumer; result.at(consumer).dataflow.mustFetch.push_back(e);
      }
    };
    if (feedback) {
      if (!core.generationContract.enabled || !dependency.useNdnsfDataV1 ||
          dependency.producers.size() != 1 || dependency.consumers.size() != 1 ||
          dependency.tensors != std::vector<std::string>{"input_ids"} ||
          dependency.collectiveOperationIndex >= core.generationContract.streamingOperationStride ||
          binding.operationIndex != dependency.collectiveOperationIndex ||
          !digest(dependency.collectiveTensorDigest) ||
          !digest(dependency.collectiveSourceLayoutDigest) ||
          !digest(dependency.collectiveTargetLayoutDigest))
        throw std::invalid_argument("projection feedback naming contract is incomplete");
      // Bind every request from its sealed control dependency; activation
      // geometry reuse must not retain another invocation's feedback scope.
      auto e = endpointBase();
      e.operation = dependency.operationKind;
      e.sourceKind = "ROLE";
      e.producerRole = dependency.producers.front();
      e.producerRank = core.roleContracts().at(e.producerRole).rank;
      e.consumerRoles = dependency.consumers;
      e.tensorId = dependency.tensors.front();
      e.bundleTensorNames = dependency.tensors;
      e.tensorDigest = dependency.collectiveTensorDigest;
      e.layoutDigest = dependency.collectiveSourceLayoutDigest;
      e.targetLayoutDigest = dependency.collectiveTargetLayoutDigest;
      appendEndpoint(std::move(e));
      continue;
    }
    if (cachedLayout) {
      for (const auto& endpoint : cachedLayout->endpoints.at(index)) appendEndpoint(endpoint);
      continue;
    }
    if (newLayout) newLayout->endpoints[index];
    std::vector<RedistributionSpec> transfers = dependency.redistributions;
    if (transfers.empty()) {
      for (const auto& tensor : dependency.tensors) {
        RedistributionSpec transfer;
        transfer.tensor = tensor; transfer.operation = "PIPELINE";
        transfer.integrityDigest = hash(dependency.tensors);
        transfer.sourceLayoutDigest = hash(NativeJson{{"scope", dependency.keyScope}, {"layout", "adapter-certified-opaque"}});
        transfer.targetLayoutDigest = transfer.sourceLayoutDigest;
        transfers.push_back(std::move(transfer));
      }
    }
    for (const auto& producer : dependency.producers) {
      if (!result.count(producer)) throw std::invalid_argument("projection dependency has a foreign producer");
      if (!feedback) outgoing.insert(producer);
      const auto& provider = core.assignment.providerByRole.at(producer);
      const auto prefix = binding.producerNamespaces.find(provider);
      if (prefix == binding.producerNamespaces.end() || prefix->second.empty() || prefix->second.front() != '/')
        throw std::invalid_argument("projection dependency has no bound producer namespace");
      for (const auto& transfer : transfers) {
        if (transfer.tensor.empty() || transfer.operation.empty() || !digest(transfer.integrityDigest) ||
            !digest(transfer.sourceLayoutDigest) || !digest(transfer.targetLayoutDigest))
          throw std::invalid_argument("projection transfer identity is incomplete");
        std::vector<std::string> consumers = dependency.consumers;
        if (!dependency.redistributions.empty()) {
          const auto checkRanks = [&](const auto& owners, const auto& requested) {
            const std::set<std::uint64_t> wanted(requested.begin(), requested.end());
            std::set<std::uint64_t> matched;
            if (wanted.empty() || wanted.size() != requested.size())
              throw std::invalid_argument("projection redistribution rank cover is invalid");
            for (const auto& owner : owners) {
              const auto rank = core.roleContracts().at(owner).rank;
              if (wanted.count(rank) && !matched.insert(rank).second)
                throw std::invalid_argument("projection redistribution rank has multiple owners");
            }
            if (matched != wanted) throw std::invalid_argument("projection redistribution has a foreign rank");
          };
          checkRanks(dependency.producers, transfer.producerRanks);
          checkRanks(dependency.consumers, transfer.consumerRanks);
          const auto rank = core.roleContracts().at(producer).rank;
          if (std::find(transfer.producerRanks.begin(), transfer.producerRanks.end(), rank) == transfer.producerRanks.end()) continue;
          consumers.erase(std::remove_if(consumers.begin(), consumers.end(), [&](const auto& role) {
            const auto target = core.roleContracts().at(role).rank;
            return std::find(transfer.consumerRanks.begin(), transfer.consumerRanks.end(), target) == transfer.consumerRanks.end();
          }), consumers.end());
          if (consumers.empty()) throw std::invalid_argument("projection redistribution has no selected consumer rank");
        }
        auto e = endpointBase(); e.producerNamespace = prefix->second;
        e.groupId = binding.groupId; e.groupEpoch = binding.groupEpoch; e.round = binding.operationIndex;
        e.operation = transfer.operation; e.sourceKind = "ROLE"; e.producerRole = producer;
        e.producerRank = core.roleContracts().at(producer).rank; e.consumerRoles = std::move(consumers);
        e.tensorId = transfer.tensor; e.tensorDigest = transfer.integrityDigest;
        // The planner's tensor ID is a logical/authenticated transport
        // identity.  A real sliced ONNX role can expose several concrete
        // values at one cut (Qwen's residual/attention boundary is four).
        // Bind those members from the prepared producer/consumer contracts;
        // never rename them to the logical ID.
        const auto& producerAssembly = core.roleContracts().at(producer);
        const auto isGenerationState = [&core] (const std::string& name) {
          return std::find(core.generationContract.stateInputNames.begin(),
                           core.generationContract.stateInputNames.end(), name) !=
                   core.generationContract.stateInputNames.end() ||
            std::find(core.generationContract.stateOutputNames.begin(),
                      core.generationContract.stateOutputNames.end(), name) !=
                   core.generationContract.stateOutputNames.end();
        };
        const auto appendIfShared = [&] (const auto& candidate) {
          if (isGenerationState(candidate.name))
            return;
          const bool sharedByEveryConsumer = std::all_of(
            e.consumerRoles.begin(), e.consumerRoles.end(),
            [&core, &candidate] (const auto& consumer) {
              const auto& consumerAssembly = core.roleContracts().at(consumer);
              return std::any_of(
                consumerAssembly.expectedInputs.begin(),
                consumerAssembly.expectedInputs.end(),
                [&candidate] (const auto& input) {
                  return candidate.name == input.name &&
                    candidate.dtype == input.dtype && candidate.shape == input.shape;
                });
            });
          if (sharedByEveryConsumer)
            e.bundleTensorNames.push_back(candidate.name);
        };
        for (const auto& output : producerAssembly.expectedOutputs)
          appendIfShared(output);
        for (const auto& input : producerAssembly.expectedInputs)
          appendIfShared(input);
        std::sort(e.bundleTensorNames.begin(), e.bundleTensorNames.end());
        e.bundleTensorNames.erase(std::unique(e.bundleTensorNames.begin(),
                                              e.bundleTensorNames.end()),
                                  e.bundleTensorNames.end());
        e.layoutDigest = transfer.sourceLayoutDigest; e.targetLayoutDigest = transfer.targetLayoutDigest;
        e.segmentCount = context.maxSegments;
        if (newLayout) newLayout->endpoints[index].push_back(staticEndpoint(e));
        appendEndpoint(std::move(e));
      }
    }
  }
  if (consumedBindings.size() != context.dependencies.size())
    throw std::invalid_argument("projection contains an unused dependency binding");
  std::vector<std::string> terminals;
  if (!candidate.resultEgressRole.empty()) terminals.push_back(selected(candidate.resultEgressRole));
  else for (const auto& item : result) if (!outgoing.count(item.first)) terminals.push_back(item.first);
  if (terminals.size() != 1) throw std::invalid_argument("projection has no unique terminal response owner");
  for (auto& item : result) item.second.dataflow.terminalResponseOwner = item.first == terminals.front();
  detail::logNativeWorkPhase(core.requestId, cachedLayout ? "projection_layout_rebind" : "projection_layout_build", geometryTiming);
  const auto certifyTiming = detail::nativePhaseStarted();
  certify(result, sealed);
  detail::logNativeWorkPhase(core.requestId, "projection_layout_certify", certifyTiming);
  if (context.layoutOwner) {
    if (newLayout) *context.layoutOwner = std::move(newLayout);
    try {
      logRuntimeEvidence("NDNSF_DI_NATIVE_PROJECTION_LAYOUT request=" + core.requestId +
        " reused=" + (cachedLayout ? "1" : "0"));
    }
    catch (...) { }
  }
  return result;
}

void NativePlanProjectionBuilder::certify(std::map<std::string, NativeRoleProjectionInputs>& result,
                                         const NativeSealedPlan& sealed)
{
  const auto& core = sealed.core;
  std::vector<NativeSelectionProjectionV3> validation;
  for (auto& item : result) {
    auto& dataflow = item.second.dataflow;
    const auto certify = [&](NativeTensorEndpointV3& e) {
      const auto expectedStride = e.sourceKind == "ROLE" && core.generationContract.enabled
        ? core.generationContract.streamingOperationStride : 0;
      if (e.namingOperationStride != expectedStride ||
          ndn::Name(e.requester) != ndn::Name(core.requesterIdentity) ||
          (e.sourceKind == "APPLICATION_INPUT" &&
           ndn::Name(e.producerNamespace) != ndn::Name(core.requesterIdentity)))
        throw std::invalid_argument("projection tensor naming contract differs from its authenticated owner");
      certifyEndpoint(e);
    };
    for (auto& e : dataflow.mayPublish) certify(e);
    for (auto& e : dataflow.mustFetch) certify(e);
    dataflow.waitFor.clear();
    if (!dataflow.mustFetch.empty()) {
      NativeReadinessPredicateV3 wait; wait.mode = "ALL";
      for (const auto& e : dataflow.mustFetch) {
        // Readiness is for prefill, not future decode/finalization objects.
        if (e.operation == "TOKEN_FEEDBACK") continue;
        if (!core.generationContract.enabled || e.sourceKind != "ROLE" ||
            e.round < core.generationContract.streamingOperationStride)
          wait.endpointDigests.push_back(e.endpointDigest);
      }
      if (!wait.endpointDigests.empty()) dataflow.waitFor.push_back(std::move(wait));
    }
    auto json = nativeDataflowJson(dataflow); json.erase("dataflow_digest"); dataflow.dataflowDigest = hash(json);
    NativeSelectionProjectionV3 value;
    value.provider = core.assignment.providerByRole.at(item.first); value.requestId = core.requestId;
    value.attempt = core.attempt; value.planDigest = sealed.planDigest;
    if (core.fixedCoreOwner)
      value.fixedRoleOwner = std::shared_ptr<const NativeSelectionRoleV3>(core.fixedCoreOwner, &core.roleContracts().at(item.first));
    else value.selectedRole = core.roleContracts().at(item.first);
    value.executionRole = item.second.executionRole;
    value.generationContract = core.generationContract;
    value.dataflow = dataflow; validation.push_back(std::move(value));
  }
  validateNativeSelectionProjectionSetV3(validation);
}
} // namespace ndnsf::di
