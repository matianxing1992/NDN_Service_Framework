#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGroupProjectionBuilder.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <set>

namespace ndnsf::di {
namespace {
std::string hash(const NativeJson& value) { return nativePlanningDigest(nativeCanonicalJson(value)); }
// The initial round identifies its sealed dependency. Equal transfer contracts
// across producer ranks share one collective operation; different tensors do not.
std::string transferKey(const NativeTensorEndpointV3& e)
{
  return nativeCanonicalJson(NativeJson{e.round, e.operation, e.tensorId,
    e.tensorDigest, e.layoutDigest, e.targetLayoutDigest});
}
struct Operation {
  std::string kind, source, target;
  std::set<std::string> producers, consumers;
  std::uint64_t index = 0;
  std::optional<std::uint64_t> sealedIndex;
};
struct Group {
  std::vector<std::string> providers;
  std::map<std::string, std::uint64_t> rank;
  std::map<std::string, Operation> operations;
};
}

std::map<std::string, NativeRoleProjectionInputs> NativeGroupProjectionBuilder::build(
  const NativeSealedPlan& sealed, const NativeSplitCandidate& candidate,
  const NativeGroupKeyAdmission& keys, NativeProjectionContext context, std::uint64_t maxBytes)
{
  sealed.validate();
  const auto& core = sealed.core;
  if (!context.dependencies.empty() || !context.groupCapabilitiesByRole.empty() ||
      !maxBytes || maxBytes > (64ULL << 20) || !context.maxSegments || context.maxSegments > (1U << 20) ||
      !context.nowMs || context.nowMs >= core.expiresAtMs || !context.noProgressMs)
    throw std::invalid_argument("group projection bounds or caller authorization are invalid");
  context.noProgressMs = std::min(context.noProgressMs, core.expiresAtMs - context.nowMs);
  // A Provider may own more than one role.  Keep one admitted offer per
  // Provider because the projection builder treats offers as identity-keyed
  // observations; repeating the same offer for each role would be rejected as
  // a forged duplicate before group construction starts.
  std::vector<NativeAdmittedOfferV3> offers;
  std::set<std::string> admittedProviders;
  for (const auto& assignment : core.assignment.providerByRole) {
    if (admittedProviders.insert(assignment.second).second)
      offers.push_back(keys.offer(assignment.second));
  }
  std::map<std::string, std::string> parent;
  const auto find = [&](std::string provider) {
    while (parent.at(provider) != provider) provider = parent.at(provider);
    return provider;
  };
  for (const auto& dependency : core.executionPlan.dependencies) {
    std::set<std::string> members;
    for (const auto& role : dependency.producers) members.insert(core.assignment.providerByRole.at(role));
    for (const auto& role : dependency.consumers) members.insert(core.assignment.providerByRole.at(role));
    if (members.empty())
      throw std::invalid_argument("group dependency has no assigned members");
    for (const auto& member : members) parent.emplace(member, member);
    for (const auto& member : members) {
      const auto left = find(*members.begin()), right = find(member);
      if (left != right) parent[std::max(left, right)] = std::min(left, right);
    }
  }
  std::map<std::string, std::vector<std::string>> components;
  for (const auto& entry : parent) components[find(entry.first)].push_back(entry.first);
  std::map<std::string, Group> groups;
  std::map<std::string, std::string> groupByProvider;
  std::size_t component = 0;
  for (const auto& entry : components) {
    const auto id = "group-" + hash(NativeJson{{"plan", sealed.planDigest},
      {"component", component++}, {"members", entry.second}}).substr(7, 32);
    auto& group = groups[id]; group.providers = entry.second;
    for (const auto& provider : group.providers) {
      group.rank.emplace(provider, group.rank.size()); groupByProvider.emplace(provider, id);
    }
  }
  for (std::size_t i = 0; i < core.executionPlan.dependencies.size(); ++i) {
    const auto& dependency = core.executionPlan.dependencies[i];
    // Feedback closes the epoch loop. It is authorized below, but must not
    // become a one-epoch readiness endpoint (which would create a cycle).
    if (dependency.operationKind == "TOKEN_FEEDBACK") continue;
    auto& binding = context.dependencies[i];
    binding.groupId = groupByProvider.at(core.assignment.providerByRole.at(dependency.producers.at(0)));
    binding.groupEpoch = "1"; binding.operationIndex = i;
    for (const auto& role : dependency.producers) {
      const auto& provider = core.assignment.providerByRole.at(role);
      binding.producerNamespaces.emplace(provider, keys.endpoint(provider));
    }
  }
  auto projections = NativePlanProjectionBuilder::build(sealed, candidate, offers, context);
  for (const auto& role : projections) for (const auto& endpoint : role.second.dataflow.mayPublish) {
    auto& group = groups.at(endpoint.groupId);
    auto& operation = group.operations[transferKey(endpoint)];
    operation.kind = endpoint.operation; operation.source = endpoint.layoutDigest; operation.target = endpoint.targetLayoutDigest;
    operation.producers.insert(std::to_string(group.rank.at(core.assignment.providerByRole.at(endpoint.producerRole))));
    for (const auto& consumer : endpoint.consumerRoles)
      operation.consumers.insert(std::to_string(group.rank.at(core.assignment.providerByRole.at(consumer))));
  }
  for (const auto& dependency : core.executionPlan.dependencies) {
    if (dependency.operationKind != "TOKEN_FEEDBACK") continue;
    if (!core.generationContract.enabled || !dependency.useNdnsfDataV1 ||
        dependency.producers.size() != 1 || dependency.consumers.size() != 1 ||
        dependency.tensors != std::vector<std::string>{"input_ids"} ||
        dependency.collectiveOperationIndex >= core.generationContract.streamingOperationStride)
      throw std::invalid_argument("group feedback execution contract is incomplete");
    const auto& producer = core.assignment.providerByRole.at(dependency.producers.front());
    const auto& consumer = core.assignment.providerByRole.at(dependency.consumers.front());
    auto& group = groups.at(groupByProvider.at(producer));
    const auto producerRank = std::to_string(group.rank.at(producer));
    const auto feedbackLayout = hash(NativeJson{{"tensor", "input_ids"},
      {"layout", "int64[1,1]"}, {"operation", "TOKEN_FEEDBACK"}});
    if (dependency.collectiveProducerRank != producerRank ||
        dependency.collectiveSourceLayoutDigest != feedbackLayout || dependency.collectiveTargetLayoutDigest != feedbackLayout ||
        dependency.collectiveTensorDigest != hash(NativeJson::array({"input_ids"})))
      throw std::invalid_argument("group feedback rank or tensor identity mismatch");
    auto& operation = group.operations["feedback:" + dependency.keyScope];
    if (operation.sealedIndex) throw std::invalid_argument("group feedback operation repeats a scope");
    operation.kind = "TOKEN_FEEDBACK";
    operation.source = dependency.collectiveSourceLayoutDigest;
    operation.target = dependency.collectiveTargetLayoutDigest;
    operation.producers.insert(producerRank);
    operation.consumers.insert(std::to_string(group.rank.at(consumer)));
    operation.sealedIndex = dependency.collectiveOperationIndex;
  }
  for (auto& pair : groups) {
    auto& group = pair.second;
    std::vector<GroupOperationV1> operations;
    const auto& generation = core.generationContract;
    const auto stride = generation.streamingOperationStride;
    if (group.operations.empty() || (generation.enabled && (!stride || stride < group.operations.size() ||
        core.executionPlan.streamingOperationStride != stride || generation.maxGeneratedTokens > (1U << 20))))
      throw std::invalid_argument("group generation operation stride or count is invalid");
    const std::uint64_t epochs = generation.enabled ? generation.maxGeneratedTokens + 1 : 1;
    // Every encoded operation occupies more than 128 bytes. Reject expansions
    // that cannot possibly fit the Selection's 1 MiB capability limit before allocation.
    if (group.operations.size() > ((1U << 20) / 128) / epochs ||
        (epochs > 1 && stride > (std::numeric_limits<std::uint64_t>::max() - group.operations.size()) / (epochs - 1)))
      throw std::invalid_argument("group operation expansion exceeds wire bounds");
    std::uint64_t index = 0;
    std::set<std::uint64_t> reserved;
    for (const auto& item : group.operations)
      if (item.second.sealedIndex && !reserved.insert(*item.second.sealedIndex).second)
        throw std::invalid_argument("group feedback operation index collision");
    for (auto& item : group.operations) {
      auto& op = item.second;
      if (op.sealedIndex) op.index = *op.sealedIndex;
      else {
        while (reserved.count(index)) ++index;
        op.index = index++;
      }
      if (generation.enabled && op.index >= stride)
        throw std::invalid_argument("group operation exceeds generation stride");
      for (std::uint64_t epoch = 0; epoch < epochs; ++epoch)
        operations.push_back({op.index + epoch * stride, op.kind,
          {op.producers.begin(), op.producers.end()}, {op.consumers.begin(), op.consumers.end()},
          hash(NativeJson{{"source", op.source}, {"target", op.target}}), maxBytes, context.maxSegments});
    }
    std::vector<GroupMemberV1> members;
    for (const auto& provider : group.providers)
      members.push_back({provider, group.rank.at(provider), keys.offer(provider).observation().offerDigest, keys.endpoint(provider)});
    auto options = keys.options(); options.maxSegments = context.maxSegments; options.maxInflightBytes = maxBytes;
    ProviderGroupCoordinator coordinator(options);
    const auto capability = coordinator.createCapability(core.requestId, "attempt-" + std::to_string(core.attempt),
      sealed.planDigest, pair.first, 1, std::move(members), std::move(operations), maxBytes,
      context.noProgressMs, core.expiresAtMs - context.nowMs);
    for (auto& projection : projections) {
      const auto& provider = core.assignment.providerByRole.at(projection.first);
      if (!group.rank.count(provider)) continue;
      const auto wire = ProviderGroupCoordinator::encodeCapability(capability.projectForProvider(provider));
      if (wire.size() > (1U << 20)) throw std::invalid_argument("group capability exceeds Selection wire bound");
      projection.second.groupCapabilityV1 = ndn_service_framework::selectionGatedHex(wire);
    }
  }
  for (auto& projection : projections) {
    const auto remap = [&](NativeTensorEndpointV3& endpoint) {
      if (endpoint.sourceKind != "ROLE") return;
      const auto& group = groups.at(endpoint.groupId);
      const auto& operation = group.operations.at(transferKey(endpoint));
      endpoint.producerRank = group.rank.at(core.assignment.providerByRole.at(endpoint.producerRole));
      endpoint.round = operation.index;
    };
    for (auto& endpoint : projection.second.dataflow.mayPublish) remap(endpoint);
    for (auto& endpoint : projection.second.dataflow.mustFetch) remap(endpoint);
  }
  NativePlanProjectionBuilder::certify(projections, sealed);
  return projections;
}
} // namespace ndnsf::di
