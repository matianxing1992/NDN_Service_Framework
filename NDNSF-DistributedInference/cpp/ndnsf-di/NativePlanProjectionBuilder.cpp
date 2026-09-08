#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanProjectionBuilder.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace ndnsf::di {
namespace {
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
      {"consumers", endpoint.consumerRoles}, {"tensor", endpoint.tensorId}, {"tensorDigest", endpoint.tensorDigest}});
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
  const auto& core = sealed.core;
  if (candidate.computedDigest() != candidate.candidateDigest || candidate.candidateDigest != core.candidateDigest ||
      candidate.model.contentDigest != core.modelDigest || candidate.graphDigest != core.graphDigest ||
      !context.nowMs || context.nowMs >= core.expiresAtMs || !context.noProgressMs || !context.maxSegments)
    throw std::invalid_argument("projection candidate or request bounds differ from sealed plan");
  std::map<std::string, const NativeAdmittedOfferV3*> admitted;
  for (const auto& offer : offers)
    if (!admitted.emplace(offer.observation().provider, &offer).second)
      throw std::invalid_argument("projection contains duplicate admitted providers");
  std::map<std::string, NativeRoleProjectionInputs> result;
  for (const auto& assignment : core.assignment.providerByRole) {
    const auto& role = assignment.first;
    const auto& spec = core.assemblyByRole.at(role);
    const auto found = admitted.find(assignment.second);
    if (found == admitted.end()) throw std::invalid_argument("projection has no admitted assigned provider");
    NativePlanSealer::grantView(core, *found->second, sealed.security);
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
  const auto selected = [&](const std::string& stage) {
    for (const auto& item : core.assemblyByRole)
      if (item.second.role == stage && item.second.rank == 0) return item.first;
    throw std::invalid_argument("projection ingress or egress stage has no selected role");
  };
  const auto endpointBase = [&] {
    NativeTensorEndpointV3 e;
    e.requester = core.requestId.front() == '/' ? core.requestId : "/" + core.requestId;
    e.requestId = core.requestId; e.attempt = core.attempt; e.planDigest = sealed.planDigest;
    e.securityProfile = "NDNSF_DATA_V1"; e.noProgressDeadlineMs = context.noProgressMs;
    e.hardDeadlineMs = core.expiresAtMs - context.nowMs;
    return e;
  };
  if (!candidate.inputIngressRole.empty()) {
    if (!digest(context.logicalInputDigest) || !digest(context.inputLayoutDigest))
      throw std::invalid_argument("projection application input identity is missing");
    auto e = endpointBase(); e.producerNamespace = core.executionPlan.serviceName;
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
    if (dependency.operationKind == "TOKEN_FEEDBACK") continue;
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
      outgoing.insert(producer);
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
              const auto rank = core.assemblyByRole.at(owner).rank;
              if (wanted.count(rank) && !matched.insert(rank).second)
                throw std::invalid_argument("projection redistribution rank has multiple owners");
            }
            if (matched != wanted) throw std::invalid_argument("projection redistribution has a foreign rank");
          };
          checkRanks(dependency.producers, transfer.producerRanks);
          checkRanks(dependency.consumers, transfer.consumerRanks);
          const auto rank = core.assemblyByRole.at(producer).rank;
          if (std::find(transfer.producerRanks.begin(), transfer.producerRanks.end(), rank) == transfer.producerRanks.end()) continue;
          consumers.erase(std::remove_if(consumers.begin(), consumers.end(), [&](const auto& role) {
            const auto target = core.assemblyByRole.at(role).rank;
            return std::find(transfer.consumerRanks.begin(), transfer.consumerRanks.end(), target) == transfer.consumerRanks.end();
          }), consumers.end());
          if (consumers.empty()) throw std::invalid_argument("projection redistribution has no selected consumer rank");
        }
        auto e = endpointBase(); e.producerNamespace = prefix->second;
        e.groupId = binding.groupId; e.groupEpoch = binding.groupEpoch; e.round = binding.operationIndex;
        e.operation = transfer.operation; e.sourceKind = "ROLE"; e.producerRole = producer;
        e.producerRank = core.assemblyByRole.at(producer).rank; e.consumerRoles = std::move(consumers);
        e.tensorId = transfer.tensor; e.tensorDigest = transfer.integrityDigest;
        e.layoutDigest = transfer.sourceLayoutDigest; e.targetLayoutDigest = transfer.targetLayoutDigest;
        e.segmentCount = context.maxSegments;
        e.consumerRole = e.consumerRoles.front(); certifyEndpoint(e);
        result.at(producer).dataflow.mayPublish.push_back(e);
        for (const auto& consumer : e.consumerRoles) {
          if (!result.count(consumer)) throw std::invalid_argument("projection dependency has a foreign consumer");
          e.consumerRole = consumer; result.at(consumer).dataflow.mustFetch.push_back(e);
        }
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
  certify(result, sealed);
  return result;
}

void NativePlanProjectionBuilder::certify(std::map<std::string, NativeRoleProjectionInputs>& result,
                                         const NativeSealedPlan& sealed)
{
  const auto& core = sealed.core;
  std::vector<NativeSelectionProjectionV3> validation;
  for (auto& item : result) {
    auto& dataflow = item.second.dataflow;
    for (auto& e : dataflow.mayPublish) certifyEndpoint(e);
    for (auto& e : dataflow.mustFetch) certifyEndpoint(e);
    dataflow.waitFor.clear();
    if (!dataflow.mustFetch.empty()) {
      NativeReadinessPredicateV3 wait; wait.mode = "ALL";
      for (const auto& e : dataflow.mustFetch) wait.endpointDigests.push_back(e.endpointDigest);
      dataflow.waitFor.push_back(std::move(wait));
    }
    auto json = nativeDataflowJson(dataflow); json.erase("dataflow_digest"); dataflow.dataflowDigest = hash(json);
    NativeSelectionProjectionV3 value;
    value.provider = core.assignment.providerByRole.at(item.first); value.requestId = core.requestId;
    value.attempt = core.attempt; value.planDigest = sealed.planDigest;
    value.selectedRole = core.assemblyByRole.at(item.first); value.executionRole = item.second.executionRole;
    value.dataflow = dataflow; validation.push_back(std::move(value));
  }
  validateNativeSelectionProjectionSetV3(validation);
}
} // namespace ndnsf::di
