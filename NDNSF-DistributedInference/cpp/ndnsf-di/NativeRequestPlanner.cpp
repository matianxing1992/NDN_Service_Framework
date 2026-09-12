#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGroupProjectionBuilder.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <set>

namespace ndnsf::di {
namespace {
bool isDigestValue(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool sameStrategyIdentity(const NativeStrategyIdentity& left,
                          const NativeStrategyIdentity& right)
{
  return left.name == right.name && left.version == right.version &&
    left.configurationDigest == right.configurationDigest &&
    left.deterministic == right.deterministic;
}

void requireObject(const NativeJson& value, const char* field)
{
  if (!value.is_object())
    throw std::invalid_argument(std::string(field) + " must be an object");
}

void requireExactKeys(const NativeJson& value,
                      std::initializer_list<const char*> required,
                      const char* field)
{
  requireObject(value, field);
  std::set<std::string> expected;
  for (const auto* key : required) expected.emplace(key);
  for (const auto& item : value.items()) {
    if (!expected.count(item.key()))
      throw std::invalid_argument(std::string(field) + " contains an unknown field");
  }
  for (const auto* key : required) {
    if (!value.contains(key))
      throw std::invalid_argument(std::string(field) + " is missing a required field");
  }
}

void requireDigestValue(const NativeJson& value, const char* field)
{
  if (!value.is_string() || !isDigestValue(value.get<std::string>()))
    throw std::invalid_argument(std::string(field) + " must be a lowercase sha256 digest");
}

std::string readString(const NativeJson& value, const char* field)
{
  if (!value.is_string())
    throw std::invalid_argument(std::string(field) + " must be a string");
  return value.get<std::string>();
}

bool readBoolean(const NativeJson& value, const char* field)
{
  if (!value.is_boolean())
    throw std::invalid_argument(std::string(field) + " must be a boolean");
  return value.get<bool>();
}

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
  if (turn.requestId.empty() || turn.executionRequestId.empty() ||
      turn.executionRequestId != sealed.core.requestId ||
      turn.attempt != sealed.core.attempt || turn.parent.serviceName != serviceName ||
      turn.parent.requestContractDigest != requestContractDigest)
    throw std::invalid_argument("conversation turn/request binding mismatch");
  const bool unboundInitialPlan = turn.attempt == 1 && turn.parent.parentContextEpoch == 0 &&
    turn.parent.planRoleMapDigest.empty() && turn.parent.expectedRoles.empty();
  if (!unboundInitialPlan && turn.parent.expectedRoles.size() != providersByRole.size())
    throw std::invalid_argument("conversation turn role map is incomplete");
  std::set<std::string> expected(turn.parent.expectedRoles.begin(), turn.parent.expectedRoles.end());
  std::set<std::string> actual;
  for (const auto& [role, provider] : providersByRole) {
    (void)provider;
    actual.insert(role);
  }
  if (!unboundInitialPlan && expected != actual)
    throw std::invalid_argument("conversation turn role set does not match placement");
  const auto roleMapDigest = conversationRoleMapDigest(providersByRole);
  // Attempt 1 must use the parent's immutable placement. A replacement may
  // switch to an alternate Provider; the coordinator binds that new map after
  // planning while retaining the old map for the parent CAS.
  if (turn.attempt == 1 && !unboundInitialPlan && roleMapDigest != turn.parent.planRoleMapDigest)
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

NativeRequestRuntime nativeRequestRuntimeFromJson(
  const std::string& configurationJson,
  const NativeRequestCatalog& catalog,
  std::shared_ptr<const NativeAuthenticatedGrantClient> grants)
{
  if (configurationJson.size() > 64 * 1024)
    throw std::invalid_argument("native request runtime configuration exceeds limit");
  if (!catalog.preparation || !catalog.splitter)
    throw std::invalid_argument("native request runtime requires a complete catalog");
  if (!grants)
    throw std::invalid_argument("native request runtime requires a native grant client");
  catalog.model.validate();

  const auto root = nativeParseJson(configurationJson);
  requireExactKeys(root,
    {"schema", "contract", "requester_identity", "protection_epoch", "input_layout_digest",
     "security", "budget", "state_mapping", "no_progress_ms", "max_segments"}, "runtime");
  if (!root.at("schema").is_string() ||
      root.at("schema").get<std::string>() != "ndnsf-di-native-request-runtime-v1")
    throw std::invalid_argument("unsupported native request runtime schema");

  const auto& contract = root.at("contract");
  requireExactKeys(contract,
    {"service_name", "task_name", "adapter_name", "adapter_descriptor_digest",
     "adapter_composition_digest", "task_descriptor_digest", "generation_mode",
     "tokenizer_digest"}, "runtime.contract");
  NativeRequestRuntime runtime;
  runtime.contract.serviceName = readString(contract.at("service_name"), "runtime.contract.service_name");
  runtime.contract.taskName = readString(contract.at("task_name"), "runtime.contract.task_name");
  runtime.contract.adapterName = readString(contract.at("adapter_name"), "runtime.contract.adapter_name");
  runtime.contract.adapterDescriptorDigest = readString(contract.at("adapter_descriptor_digest"), "runtime.contract.adapter_descriptor_digest");
  runtime.contract.adapterCompositionDigest = readString(contract.at("adapter_composition_digest"), "runtime.contract.adapter_composition_digest");
  runtime.contract.taskDescriptorDigest = readString(contract.at("task_descriptor_digest"), "runtime.contract.task_descriptor_digest");
  runtime.contract.generationMode = readString(contract.at("generation_mode"), "runtime.contract.generation_mode");
  runtime.contract.tokenizerDigest = readString(contract.at("tokenizer_digest"), "runtime.contract.tokenizer_digest");
  if (runtime.contract.serviceName.empty() || runtime.contract.serviceName.front() != '/' ||
      runtime.contract.taskName.empty() || runtime.contract.adapterName.empty() ||
      runtime.contract.generationMode.empty())
    throw std::invalid_argument("native request runtime contract identity is incomplete");
  if (!isSupportedNativeGenerationMode(runtime.contract.generationMode))
    throw std::invalid_argument(
      "native request runtime generation mode must be TOKEN_DIAGNOSTIC or TOKEN_STREAMING");
  if (!runtime.contract.tokenizerDigest.empty())
    requireDigestValue(contract.at("tokenizer_digest"), "runtime.contract.tokenizer_digest");
  if (runtime.contract.generationMode == "TOKEN_STREAMING" &&
      runtime.contract.tokenizerDigest.empty())
    throw std::invalid_argument(
      "native request runtime TOKEN_STREAMING requires tokenizer digest");
  requireDigestValue(contract.at("adapter_descriptor_digest"), "runtime.contract.adapter_descriptor_digest");
  requireDigestValue(contract.at("adapter_composition_digest"), "runtime.contract.adapter_composition_digest");
  requireDigestValue(contract.at("task_descriptor_digest"), "runtime.contract.task_descriptor_digest");
  if (runtime.contract.adapterName != catalog.model.descriptor.adapterId ||
      runtime.contract.adapterDescriptorDigest != catalog.model.descriptor.adapter.descriptorDigest())
    throw std::invalid_argument("native request runtime adapter identity does not match catalog");
  if (std::find(catalog.model.descriptor.adapter.tasks.begin(),
                catalog.model.descriptor.adapter.tasks.end(), runtime.contract.taskName) ==
      catalog.model.descriptor.adapter.tasks.end())
    throw std::invalid_argument("native request runtime task is not supported by catalog adapter");

  runtime.requesterIdentity = readString(root.at("requester_identity"), "runtime.requester_identity");
  runtime.protectionEpoch = readString(root.at("protection_epoch"), "runtime.protection_epoch");
  runtime.inputLayoutDigest = readString(root.at("input_layout_digest"), "runtime.input_layout_digest");
  if (runtime.requesterIdentity.empty() || runtime.requesterIdentity.front() != '/' ||
      runtime.protectionEpoch.empty() || runtime.protectionEpoch == "plaintext-v1")
    throw std::invalid_argument("native request runtime requester or protection identity is invalid");
  requireDigestValue(root.at("input_layout_digest"), "runtime.input_layout_digest");
  if (runtime.requesterIdentity != grants->requesterIdentity() ||
      runtime.protectionEpoch != grants->protectionEpoch())
    throw std::invalid_argument("native request runtime identity does not match grant client");

  const auto& security = root.at("security");
  requireExactKeys(security, {"policy_digest", "require_protected_artifacts"}, "runtime.security");
  runtime.security.policyDigest = readString(security.at("policy_digest"), "runtime.security.policy_digest");
  runtime.security.requireProtectedArtifacts = readBoolean(
    security.at("require_protected_artifacts"), "runtime.security.require_protected_artifacts");
  requireDigestValue(security.at("policy_digest"), "runtime.security.policy_digest");
  if (!runtime.security.requireProtectedArtifacts)
    throw std::invalid_argument("native request runtime cannot disable protected artifacts");

  const auto& budget = root.at("budget");
  requireExactKeys(budget, {"max_candidates", "max_policy_ms", "max_reentries"}, "runtime.budget");
  const auto readUnsigned = [] (const NativeJson& value, const char* field) -> std::uint64_t {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
      return static_cast<std::uint64_t>(value.get<std::int64_t>());
    throw std::invalid_argument(std::string(field) + " must be a nonnegative integer");
  };
  const auto maxCandidates = readUnsigned(budget.at("max_candidates"), "runtime.budget.max_candidates");
  const auto maxPolicyMs = readUnsigned(budget.at("max_policy_ms"), "runtime.budget.max_policy_ms");
  const auto maxReentries = readUnsigned(budget.at("max_reentries"), "runtime.budget.max_reentries");
  if (maxCandidates > std::numeric_limits<std::size_t>::max() ||
      maxReentries > std::numeric_limits<std::size_t>::max())
    throw std::invalid_argument("native request runtime budget exceeds host size limit");
  runtime.budget = {static_cast<std::size_t>(maxCandidates), maxPolicyMs,
                    static_cast<std::size_t>(maxReentries)};
  runtime.budget.validate();

  const auto& state = root.at("state_mapping");
  requireExactKeys(state, {"inputs", "outputs"}, "runtime.state_mapping");
  try {
    runtime.stateMapping.inputs = state.at("inputs").get<NativeStateTensorMapping::Roles>();
    runtime.stateMapping.outputs = state.at("outputs").get<NativeStateTensorMapping::Roles>();
  }
  catch (const std::exception& error) {
    throw std::invalid_argument(std::string("runtime.state_mapping is malformed: ") + error.what());
  }
  if (runtime.stateMapping.inputs != catalog.stateMapping.inputs ||
      runtime.stateMapping.outputs != catalog.stateMapping.outputs)
    throw std::invalid_argument("native request runtime state mapping does not match catalog");

  const auto noProgressMs = readUnsigned(root.at("no_progress_ms"), "runtime.no_progress_ms");
  const auto maxSegments = readUnsigned(root.at("max_segments"), "runtime.max_segments");
  if (noProgressMs == 0 || noProgressMs > 24ULL * 60ULL * 60ULL * 1000ULL ||
      maxSegments == 0 || maxSegments > (1ULL << 20))
    throw std::invalid_argument("native request runtime progress or segment limit is out of range");
  runtime.noProgressMs = noProgressMs;
  runtime.maxSegments = static_cast<std::size_t>(maxSegments);
  runtime.grants = std::move(grants);
  runtime.catalog = catalog.preparation;
  return runtime;
}

namespace {

NativePlannedRequest planNativeRequestImpl(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const NativeStrategyPorts& ports,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn)
{
  control.requireActive();
  runtime.budget.validate();
  ports.splitterIdentity.validate();
  ports.placementIdentity.validate();
  if (!ports.enumerate || !ports.proposeRoles)
    throw std::invalid_argument("native strategy ports are incomplete");
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
  const auto budgetDeadline = policyStart +
    std::chrono::milliseconds(runtime.budget.maxPolicyMs);
  const auto strategyDeadline = control.deadline == std::chrono::steady_clock::time_point{} ?
    budgetDeadline : std::min(control.deadline, budgetDeadline);
  const ExtensionControl extensionControl{strategyDeadline, control.cancelled};
  extensionControl.requireActive();
  auto candidates = ports.enumerate(model.descriptor, model.graph, runtime.budget,
                                    extensionControl);
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
    if (!sameStrategyIdentity(candidate.splitter, ports.splitterIdentity))
      throw std::invalid_argument("native splitter returned a foreign strategy identity");
    if (runtime.catalog)
      candidate = runtime.catalog->bindStateContracts(model, candidate, runtime.stateMapping, control);
    auto roles = preparation.prepareRoles(model, candidate, control);
    NativeRolePlacementProposalV3 proposal;
    const auto placementStart = std::chrono::steady_clock::now();
    try {
      proposal = ports.proposeRoles(context, closure.digest, roles, offers, epochMs(),
                                    extensionControl);
    }
    catch (const NativeNoFeasiblePlacement&) {
      policyUsed += std::chrono::steady_clock::now() - placementStart;
      extensionControl.requireActive();
      if (policyUsed > policyLimit) throw std::runtime_error("DI_NATIVE_POLICY_BUDGET_EXCEEDED");
      continue;
    }
    policyUsed += std::chrono::steady_clock::now() - placementStart;
    if (policyUsed > policyLimit) throw std::runtime_error("DI_NATIVE_POLICY_BUDGET_EXCEEDED");
    if (!sameStrategyIdentity(proposal.strategy, ports.placementIdentity))
      throw std::invalid_argument("native placement returned a foreign strategy identity");
    extensionControl.requireActive();
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
    extensionControl.requireActive();
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
    extensionControl.requireActive();
    auto core = NativePlanSealer::sealCore(model, candidate, proposal, execution, offers, closure.digest, sealing);
    std::vector<NativeGrantBinding> grants;
    NativeGrantControl grantControl{std::chrono::system_clock::time_point(std::chrono::milliseconds(wireDeadlineMs)), cancelled};
    for (const auto& role : execution.roles) {
      control.requireActive();
      const auto offer = std::find_if(offers.begin(), offers.end(), [&](const auto& value) {
        return value.observation().provider == core.assignment.providerByRole.at(role);
      });
      if (offer == offers.end()) {
        throw std::invalid_argument("selected role Provider is outside the admitted offers");
      }
      grants.push_back(runtime.grants->acquire(core, *offer, runtime.security, grantControl, role));
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

} // namespace

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
  NativeStrategyPorts ports;
  ports.splitterIdentity = splitter.identity();
  ports.placementIdentity = placement.identity();
  ports.enumerate = [&splitter](const NativeModelDescriptor& descriptor,
                                const NativeGraphSnapshot& graph,
                                const NativeCandidateBudget& budget,
                                const ExtensionControl& extension) {
    extension.requireActive();
    auto result = splitter.enumerate(descriptor, graph, budget);
    extension.requireActive();
    return result;
  };
  ports.proposeRoles = [&placement](const NativeOfferBindingContext& context,
                                    const std::string& ackClosedDigest,
                                    const std::vector<NativeSelectionRoleV3>& roles,
                                    const std::vector<NativeAdmittedOfferV3>& offers,
                                    std::uint64_t nowMs,
                                    const ExtensionControl& extension) {
    extension.requireActive();
    auto result = placement.proposeRoles(context, ackClosedDigest, roles, offers, nowMs);
    extension.requireActive();
    return result;
  };
  return planNativeRequestImpl(runtime, options, model, encoded, ports, preparation,
                               admission, closure, control, wireDeadlineMs,
                               std::move(cancelled), conversationTurn);
}

NativePlannedRequest planNativeRequestCooperative(
  const NativeRequestRuntime& runtime, const NativeRequestOptions& options,
  const NativeInspectedModel& model, const NativeEncodedRequest& encoded,
  const CooperativeModelSplitStrategy& splitter,
  const CooperativePlacementStrategy& placement,
  const NativeRequestPreparation& preparation, const NativeOfferAdmission& admission,
  const ndn_service_framework::CollaborationAckClosure& closure,
  const NativeRequestControl& control, std::uint64_t wireDeadlineMs,
  std::shared_ptr<const std::atomic<bool>> cancelled,
  const NativeConversationTurn* conversationTurn)
{
  NativeStrategyPorts ports;
  ports.splitterIdentity = splitter.identity();
  ports.placementIdentity = placement.identity();
  ports.enumerate = [&splitter](const NativeModelDescriptor& descriptor,
                                const NativeGraphSnapshot& graph,
                                const NativeCandidateBudget& budget,
                                const ExtensionControl& extensionControl) {
    return splitter.enumerate(descriptor, graph, budget, extensionControl);
  };
  ports.proposeRoles = [&placement](const NativeOfferBindingContext& context,
                                    const std::string& ackClosedDigest,
                                    const std::vector<NativeSelectionRoleV3>& roles,
                                    const std::vector<NativeAdmittedOfferV3>& offers,
                                    std::uint64_t nowMs,
                                    const ExtensionControl& extensionControl) {
    return placement.proposeRoles(context, ackClosedDigest, roles, offers, nowMs,
                                   extensionControl);
  };
  return planNativeRequestImpl(runtime, options, model, encoded, ports, preparation,
                               admission, closure, control, wireDeadlineMs,
                               std::move(cancelled), conversationTurn);
}
} // namespace ndnsf::di
