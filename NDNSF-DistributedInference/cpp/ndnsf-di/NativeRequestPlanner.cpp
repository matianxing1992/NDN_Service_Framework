#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGroupProjectionBuilder.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <algorithm>
#include <set>

namespace ndnsf::di {
namespace {
std::uint64_t epochMs()
{
  const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  if (value <= 0) throw std::runtime_error("invalid request wall clock");
  return static_cast<std::uint64_t>(value);
}
class FrozenSelection final : public ndn_service_framework::ParticipantSelectionPolicy
{
public:
  explicit FrozenSelection(std::vector<ndn_service_framework::SelectedParticipant> selected)
    : m_selected(std::move(selected)) {}
  std::vector<ndn_service_framework::SelectedParticipant> select(
    const std::vector<ndn_service_framework::AckCandidate>&,
    const std::vector<ndn_service_framework::CollaborationRoleSpec>&) const override
  {
    // Core independently compares every returned ACK with its immutable closure.
    return m_selected;
  }
private:
  const std::vector<ndn_service_framework::SelectedParticipant> m_selected;
};

std::string conversationRoleMapDigest(
  const std::map<std::string, std::string>& providersByRole)
{
  NativeJson roleMap = NativeJson::array();
  for (const auto& [role, provider] : providersByRole)
    roleMap.push_back(NativeJson::array({role, provider}));
  return nativePlanningDigest(nativeCanonicalJson(roleMap));
}

void bindConversationProjections(
  std::map<std::string, NativeRoleProjectionInputs>& projections,
  const NativeSealedPlan& sealed,
  const NativeConversationTurn& turn,
  const std::string& requestContractDigest,
  const std::string& serviceName,
  const std::map<std::string, std::string>& providersByRole)
{
  if (turn.requestId.empty() || turn.requestId != sealed.core.requestId ||
      turn.attempt != sealed.core.attempt || turn.parent.serviceName != serviceName ||
      turn.parent.requestContractDigest != requestContractDigest)
    throw std::invalid_argument("conversation turn/request binding mismatch");
  if (turn.parent.expectedRoles.size() != providersByRole.size())
    throw std::invalid_argument("conversation turn role map is incomplete");
  std::set<std::string> expected(turn.parent.expectedRoles.begin(), turn.parent.expectedRoles.end());
  std::set<std::string> actual;
  for (const auto& [role, provider] : providersByRole) {
    (void)provider;
    actual.insert(role);
  }
  if (expected != actual)
    throw std::invalid_argument("conversation turn role set does not match placement");
  const auto roleMapDigest = conversationRoleMapDigest(providersByRole);
  if (roleMapDigest != turn.parent.planRoleMapDigest)
    throw std::invalid_argument("conversation turn plan-role map mismatch");

  ConversationTurnBindingV1 binding;
  binding.conversationId = turn.parent.conversationId;
  binding.parentContextEpoch = turn.parent.parentContextEpoch;
  binding.successorContextEpoch = turn.successorContextEpoch;
  binding.serviceName = serviceName;
  binding.planRoleMapDigest = roleMapDigest;
  binding.requestContractDigest = requestContractDigest;
  binding.retentionDeadlineMs = turn.parent.retentionDeadlineMs;
  binding.parentCheckpointDigest = turn.parent.parentCheckpointDigest;
  binding.validate();

  std::map<std::string, std::string> parentReceipts;
  if (binding.parentContextEpoch > 0) {
    if (turn.parent.parentCheckpointWire.empty())
      throw std::invalid_argument("conversation append parent checkpoint is missing");
    const auto checkpoint = NativeJson::parse(turn.parent.parentCheckpointWire);
    if (!checkpoint.is_object() || !checkpoint.contains("roleReceiptDigests") ||
        !checkpoint.at("roleReceiptDigests").is_object())
      throw std::invalid_argument("conversation append parent receipt map is missing");
    for (const auto& item : checkpoint.at("roleReceiptDigests").items()) {
      if (!item.value().is_string())
        throw std::invalid_argument("conversation append parent receipt is malformed");
      parentReceipts.emplace(item.key(), item.value().get<std::string>());
    }
    if (parentReceipts.size() != providersByRole.size())
      throw std::invalid_argument("conversation append parent receipt set is incomplete");
  }

  for (const auto& [role, provider] : providersByRole) {
    (void)provider;
    auto projection = projections.find(role);
    if (projection == projections.end())
      throw std::invalid_argument("conversation projection role is missing");
    projection->second.conversationTurnBinding = binding;
    if (binding.parentContextEpoch > 0) {
      ConversationStateReferenceV1 reference;
      reference.conversationId = binding.conversationId;
      reference.contextEpoch = binding.parentContextEpoch;
      reference.serviceName = binding.serviceName;
      reference.planRoleMapDigest = binding.planRoleMapDigest;
      reference.checkpointDigest = binding.parentCheckpointDigest;
      reference.roleName = role;
      const auto receipt = parentReceipts.find(role);
      if (receipt == parentReceipts.end())
        throw std::invalid_argument("conversation append role receipt is missing");
      reference.roleReceiptDigest = receipt->second;
      reference.expiresAtMs = turn.parent.retentionDeadlineMs;
      reference.validate();
      projection->second.conversationStateReference = std::move(reference);
    }
  }
}
}

NativePlannedRequest planNativeRequest(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const NativeModelSplitStrategy& splitter, const NativePlacementStrategy& placement,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn)
{
  control.requireActive();
  runtime.budget.validate();
  if (closure.requestId != ndn::Name(control.requestId) || closure.digest.empty() ||
      wireDeadlineMs <= epochMs() || !runtime.grants || !runtime.security.requireProtectedArtifacts ||
      runtime.requesterIdentity.empty() || runtime.protectionEpoch.empty())
    throw std::invalid_argument("native request runtime/ACK binding is incomplete");
  NativeOfferBindingContext context{control.requestId, control.attempt, runtime.contract.serviceName,
    encoded.modelIntentDigest, model.graph.graphDigest, wireDeadlineMs};
  std::vector<NativeAdmittedOfferV3> offers;
  for (const auto& ack : closure.candidates) {
    if (encoded.recovery && ack.providerName.toUri() == encoded.recovery->failedProvider) continue;
    offers.push_back(admission.verify(ack, context, epochMs()));
  }
  if (offers.empty()) throw std::runtime_error("DI_NATIVE_NO_ADMITTED_PROVIDER");
  const auto policyStart = std::chrono::steady_clock::now();
  auto candidates = splitter.enumerate(model.descriptor, model.graph, runtime.budget);
  control.requireActive();
  auto policyUsed = std::chrono::steady_clock::now() - policyStart;
  const auto policyLimit = std::chrono::milliseconds(runtime.budget.maxPolicyMs);
  if (policyUsed > policyLimit)
    throw std::runtime_error("DI_NATIVE_POLICY_BUDGET_EXCEEDED");
  if (candidates.empty() || candidates.size() > runtime.budget.maxCandidates)
    throw std::runtime_error("DI_NATIVE_CANDIDATE_BUDGET_REJECTED");

  // The splitter supplies its deterministic candidate preference order. No
  // artifact publication happens until one complete placement is validated.
  for (auto candidate : candidates) {
    control.requireActive();
    if (runtime.catalog)
      candidate = runtime.catalog->bindStateContracts(model, candidate, runtime.stateMapping, control);
    auto roles = preparation.prepareRoles(model, candidate, control);
    NativeRolePlacementProposalV3 proposal;
    const auto placementStart = std::chrono::steady_clock::now();
    try { proposal = placement.proposeRoles(context, closure.digest, roles, offers, epochMs()); }
    catch (const NativeNoFeasiblePlacement&) {
      policyUsed += std::chrono::steady_clock::now() - placementStart;
      if (policyUsed > policyLimit) throw std::runtime_error("DI_NATIVE_POLICY_BUDGET_EXCEEDED");
      continue;
    }
    policyUsed += std::chrono::steady_clock::now() - placementStart;
    if (policyUsed > policyLimit) throw std::runtime_error("DI_NATIVE_POLICY_BUDGET_EXCEEDED");
    validateNativeRolePlacement(proposal, roles, offers, epochMs());
    NativeExecutionPlan execution = candidate.executionPlan;
    execution.serviceName = runtime.contract.serviceName;
    execution.modelName = model.descriptor.modelName;
    execution.roles.clear();
    std::map<std::string, std::vector<std::string>> selectedByStage;
    for (const auto& role : proposal.roles) {
      execution.roles.push_back(role.selectedRole);
      selectedByStage[role.role].push_back(role.selectedRole);
    }
    const auto expand = [&](const std::vector<std::string>& stages) {
      std::vector<std::string> result;
      for (const auto& stage : stages) {
        const auto& selected = selectedByStage.at(stage);
        result.insert(result.end(), selected.begin(), selected.end());
      }
      return result;
    };
    for (auto& dependency : execution.dependencies) {
      dependency.producers = expand(dependency.producers);
      dependency.consumers = expand(dependency.consumers);
    }
    if (options.generation) {
      if (!options.stream || !options.generation->enabled ||
          options.generation->mode != "TOKEN_STREAMING")
        throw std::invalid_argument("generation requires the authenticated streaming request path");
      std::set<std::string> sources(execution.roles.begin(), execution.roles.end());
      std::set<std::string> terminals = sources;
      for (const auto& edge : execution.dependencies) {
        if (edge.operationKind == "TOKEN_FEEDBACK")
          throw std::invalid_argument("candidate cannot supply requester-owned generation feedback");
        for (const auto& role : edge.consumers) sources.erase(role);
        for (const auto& role : edge.producers) terminals.erase(role);
      }
      if (sources.size() != 1 || terminals.size() != 1)
        throw std::invalid_argument("generation requires one pipeline source and terminal");
      const auto& generation = *options.generation;
      if (generation.tokenInputName.empty() || generation.stateInputNames.empty() ||
          generation.stateInputNames.size() != generation.stateOutputNames.size() ||
          std::set<std::string>(generation.stateInputNames.begin(), generation.stateInputNames.end()).size() != generation.stateInputNames.size() ||
          std::set<std::string>(generation.stateOutputNames.begin(), generation.stateOutputNames.end()).size() != generation.stateOutputNames.size())
        throw std::invalid_argument("generation state input/output contract is incomplete");
      for (const auto& role : proposal.roles) {
        const auto includes = [](const auto& tensors, const std::string& name) {
          return std::any_of(tensors.begin(), tensors.end(), [&](const auto& tensor) { return tensor.name == name; });
        };
        if (role.selectedRole == *sources.begin() && !includes(role.expectedInputs, generation.tokenInputName))
          throw std::invalid_argument("generation source omits the token input");
        for (const auto& name : generation.stateInputNames)
          if (name.empty() || !includes(role.expectedInputs, name))
            throw std::invalid_argument("generation role omits a sealed state input");
        for (const auto& name : generation.stateOutputNames)
          if (name.empty() || !includes(role.expectedOutputs, name))
            throw std::invalid_argument("generation role omits a sealed state output");
      }
      const auto hash = [](const NativeJson& value) {
        return nativePlanningDigest(nativeCanonicalJson(value));
      };
      NativeDependencySpec feedback;
      feedback.producers = {*terminals.begin()}; feedback.consumers = {*sources.begin()};
      feedback.keyScope = "token-feedback-" + hash({{"request", control.requestId},
        {"attempt", control.attempt}, {"producer", feedback.producers.front()},
        {"consumer", feedback.consumers.front()}}).substr(7, 16);
      feedback.topicPrefix = "/token-feedback";
      feedback.objectNameTemplate = "{producerProvider}/NDNSF/DI/DATA/{sessionId}/{keyScope}/{producerRole}/{sequence}";
      feedback.tensors = {"input_ids"}; feedback.operationKind = "TOKEN_FEEDBACK";
      feedback.useNdnsfDataV1 = true;
      // A dependency can carry several tensor transfers. Reserve a round for
      // each transfer before the feedback round, rather than overlapping the
      // next epoch whenever transfer count exceeds dependency count.
      std::uint64_t transferCount = 0;
      for (const auto& edge : execution.dependencies) {
        transferCount += edge.redistributions.empty() ? edge.tensors.size() : edge.redistributions.size();
        if (transferCount >= (1U << 20)) throw std::invalid_argument("generation operation bound exceeded");
      }
      feedback.collectiveOperationIndex = transferCount;
      // A validated single-source/single-terminal DAG is connected. Group
      // membership therefore uses this same sorted provider set, including a
      // one-provider self-feedback group for a full-model request.
      std::set<std::string> providers;
      std::string terminalProvider;
      for (const auto& role : proposal.roles) {
        const auto& provider = proposal.providerByRole.at(role.selectedRole);
        providers.insert(provider);
        if (role.selectedRole == feedback.producers.front()) terminalProvider = provider;
      }
      if (terminalProvider.empty()) throw std::invalid_argument("generation terminal has no provider");
      feedback.collectiveProducerRank = std::to_string(std::distance(providers.begin(), providers.find(terminalProvider)));
      feedback.collectiveSourceLayoutDigest = feedback.collectiveTargetLayoutDigest =
        hash({{"tensor", "input_ids"}, {"layout", "int64[1,1]"}, {"operation", "TOKEN_FEEDBACK"}});
      feedback.collectiveTensorDigest = hash(NativeJson::array({"input_ids"}));
      execution.dependencies.push_back(std::move(feedback));
      execution.streamingOperationStride = transferCount + 1;
    }
    NativePlanSealingInputs sealing;
    sealing.artifacts = preparation.ensureArtifacts(model, candidate, proposal, control);
    sealing.requesterIdentity = runtime.requesterIdentity;
    sealing.protectionEpoch = runtime.protectionEpoch;
    sealing.expiresAtMs = wireDeadlineMs;
    sealing.requestContractDigest = encoded.requestContractDigest;
    if (options.generation) {
      sealing.generationContract = *options.generation;
      sealing.generationContract.streamingOperationStride = execution.streamingOperationStride;
    }
    for (const auto& role : roles) sealing.assemblyByRole.emplace(role.selectedRole, role);
    auto core = NativePlanSealer::sealCore(model, candidate, proposal, execution, offers, closure.digest, sealing);
    std::vector<NativeGrantBinding> grants;
    NativeGrantControl grantControl{std::chrono::system_clock::time_point(std::chrono::milliseconds(wireDeadlineMs)), cancelled};
    for (const auto& selected : core.offerDigestByProvider) {
      control.requireActive();
      const auto offer = std::find_if(offers.begin(), offers.end(), [&](const auto& value) {
        return value.observation().provider == selected.first;
      });
      grants.push_back(runtime.grants->acquire(core, *offer, runtime.security, grantControl));
    }
    NativePlannedRequest result;
    result.sealed = NativePlanSealer::finalizeSecurity(core, grants, runtime.security);
    NativeProjectionContext projection{epochMs(), runtime.noProgressMs, runtime.maxSegments};
    projection.logicalInputDigest = encoded.logicalInputDigest;
    projection.inputLayoutDigest = runtime.inputLayoutDigest;
    std::map<std::string, NativeRoleProjectionInputs> projections;
    if (execution.dependencies.empty())
      projections = NativePlanProjectionBuilder::build(result.sealed, candidate, offers, projection);
    else {
      std::vector<ndn_service_framework::AckSelectionCandidate> selectedAcks;
      for (const auto& ack : closure.candidates)
        if (core.offerDigestByProvider.count(ack.providerName.toUri())) selectedAcks.push_back(ack);
      NativeGroupKeyAdmission keys(admission, selectedAcks, context, epochMs());
      projections = NativeGroupProjectionBuilder::build(result.sealed, candidate, keys, projection);
    }
    if (conversationTurn) {
      bindConversationProjections(projections, result.sealed, *conversationTurn,
        encoded.requestContractDigest, runtime.contract.serviceName,
        core.assignment.providerByRole);
    }
    auto& plan = result.corePlan;
    plan.ackCollectionTimeMs = static_cast<int>(options.ackTimeoutMs);
    plan.timeoutMs = static_cast<int>(options.timeoutMs);
    std::vector<ndn_service_framework::SelectedParticipant> selected;
    for (const auto& role : execution.roles) {
      const auto& provider = core.assignment.providerByRole.at(role);
      const auto ack = std::find_if(closure.candidates.begin(), closure.candidates.end(), [&](const auto& value) {
        return value.providerName.toUri() == provider;
      });
      if (ack == closure.candidates.end()) throw std::invalid_argument("selected provider is outside ACK closure");
      const auto wire = NativePlanSealer::encode(NativePlanSealer::project(result.sealed, provider, projections.at(role)));
      ndn_service_framework::CollaborationRoleSpec spec;
      spec.role = role; spec.service = ndn::Name(runtime.contract.serviceName);
      spec.requiredArtifact = ndn::Name(core.artifacts.artifactNameByRole.at(role));
      spec.assignmentPayload = ndn::Buffer(wire.begin(), wire.end());
      spec.terminalResponseOwner = projections.at(role).dataflow.terminalResponseOwner;
      if (spec.terminalResponseOwner) result.terminalProvider = provider;
      plan.roles.push_back(spec);
      ndn_service_framework::SelectedParticipant participant;
      participant.role = role; participant.service = spec.service; participant.provider = ndn::Name(provider);
      participant.assignedArtifact = spec.requiredArtifact; participant.assignmentPayload = spec.assignmentPayload;
      participant.ack = *ack; participant.artifactDataName = ndn::Name(core.artifacts.sourceByRole.at(role));
      selected.push_back(std::move(participant));
    }
    std::map<std::string, std::set<std::string>> scopes;
    for (const auto& edge : execution.dependencies) {
      plan.dependencies.push_back({edge.producers, edge.consumers, edge.keyScope, ndn::Name(edge.topicPrefix), true});
      scopes[edge.keyScope].insert(edge.producers.begin(), edge.producers.end());
      scopes[edge.keyScope].insert(edge.consumers.begin(), edge.consumers.end());
    }
    for (const auto& scope : scopes)
      plan.keyScopes.push_back({scope.first, std::vector<std::string>(scope.second.begin(), scope.second.end())});
    if (conversationTurn) {
      plan.keyScopes.push_back({"ndnsf-di-conversation-state-v1", execution.roles});
    }
    plan.participantSelector = std::make_shared<const FrozenSelection>(std::move(selected));
    control.requireActive();
    return result;
  }
  throw std::runtime_error("DI_NATIVE_NO_FEASIBLE_CANDIDATE");
}
} // namespace ndnsf::di
