#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-entry-runner.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace ndnsf::di {

std::filesystem::path
nativeSourceEntryGraphPath(const std::filesystem::path& root, std::size_t index)
{
  return index == 0 ? root / "model.onnx" :
                      root / ("entry-" + std::to_string(index)) / "model.onnx";
}

std::string
encodeNativeSourceEntryFiles(const std::vector<NativeSourceEntryFiles>& entries)
{
  auto values = NativeJson::array();
  for (const auto& entry : entries) {
    values.push_back({{"entryId", entry.entryId}, {"graphDigest", entry.graphDigest},
                      {"graphBytes", entry.graphBytes},
                      {"initializerDigest", entry.initializerDigest},
                      {"initializerBytes", entry.initializerBytes}});
  }
  return nativeCanonicalJson(values);
}

std::vector<NativeSourceEntryFiles>
decodeNativeSourceEntryFiles(const std::string& wire)
{
  const auto values = nativeParseJson(wire);
  if (!values.is_array() || values.size() < 2)
    throw std::invalid_argument("source artifact must contain multiple entries");
  const auto isDigest = [] (const std::string& value) {
    return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
      std::all_of(value.begin() + 7, value.end(), [] (char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
  };
  std::vector<NativeSourceEntryFiles> entries;
  std::set<std::string> ids;
  for (const auto& value : values) {
    if (!value.is_object() || !value.at("graphBytes").is_number_unsigned() ||
        !value.at("initializerBytes").is_number_unsigned())
      throw std::invalid_argument("source artifact has an invalid entry file size");
    NativeSourceEntryFiles entry{value.at("entryId").get<std::string>(),
      value.at("graphDigest").get<std::string>(), value.at("graphBytes").get<std::uint64_t>(),
      value.at("initializerDigest").get<std::string>(),
      value.at("initializerBytes").get<std::uint64_t>()};
    if (entry.entryId.empty() || !ids.insert(entry.entryId).second || !entry.graphBytes ||
        !isDigest(entry.graphDigest) ||
        (entry.initializerBytes ? !isDigest(entry.initializerDigest) :
                                  !entry.initializerDigest.empty()))
      throw std::invalid_argument("source artifact has an invalid entry file identity");
    entries.push_back(std::move(entry));
  }
  if (encodeNativeSourceEntryFiles(entries) != nativeCanonicalJson(values))
    throw std::invalid_argument("source artifact has unknown entry file fields");
  return entries;
}

void
NativeSubgraphEntryContract::validate() const
{
  if (entryId.empty() || unitId.empty() || inputNames.empty() || outputNames.empty())
    throw std::invalid_argument("entry contract is incomplete");
  const auto names = [] (const std::vector<std::string>& values) {
    std::set<std::string> result;
    for (const auto& name : values) {
      if (name.empty() || !result.insert(name).second)
        throw std::invalid_argument("entry has empty or duplicate graph names");
    }
    return result;
  };
  const auto inputs = names(inputNames);
  const auto outputs = names(outputNames);
  const auto bindings = [] (const auto& mapping, const auto& declared) {
    for (const auto& item : mapping) {
      if (declared.count(item.first) == 0 || item.second.empty())
        throw std::invalid_argument("entry binding references an undeclared graph value");
    }
  };
  bindings(inputSlotByGraphName, inputs);
  bindings(outputSlotByGraphName, outputs);
  bindings(passthroughSlotByInputGraphName, inputs);
  for (const auto& item : passthroughSlotByInputGraphName) {
    if (inputSlotByGraphName.count(item.first) == 0)
      throw std::invalid_argument("entry passthrough has no input slot binding");
  }
  std::set<std::string> successors;
  for (const auto& item : stateSuccessorByInputGraphName) {
    if (inputs.count(item.first) == 0 || outputs.count(item.second) == 0 ||
        !successors.insert(item.second).second)
      throw std::invalid_argument("entry state successor binding is incomplete or duplicated");
  }
  if (relativeGraphPath.empty() != graphDigest.empty())
    throw std::invalid_argument("entry has a partial graph file identity");
  if (!relativeGraphPath.empty()) {
    const std::filesystem::path path(relativeGraphPath);
    if (path.is_absolute())
      throw std::invalid_argument("entry graph path must be relative");
    for (const auto& part : path) {
      if (part == "..")
        throw std::invalid_argument("entry graph path escapes the assembly root");
    }
    if (graphDigest.size() != 71 || graphDigest.compare(0, 7, "sha256:") != 0 ||
        !std::all_of(graphDigest.begin() + 7, graphDigest.end(), [] (char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
      throw std::invalid_argument("entry graph digest is not canonical sha256");
  }
}

bool
NativeSubgraphEntryContract::operator==(const NativeSubgraphEntryContract& other) const
{
  return std::tie(entryId, unitId, inputNames, outputNames, relativeGraphPath, graphDigest,
                  inputSlotByGraphName, outputSlotByGraphName, passthroughSlotByInputGraphName,
                  stateSuccessorByInputGraphName) ==
         std::tie(other.entryId, other.unitId, other.inputNames, other.outputNames,
                  other.relativeGraphPath, other.graphDigest, other.inputSlotByGraphName,
                  other.outputSlotByGraphName, other.passthroughSlotByInputGraphName,
                  other.stateSuccessorByInputGraphName);
}

NativeEntryRunner::NativeEntryRunner(
  std::vector<NativeSubgraphEntryContract> entries, ChildFactory childFactory,
  std::function<NativeModelRunnerSpec(const NativeSubgraphEntryContract&)> specFactory)
  : m_entries(std::move(entries))
{
  if (!childFactory || !specFactory)
    throw std::invalid_argument("entry runner requires child and spec factories");
  if (m_entries.empty())
    throw std::invalid_argument("entry runner requires at least one entry");
  std::set<std::string> ids;
  for (const auto& entry : m_entries) {
    entry.validate();
    if (!ids.insert(entry.entryId).second)
      throw std::invalid_argument("entry runner entry ids are duplicated");
  }
  // Prepare every entry once. A failure here happens before the first step, so a
  // partially assembled unit can never reach the reuse cache.
  for (const auto& entry : m_entries) {
    auto spec = specFactory(entry);
    // ORT profiles share the same parent prefix. Entry slots disambiguate them
    // even if multiple sessions start in the same clock tick.
    for (const auto* key : {"providerProfilePrefix", "provider_profile_prefix"}) {
      const auto profile = spec.metadata.find(key);
      if (profile != spec.metadata.end() && !profile->second.empty())
        profile->second += "-entry-" + nativePlanningDigest(entry.entryId).substr(7);
    }
    m_children.emplace(entry.entryId, childFactory(spec));
  }
}

const NativeSubgraphEntryContract*
NativeEntryRunner::findEntry(const std::string& entryId) const
{
  const auto found = std::find_if(m_entries.begin(), m_entries.end(),
    [&entryId] (const auto& entry) { return entry.entryId == entryId; });
  return found == m_entries.end() ? nullptr : &*found;
}

std::string
NativeEntryRunner::checkTemplate(const std::vector<NativeExecutionStepSpec>& steps,
                                      const std::string& unitId) const
{
  if (steps.empty()) return "the unit template has no steps";
  if (unitId.empty()) return "the unit id is empty";
  std::set<std::string> referenced;
  for (const auto& step : steps) {
    if (step.unitId != unitId) {
      return "step " + step.stepId + " belongs to another unit";
    }
    if (step.operation == "SUM") continue;
    if (step.operation != "CALL_SUBGRAPH") {
      return "step " + step.stepId + " has an unsupported operation";
    }
    if (step.artifactEntry.empty()) {
      return "step " + step.stepId + " names no entry";
    }
    if (referenced.insert(step.artifactEntry).second &&
        findEntry(step.artifactEntry) == nullptr) {
      return "the unit template needs an entry this runner does not hold: " +
             step.artifactEntry;
    }
  }
  // The converse also matters: an entry no step references was assembled but
  // never runs, which means the rank artifact is not the one this template
  // describes.
  for (const auto& entry : m_entries) {
    if (entry.unitId != unitId) {
      return "the entry table carries another unit's entry: " + entry.entryId;
    }
    if (referenced.count(entry.entryId) == 0) {
      return "the entry table holds an entry no step references: " + entry.entryId;
    }
  }
  return {};
}

namespace {

/** State ownership comes from admitted bindings, independent of model taxonomy. */
bool holdsState(const NativeSubgraphEntryContract& entry)
{
  return !entry.stateSuccessorByInputGraphName.empty();
}

} // namespace

bool
NativeEntryRunner::supportsOpaqueStateHandles() const
{
  // Stateless entries do not determine state support; every declared state owner must
  // support it. This preserves all-or-nothing aggregation without model-name inference.
  bool anyState = false;
  for (const auto& entry : m_entries) {
    if (!holdsState(entry)) continue;
    anyState = true;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) return false;
    if (!child->second->supportsOpaqueStateHandles()) return false;
  }
  return anyState;
}

std::optional<NativeOpaqueStateHandleV1>
NativeEntryRunner::stateHandleSnapshot(const std::string& sessionId) const
{
  if (!supportsOpaqueStateHandles()) return std::nullopt;
  NativeOpaqueStateHandleV1 unit;
  // One token per stateful child, so a restore can reach the exact sub-session
  // that owns each layer's KV. The unit handle names the unit; the token names
  // the children.
  std::string token = "ndnsf-resident-state-v1:tp-unit";
  std::size_t children = 0;
  for (const auto& entry : m_entries) {
    if (!holdsState(entry)) continue;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) return std::nullopt;
    const auto handle = child->second->stateHandleSnapshot(sessionId);
    if (!handle) {
      // A partial aggregate would let a checkpoint omit layers and still look
      // complete, so a missing child means this unit has no handle to give.
      return std::nullopt;
    }
    token += ";" + entry.entryId + "=" + handle->token;
    ++children;
  }
  if (children == 0) return std::nullopt;
  unit.sessionId = sessionId;
  unit.role = m_entries.front().unitId;
  unit.token = std::move(token);
  // Identity and epoch come from the children, which know the Provider binding;
  // taking them from this runner would invent a binding it never received.
  for (const auto& entry : m_entries) {
    if (!holdsState(entry)) continue;
    const auto child = m_children.find(entry.entryId);
    const auto handle = child->second->stateHandleSnapshot(sessionId);
    if (!handle) return std::nullopt;
    unit.providerIdentity = handle->providerIdentity;
    unit.providerBootId = handle->providerBootId;
    unit.stateInferenceEpoch = handle->stateInferenceEpoch;
    break;
  }
  return unit;
}

void
NativeEntryRunner::releaseSessionState(const std::string& sessionId)
{
  // Every child, not only the stateful ones: a non-state child may still hold
  // request-scoped buffers, and leaving them resident for a session the caller
  // considers terminal is the leak the lifecycle rule forbids.
  for (auto& child : m_children) {
    if (child.second) child.second->releaseSessionState(sessionId);
  }
}

std::optional<ExecutionEvidence>
NativeEntryRunner::executionEvidenceSnapshot() const
{
  std::optional<ExecutionEvidence> unit;
  std::string artifactDigest;
  std::vector<ExecutionEvidence> cudaChildren;
  for (const auto& entry : m_entries) {
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) return std::nullopt;
    const auto evidence = child->second->executionEvidenceSnapshot();
    if (!evidence || !evidence->loadCompleted || !evidence->realCompute ||
        evidence->cpuFallbackUsed ||
        !evidence->warmupReady() ||
        std::find(evidence->roles.begin(), evidence->roles.end(), entry.entryId) ==
          evidence->roles.end())
      return std::nullopt;
    const auto artifact = evidence->artifactDigests.find(entry.entryId);
    if (artifact == evidence->artifactDigests.end() || artifact->second.empty())
      return std::nullopt;
    if (evidence->runnerKind == RunnerKind::OnnxRuntimeCuda) {
      if (cudaChildren.empty()) cudaChildren.reserve(m_entries.size());
      cudaChildren.push_back(*evidence);
    }
    if (!unit) {
      unit = *evidence;
      artifactDigest = artifact->second;
      continue;
    }
    if (unit->runnerKind != evidence->runnerKind || unit->deviceKind != evidence->deviceKind ||
        unit->deviceId != evidence->deviceId || unit->gpuUuid != evidence->gpuUuid ||
        unit->runtimeVersion != evidence->runtimeVersion ||
        unit->providerName != evidence->providerName ||
        unit->providerBootId != evidence->providerBootId ||
        unit->planDigest != evidence->planDigest || unit->modelDigest != evidence->modelDigest ||
        artifactDigest != artifact->second)
      return std::nullopt;
    unit->warmupCompleted = unit->warmupCompleted && evidence->warmupCompleted;
    unit->warmupSkippedUnrankedInput =
      unit->warmupSkippedUnrankedInput || evidence->warmupSkippedUnrankedInput;
    unit->warmupDeferredDependencyInput =
      unit->warmupDeferredDependencyInput || evidence->warmupDeferredDependencyInput;
  }
  if (!unit) return std::nullopt;
  if (!cudaChildren.empty()) {
    std::vector<ExecutionEvidence::EntryProfile> profiles;
    std::vector<std::reference_wrapper<const ExecutionEvidence::NodeProviderAssignments>> groups;
    std::set<std::string> paths;
    for (std::size_t i = 0; i < cudaChildren.size(); ++i) {
      const auto& child = cudaChildren[i];
      const auto& entryId = m_entries[i].entryId;
      const auto& assignments = child.getNodeProviderAssignments();
      if (child.gpuUuid.empty() || child.providerProfilePath.empty() ||
          child.profileRequestId.empty() || child.profileAttemptEpoch == 0 ||
          !paths.insert(child.providerProfilePath).second || assignments.empty() ||
          std::any_of(assignments.begin(), assignments.end(), [&] (const auto& node) {
            return node.role != entryId || (node.modelNode &&
                   (node.nodeName.empty() || node.provider != "CUDAExecutionProvider"));
          }) || std::none_of(assignments.begin(), assignments.end(),
                              [] (const auto& node) { return node.modelNode; }))
        break;
      profiles.push_back({entryId, child.providerProfilePath, child.profileRequestId,
                          child.profileAttemptEpoch});
      groups.emplace_back(assignments);
    }
    if (profiles.size() == m_entries.size()) {
      unit->entryProfiles = std::move(profiles);
      unit->concatenateNodeProviderAssignments(groups);
    }
    else {
      // Readiness can precede profiling, but partial coverage must never qualify a rank.
      unit->entryProfiles.clear();
      unit->setNodeProviderAssignments({});
      unit->providerProfilePath.clear();
      unit->profileRequestId.clear();
      unit->profileAttemptEpoch = 0;
    }
  }
  // The Provider assigned a unit role, not an entry id, so the aggregate answers under the
  // unit's name with the artifact this rank was assembled from.
  unit->roles = {m_entries.front().unitId};
  unit->artifactDigests.clear();
  if (!artifactDigest.empty())
    unit->artifactDigests.emplace(m_entries.front().unitId, artifactDigest);
  return unit;
}

std::optional<NativeRuntimeMetrics>
NativeEntryRunner::runtimeMetricsSnapshot() const
{
  std::optional<NativeRuntimeMetrics> unit;
  std::optional<NativeBackendExecutionTiming> latest;
  for (const auto& entry : m_entries) {
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) continue;
    const auto metrics = child->second->runtimeMetricsSnapshot();
    if (!metrics) continue;
    // The unit ran one backend call per step, so the latest of them is the unit's latest. Which
    // entry made it is an internal detail of this runner; a caller asking the unit what it last
    // did wants the call, not the entry id.
    if (metrics->lastBackendExecution &&
        (!latest || metrics->lastBackendExecution->finishedSteadyNs >=
                      latest->finishedSteadyNs))
      latest = metrics->lastBackendExecution;
    if (!unit) {
      unit = *metrics;
      unit->lastBackendExecution.reset();
      continue;
    }
    unit->stateDeviceToHostBytes += metrics->stateDeviceToHostBytes;
    unit->stateHostToDeviceBytes += metrics->stateHostToDeviceBytes;
    unit->activationInputBytes += metrics->activationInputBytes;
    unit->activationOutputBytes += metrics->activationOutputBytes;
    unit->activationHostToDeviceBytes += metrics->activationHostToDeviceBytes;
    unit->activationDeviceToHostBytes += metrics->activationDeviceToHostBytes;
    unit->controlBytes += metrics->controlBytes;
    unit->stateInputHits += metrics->stateInputHits;
    unit->stateInputMisses += metrics->stateInputMisses;
    unit->stateRecomputes += metrics->stateRecomputes;
    unit->stateReleases += metrics->stateReleases;
  }
  if (!unit) return std::nullopt;
  if (latest && latest->identity) {
    // Report it under the unit's own role. The child's identity names the entry, and the caller
    // asked this runner, which is the unit; the timing itself is the child's real call.
    auto identity = std::make_shared<NativeBackendExecutionIdentity>(*latest->identity);
    identity->role = m_entries.front().unitId;
    latest->identity = std::move(identity);
    unit->lastBackendExecution = std::move(latest);
  }
  return unit;
}

bool
NativeEntryRunner::supportsConversationStateTransfer() const
{
  // Promising transfer from one child would promise something that fails half
  // way, so the answer is the conjunction over the KV holders.
  bool anyState = false;
  for (const auto& entry : m_entries) {
    if (!holdsState(entry)) continue;
    anyState = true;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) return false;
    if (!child->second->supportsConversationStateTransfer()) return false;
  }
  return anyState;
}

std::optional<NativeConversationStateHandleV1>
NativeEntryRunner::promoteSessionStateToConversation(
  const std::string& sessionId, const std::string& conversationKey)
{
  if (sessionId.empty() || conversationKey.empty()) return std::nullopt;
  if (!supportsConversationStateTransfer()) return std::nullopt;
  // Children that promoted, so a later failure can release exactly those.
  std::vector<std::pair<std::shared_ptr<NativeModelRunner>,
                        NativeConversationStateHandleV1>> committed;
  const auto release = [&committed] {
    for (auto& item : committed) {
      try {
        item.first->releaseConversationState(item.second);
      }
      catch (...) {
        // A rollback failure must not replace the original promotion failure; the
        // caller needs to know why the commit did not happen.
      }
    }
    committed.clear();
  };

  NativeConversationStateHandleV1 unit;
  std::size_t children = 0;
  std::string unitToken = "ndnsf-resident-state-v1:tp-unit-conversation";
  for (const auto& entry : m_entries) {
    if (!holdsState(entry)) continue;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) {
      release();
      return std::nullopt;
    }
    // One key per child: two stateful children sharing a key would make the
    // adapter refuse the second promotion as a duplicate conversation.
    const auto childKey = conversationKey + "/" + entry.entryId;
    std::optional<NativeConversationStateHandleV1> promoted;
    try {
      promoted = child->second->promoteSessionStateToConversation(sessionId, childKey);
    }
    catch (...) {
      release();
      return std::nullopt;
    }
    if (!promoted) {
      // A child that cannot promote fails the whole unit: a checkpoint missing one
      // layer must not be published as complete.
      release();
      return std::nullopt;
    }
    if (children == 0) {
      // Identity comes from the first child, which knows the Provider binding;
      // this runner never received one and must not invent it.
      unit.opaque.providerIdentity = promoted->opaque.providerIdentity;
      unit.opaque.providerBootId = promoted->opaque.providerBootId;
      unit.opaque.stateInferenceEpoch = promoted->opaque.stateInferenceEpoch;
    }
    unitToken += ";" + entry.entryId + "=" + promoted->opaque.token;
    committed.emplace_back(child->second, *promoted);
    ++children;
  }
  if (children == 0) return std::nullopt;
  unit.conversationKey = conversationKey;
  // The unit's logical size is the sum of its children's, so a caller bounding
  // memory sees the whole rank rather than one layer.
  unit.logicalBytes = 0;
  for (const auto& item : committed) unit.logicalBytes += item.second.logicalBytes;
  unit.opaque.sessionId = sessionId;
  unit.opaque.role = m_entries.front().unitId;
  unit.opaque.token = std::move(unitToken);
  try {
    unit.validate();
  }
  catch (...) {
    release();
    return std::nullopt;
  }
  return unit;
}

/** The handle one child owns for a unit handle.
 *
 * Promotion, restore and release must agree on this mapping or the three operations address
 * different states. Promotion gives each KV child its own key -- the unit key plus the entry
 * id -- and its own role, because the ONNX adapter refuses a handle whose role is not the graph
 * it holds. Every operation that reaches a child goes through this one function.
 */
NativeConversationStateHandleV1
childConversationHandle(const NativeConversationStateHandleV1& unit,
                        const std::string& entryId)
{
  auto child = unit;
  child.conversationKey = unit.conversationKey + "/" + entryId;
  child.opaque.role = entryId;
  return child;
}

bool
NativeEntryRunner::restoreConversationState(
  const NativeConversationStateHandleV1& state, const std::string& sessionId)
{
  if (sessionId.empty() || state.conversationKey.empty()) return false;
  // Restore is all-or-nothing, and undoing it must not destroy the checkpoint being restored.
  //
  // The ONNX adapter's restore only records a binding from this session to an existing
  // conversation; the conversation itself is the committed state. Releasing that conversation
  // on a partial failure would delete the very checkpoint the caller could still use, so a
  // retry after a failed restore would find nothing -- a worse outcome than the failure. What
  // restore creates is the session binding, and `releaseSessionState` is what removes exactly
  // that while leaving the conversation intact.
  std::vector<std::shared_ptr<NativeModelRunner>> restored;
  const auto rollback = [&restored, &sessionId] {
    for (auto& child : restored) {
      try {
        child->releaseSessionState(sessionId);
      }
      catch (...) {
        // A rollback failure must not replace the original restore failure.
      }
    }
    restored.clear();
  };
  for (const auto& entry : m_entries) {
    if (!holdsState(entry)) continue;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) {
      rollback();
      return false;
    }
    const auto childState = childConversationHandle(state, entry.entryId);
    bool ok = false;
    try {
      ok = child->second->restoreConversationState(childState, sessionId);
    }
    catch (...) {
      rollback();
      return false;
    }
    if (!ok) {
      rollback();
      return false;
    }
    restored.push_back(child->second);
  }
  return !restored.empty();
}

bool
NativeEntryRunner::releaseConversationState(
  const NativeConversationStateHandleV1& state)
{
  if (state.conversationKey.empty()) return false;
  // Best effort across every KV child: one failing release must not prevent the
  // others from freeing their buffers, but the caller still learns it failed.
  bool allReleased = true;
  std::size_t attempts = 0;
  for (const auto& entry : m_entries) {
    if (!holdsState(entry)) continue;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) {
      allReleased = false;
      continue;
    }
    ++attempts;
    // The child owns a key derived from the unit's, so releasing the unit handle verbatim
    // would ask it to free a state it never held and would report success while the child
    // kept its buffers.
    const auto childState = childConversationHandle(state, entry.entryId);
    try {
      if (!child->second->releaseConversationState(childState)) allReleased = false;
    }
    catch (...) {
      allReleased = false;
    }
  }
  return attempts != 0 && allReleased;
}

std::map<std::string, TensorBundle>
NativeEntryRunner::run(const RoleExecutionContext& ctx)
{
  if (!ctx.activeStep || !ctx.roleSteps)
    throw std::invalid_argument(
      "entry runner requires an active step and its role template");
  const auto& step = *ctx.activeStep;
  if (step.unitId != ctx.role)
    throw std::invalid_argument("active step does not belong to this role");

  // SUM has no graph and no child. Reducing here would make the runner decide
  // what the worker is supposed to own, so the operation is refused rather than
  // handled locally.
  if (step.operation == "SUM") {
    throw std::invalid_argument(
      "entry runner does not execute SUM; the worker reduces it");
  }
  if (step.operation != "CALL_SUBGRAPH" || step.artifactEntry.empty())
    throw std::invalid_argument("active step is not a subgraph call");

  const auto* entry = findEntry(step.artifactEntry);
  if (entry == nullptr)
    throw std::invalid_argument(
      "active step references an entry this runner does not hold");
  if (entry->unitId != ctx.role)
    throw std::invalid_argument("entry contract belongs to another unit");

  // Map the step's slot values onto the child's graph names. The entry contract
  // is the authority on which graph input each slot feeds, so a missing slot is
  // reported here instead of being guessed from tensor names.
  RoleExecutionContext child = ctx;
  child.inputsByScope.clear();
  child.inputEdgesByScope.clear();
  child.activeStep.reset();
  child.roleSteps.reset();
  child.stepInputsBySlot.clear();
  // The contract owns the binding, so the parent translates rather than guesses. A slot
  // whose id happens to equal the graph name still works, because the binding for such an
  // entry says so; what the parent must not do is assume it.
  for (const auto& graphInput : entry->inputNames) {
    const auto bound = entry->inputSlotByGraphName.find(graphInput);
    if (bound == entry->inputSlotByGraphName.end()) {
      // A synthesized input has no slot by construction: the frozen export derives it
      // from a value the plan does carry, so the caller supplies it under the graph name
      // directly. Naming it in the contract is what keeps this from looking like an
      // omission.
      const auto direct = ctx.stepInputsBySlot.find(graphInput);
      if (direct != ctx.stepInputsBySlot.end())
        child.inputsByScope.emplace(graphInput, direct->second);
      continue;
    }
    const auto value = ctx.stepInputsBySlot.find(bound->second);
    if (value == ctx.stepInputsBySlot.end()) continue;
    // A slot may carry several named tensors, and the child resolves its graph inputs by
    // name inside each bundle. Handing it a bundle that holds only another tensor would
    // make the child report that input as missing even though the step supplied it, so the
    // bundle is narrowed to the one tensor this graph input needs. A slot whose bundle
    // already names the tensor, or a raw payload, is passed through untouched.
    if (value->second.localTensors) {
      const auto views = decodeTensorBundleViews(value->second);
      const auto byName = [&views] (const std::string& name) {
        return std::find_if(views.begin(), views.end(), [&name] (const auto& tensor) {
          return tensor.name == name;
        });
      };
      auto selected = byName(graphInput);
      if (selected == views.end()) selected = byName(bound->second);
      if (selected != views.end() && (views.size() != 1 || selected->name != graphInput)) {
        auto local = std::make_shared<LocalTensorBundle>();
        local->owner = value->second.localTensors->owner;
        local->tensors.push_back(*selected);
        local->tensors.front().name = graphInput;
        TensorBundle narrowed;
        narrowed.name = graphInput;
        narrowed.expectedBytes = tensorBundleWireSize(*local);
        narrowed.localTensors = std::move(local);
        narrowed.transferObservation = value->second.transferObservation;
        child.inputsByScope.emplace(graphInput, std::move(narrowed));
        continue;
      }
    }
    else if (isEncodedTensorBundle(value->second.payload)) {
      // The bundle names its tensor after the slot only when the slot carries exactly one
      // value; a vector-valued slot names each member. So both the graph input name and the
      // slot id are legitimate keys, and the graph input name is preferred because that is
      // what a multi-member bundle uses.
      for (const auto& key : {graphInput, bound->second}) {
        auto tensors = decodeTensorBundle(value->second.payload);
        try {
          auto tensor = findTensor(std::move(tensors), key);
          // The child resolves a bundle member by the *inner* tensor name, so renaming
          // only the envelope would rename nothing the lookup reads. Both names become the
          // graph input this entry declares -- an alias the entry contract owns, not a
          // guess: the slot was bound to this graph input above.
          tensor.name = graphInput;
          TensorBundle narrowed;
          narrowed.name = graphInput;
          narrowed.payload = encodeTensorBundle({std::move(tensor)});
          narrowed.expectedBytes = narrowed.payload.size();
          child.inputsByScope.emplace(graphInput, std::move(narrowed));
          break;
        }
        catch (const std::out_of_range&) {
        }
      }
      if (child.inputsByScope.count(graphInput) != 0) continue;
    }
    child.inputsByScope.emplace(graphInput, value->second);
  }
  if (child.inputsByScope.empty())
    throw std::invalid_argument(
      "active step provides no input for this entry");

  const auto childRunner = m_children.find(entry->entryId);
  if (childRunner == m_children.end() || !childRunner->second)
    throw std::logic_error("entry runner lost a prepared child");

  // Ask the child for typed host values. This runner is about to read individual tensors out
  // of its result and hand them on unchanged, so an encoded bundle would be encoded here and
  // decoded immediately here. The preference is stated on the child context rather than read
  // from the child's activeStep, which this call deliberately clears.
  child.preferLocalTensorOutputs = true;
  const auto produced = childRunner->second->run(child);

  // Resolve the child's outputs back onto the entry's declared boundary names.
  //
  // An entry with one graph output comes back keyed by its own scope. An entry with
  // several -- an attention subgraph produces the projected attention output and both KV
  // successors -- comes back as one bundle, because the ordinary runner packs multiple
  // outputs rather than inventing a key per tensor. Both shapes are legitimate, so the lookup
  // is by declared name inside whatever the child returned, and a missing name is a contract
  // violation rather than something to paper over.
  //
  // Selecting one tensor out of a local bundle moves view metadata and keeps the same storage
  // owner: the bytes are the child's, and re-encoding them here would copy every activation on
  // its way through the unit.
  std::map<std::string, TensorBundle> byName;
  for (const auto& item : produced) {
    const auto& bundle = item.second;
    if (bundle.localTensors) {
      std::vector<NamedTensorView> views;
      try {
        views = decodeTensorBundleViews(bundle);
      }
      catch (const std::invalid_argument&) {
        // A single-output entry need not be a pack; the fallback below covers it by scope.
        byName.emplace(item.first, bundle);
        continue;
      }
      for (auto& view : views) {
        TensorBundle single;
        single.name = view.name;
        auto local = std::make_shared<LocalTensorBundle>();
        local->tensors.push_back(view);
        local->owner = bundle.localTensors->owner;
        // `expectedBytes` is wire-sized (D7:161), so a local value states what it would cost to
        // cross a boundary rather than its raw tensor bytes. The two differ by the bundle framing
        // and this tensor's name and shape, and the field is compared against a wire bundle's, so
        // writing the raw size made that comparison compare a value with itself.
        //
        // Sized before the move: computing it afterwards dereferenced a moved-from pointer, which
        // crashed rather than returning a wrong number.
        single.expectedBytes = tensorBundleWireSize(*local);
        single.localTensors = std::move(local);
        single.transferObservation = bundle.transferObservation;
        byName.emplace(single.name, std::move(single));
      }
      continue;
    }
    if (isEncodedTensorBundle(bundle.payload)) {
      for (auto& tensor : decodeTensorBundle(bundle.payload)) {
        TensorBundle single;
        single.name = tensor.name;
        single.payload = encodeTensorBundle({std::move(tensor)});
        single.expectedBytes = single.payload.size();
        byName.emplace(single.name, std::move(single));
      }
      // A single-output entry whose scope is the graph name is already covered by the
      // fallback below; a bundle that decoded to nothing would hide a producer defect.
      continue;
    }
    byName.emplace(item.first, bundle);
  }
  // Translate graph outputs onto the step's declared slots, and carry the pre-MLP
  // residual through from the input side when the entry does not return it. Both
  // translations come from the contract, so a slot the contract does not name never
  // appears in the result and a graph output the child omitted is an error.
  std::map<std::string, TensorBundle> outputs;
  for (const auto& graphOutput : entry->outputNames) {
    const auto bound = entry->outputSlotByGraphName.find(graphOutput);
    if (bound == entry->outputSlotByGraphName.end()) continue;
    const auto found = byName.find(graphOutput);
    if (found == byName.end())
      throw std::runtime_error(
        "entry did not produce a declared output: " + graphOutput);
    outputs.emplace(bound->second, found->second);
  }
  for (const auto& item : entry->passthroughSlotByInputGraphName) {
    const auto source = ctx.stepInputsBySlot.find(
      entry->inputSlotByGraphName.at(item.first));
    if (source == ctx.stepInputsBySlot.end()) {
      throw std::invalid_argument(
        "entry needs a passthrough slot it was not given: " + item.second);
    }
    outputs.emplace(item.second, source->second);
  }
  // Decode state travels under its own declared names, never under a step slot.
  for (const auto& state : entry->stateSuccessorByInputGraphName) {
    const auto& stateName = state.second;
    const auto found = byName.find(stateName);
    if (found == byName.end())
      throw std::runtime_error(
        "entry did not produce its declared state: " + stateName);
    outputs.emplace(stateName, found->second);
  }
  if (outputs.empty())
    throw std::runtime_error("entry produced no bound output");
  return outputs;
}

} // namespace ndnsf::di
