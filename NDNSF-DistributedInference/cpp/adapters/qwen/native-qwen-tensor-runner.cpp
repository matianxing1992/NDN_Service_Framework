#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-runner.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace ndnsf::di::qwen {

NativeQwenTensorRunner::NativeQwenTensorRunner(
  std::vector<QwenTpEntryContract> entries, ChildFactory childFactory,
  std::function<NativeModelRunnerSpec(const QwenTpEntryContract&)> specFactory)
  : m_entries(std::move(entries))
  , m_childFactory(std::move(childFactory))
  , m_specFactory(std::move(specFactory))
{
  if (!m_childFactory || !m_specFactory)
    throw std::invalid_argument("tensor-parallel runner requires child and spec factories");
  if (m_entries.empty())
    throw std::invalid_argument("tensor-parallel runner requires at least one entry");
  std::set<std::string> ids;
  for (const auto& entry : m_entries) {
    entry.validate();
    if (!ids.insert(entry.entryId).second)
      throw std::invalid_argument("tensor-parallel runner entry ids are duplicated");
  }
  // Prepare every entry once. A failure here happens before the first step, so a
  // partially assembled unit can never reach the reuse cache.
  for (const auto& entry : m_entries) {
    m_children.emplace(entry.entryId, m_childFactory(m_specFactory(entry)));
  }
}

const QwenTpEntryContract*
NativeQwenTensorRunner::findEntry(const std::string& entryId) const
{
  const auto found = std::find_if(m_entries.begin(), m_entries.end(),
    [&entryId] (const auto& entry) { return entry.entryId == entryId; });
  return found == m_entries.end() ? nullptr : &*found;
}

std::string
NativeQwenTensorRunner::checkTemplate(const std::vector<NativeExecutionStepSpec>& steps,
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

/** Attention entries are the only ones that can own KV state. */
bool holdsKvState(const QwenTpEntryContract& entry)
{
  return entry.kind == "ATTN";
}

} // namespace

bool
NativeQwenTensorRunner::supportsOpaqueStateHandles() const
{
  // C6: INIT/MLP/HEAD legitimately cannot answer a state question, so the unit's
  // answer comes from the attention children only. Requiring every child would
  // make a correct unit report "no state support" and disable reuse entirely.
  bool anyAttention = false;
  for (const auto& entry : m_entries) {
    if (!holdsKvState(entry)) continue;
    anyAttention = true;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) return false;
    if (!child->second->supportsOpaqueStateHandles()) return false;
  }
  return anyAttention;
}

std::optional<NativeOpaqueStateHandleV1>
NativeQwenTensorRunner::stateHandleSnapshot(const std::string& sessionId) const
{
  if (!supportsOpaqueStateHandles()) return std::nullopt;
  NativeOpaqueStateHandleV1 unit;
  // One token per attention child, so a restore can reach the exact sub-session
  // that owns each layer's KV. The unit handle names the unit; the token names
  // the children.
  std::string token = "ndnsf-resident-state-v1:tp-unit";
  std::size_t children = 0;
  for (const auto& entry : m_entries) {
    if (!holdsKvState(entry)) continue;
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
    if (!holdsKvState(entry)) continue;
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
NativeQwenTensorRunner::releaseSessionState(const std::string& sessionId)
{
  // Every child, not only the attention ones: a non-state child may still hold
  // request-scoped buffers, and leaving them resident for a session the caller
  // considers terminal is the leak the lifecycle rule forbids.
  for (auto& child : m_children) {
    if (child.second) child.second->releaseSessionState(sessionId);
  }
}

std::optional<ExecutionEvidence>
NativeQwenTensorRunner::executionEvidenceSnapshot() const
{
  std::optional<ExecutionEvidence> unit;
  std::string artifactDigest;
  for (const auto& entry : m_entries) {
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) continue;
    const auto evidence = child->second->executionEvidenceSnapshot();
    if (!evidence) continue;
    const auto artifact = evidence->artifactDigests.find(entry.entryId);
    if (!unit) {
      unit = *evidence;
      artifactDigest = artifact == evidence->artifactDigests.end()
        ? std::string{} : artifact->second;
      continue;
    }
    // Load and warmup are the conjunction over the entries: a unit whose INIT ran but whose
    // attention entry did not load is not ready, and reporting the first child's answer
    // would let one successful entry speak for the whole rank.
    unit->loadCompleted = unit->loadCompleted && evidence->loadCompleted;
    unit->warmupCompleted = unit->warmupCompleted && evidence->warmupCompleted;
    unit->warmupSkippedUnrankedInput =
      unit->warmupSkippedUnrankedInput || evidence->warmupSkippedUnrankedInput;
    if (artifactDigest.empty() && artifact != evidence->artifactDigests.end())
      artifactDigest = artifact->second;
  }
  if (!unit) return std::nullopt;
  // The Provider assigned a unit role, not an entry id, so the aggregate answers under the
  // unit's name with the artifact this rank was assembled from.
  unit->roles = {m_entries.front().unitId};
  unit->artifactDigests.clear();
  if (!artifactDigest.empty())
    unit->artifactDigests.emplace(m_entries.front().unitId, artifactDigest);
  return unit;
}

bool
NativeQwenTensorRunner::supportsConversationStateTransfer() const
{
  // Promising transfer from one child would promise something that fails half
  // way, so the answer is the conjunction over the KV holders.
  bool anyAttention = false;
  for (const auto& entry : m_entries) {
    if (!holdsKvState(entry)) continue;
    anyAttention = true;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) return false;
    if (!child->second->supportsConversationStateTransfer()) return false;
  }
  return anyAttention;
}

std::optional<NativeConversationStateHandleV1>
NativeQwenTensorRunner::promoteSessionStateToConversation(
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
    if (!holdsKvState(entry)) continue;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) {
      release();
      return std::nullopt;
    }
    // One key per child: two attention children sharing a key would make the
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

bool
NativeQwenTensorRunner::restoreConversationState(
  const NativeConversationStateHandleV1& state, const std::string& sessionId)
{
  if (sessionId.empty() || state.conversationKey.empty()) return false;
  // Restore is all-or-nothing. A child that fails leaves the ones already restored
  // holding state from a different checkpoint, so those are rolled back before this
  // returns false.
  std::vector<std::shared_ptr<NativeModelRunner>> restored;
  const auto rollback = [&restored] {
    for (auto& child : restored) {
      try {
        child->releaseSessionState("");  // per-session release is a no-op for ""
      }
      catch (...) {
      }
    }
    restored.clear();
  };
  for (const auto& entry : m_entries) {
    if (!holdsKvState(entry)) continue;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) {
      rollback();
      return false;
    }
    bool ok = false;
    try {
      ok = child->second->restoreConversationState(state, sessionId);
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
NativeQwenTensorRunner::releaseConversationState(
  const NativeConversationStateHandleV1& state)
{
  if (state.conversationKey.empty()) return false;
  // Best effort across every KV child: one failing release must not prevent the
  // others from freeing their buffers, but the caller still learns it failed.
  bool allReleased = true;
  std::size_t attempts = 0;
  for (const auto& entry : m_entries) {
    if (!holdsKvState(entry)) continue;
    const auto child = m_children.find(entry.entryId);
    if (child == m_children.end() || !child->second) {
      allReleased = false;
      continue;
    }
    ++attempts;
    try {
      if (!child->second->releaseConversationState(state)) allReleased = false;
    }
    catch (...) {
      allReleased = false;
    }
  }
  return attempts != 0 && allReleased;
}

std::map<std::string, TensorBundle>
NativeQwenTensorRunner::run(const RoleExecutionContext& ctx)
{
  if (!ctx.activeStep || !ctx.roleSteps)
    throw std::invalid_argument(
      "tensor-parallel runner requires an active step and its role template");
  const auto& step = *ctx.activeStep;
  if (step.unitId != ctx.role)
    throw std::invalid_argument("active step does not belong to this role");

  // SUM has no graph and no child. Reducing here would make the runner decide
  // what the worker is supposed to own, so the operation is refused rather than
  // handled locally.
  if (step.operation == "SUM") {
    throw std::invalid_argument(
      "tensor-parallel runner does not execute SUM; the worker reduces it");
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
    if (isEncodedTensorBundle(value->second.payload)) {
      // The bundle names its tensor after the slot only when the slot carries exactly one
      // value; a vector-valued slot names each member. So both the graph input name and the
      // slot id are legitimate keys, and the graph input name is preferred because that is
      // what a multi-member bundle uses.
      for (const auto& key : {graphInput, bound->second}) {
        auto tensors = decodeTensorBundle(value->second.payload);
        try {
          auto tensor = findTensor(std::move(tensors), key);
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
  if (childRunner == m_children.end())
    throw std::logic_error("tensor-parallel runner lost a prepared child");

  const auto produced = childRunner->second->run(child);

  // Resolve the child's outputs back onto the entry's declared boundary names.
  //
  // An entry with one graph output comes back keyed by its own scope. An entry with
  // several -- an attention subgraph produces the projected attention output and both KV
  // successors -- comes back as one encoded bundle, because the ordinary runner packs
  // multiple outputs rather than inventing a key per tensor. Both shapes are legitimate,
  // so the lookup is by declared name inside whatever the child returned, and a missing
  // name is a contract violation rather than something to paper over.
  std::map<std::string, TensorBundle> byName;
  for (const auto& item : produced) {
    const auto& bundle = item.second;
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
  for (const auto& stateName : entry->stateOutputGraphNames) {
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

namespace {

/** Parse the minimal step descriptors a spec may carry for validation.
 *
 * Only the fields the consistency check needs are read. The worker still owns the
 * authoritative template from the sealed plan; this copy exists so construction
 * can reject an obviously mismatched table.
 */
std::vector<NativeExecutionStepSpec>
parseDeclaredSteps(const std::string& json)
{
  const auto parsed = nativeParseJson(json);
  if (!parsed.is_array()) {
    throw std::invalid_argument("native-qwen-tensor step declaration is not an array");
  }
  std::vector<NativeExecutionStepSpec> steps;
  steps.reserve(parsed.size());
  for (const auto& item : parsed) {
    NativeExecutionStepSpec step;
    step.stepId = item.at("stepId").get<std::string>();
    step.unitId = item.at("unitId").get<std::string>();
    step.operation = item.at("operation").get<std::string>();
    step.artifactEntry = item.value("artifactEntry", std::string{});
    steps.push_back(std::move(step));
  }
  return steps;
}

/** Read one entry contract out of the spec metadata.
 *
 * The spec is a flat string map, so the entry table travels as one JSON value
 * under a fixed key. A role dispatched to this backend without a table is a
 * configuration error, not an empty unit.
 */
std::vector<QwenTpEntryContract>
entriesFromSpec(const NativeModelRunnerSpec& spec)
{
  const auto found = spec.metadata.find("tensorParallelEntries");
  if (found == spec.metadata.end() || found->second.empty()) {
    throw std::invalid_argument(
      "native-qwen-tensor spec carries no entry table");
  }
  const auto parsed = nativeParseJson(found->second);
  if (!parsed.is_array() || parsed.empty()) {
    throw std::invalid_argument(
      "native-qwen-tensor entry table is empty or not an array");
  }
  // One parser for the table, shared with the assembler, so the runner cannot
  // accept a table the writer would have refused.
  return parseQwenTpEntryTable(found->second);
}

/** Child specs inherit the parent's backend-neutral identity and point at one
 * entry's model file. The session cache is deliberately absent: C4 keeps a whole
 * prepared group in the parent and does not raise the default cache capacity.
 */
NativeModelRunnerSpec
childSpec(const NativeModelRunnerSpec& parent, const QwenTpEntryContract& entry)
{
  NativeModelRunnerSpec child;
  child.role = entry.entryId;
  child.kind = parent.kind;
  // Children run through the ordinary ONNX creator, not this backend.
  child.backend = "onnxruntime";
  // Each entry is one assembled subgraph file. The parent records the directory
  // it was handed, and the entry id names the file inside it.
  const auto root = parent.metadata.find("tensorParallelEntryRoot");
  if (root == parent.metadata.end() || root->second.empty()) {
    throw std::invalid_argument(
      "native-qwen-tensor spec carries no entry root");
  }
  // The contract names the file it was written from, relative to the root, so a
  // path never has to be re-derived from an id.
  child.path = root->second + "/" + entry.relativeGraphPath;
  child.metadata = parent.metadata;
  // The parent's passthrough list describes the whole role's boundary: values its role
  // publishes by carrying an input through. A single entry publishes only what its own
  // contract says, and the parent already handles an entry-level passthrough through
  // `passthroughSlotByInputGraphName`. Inheriting the role-level list made INIT try to pass
  // `attention_mask` through, which it never declares, so the child failed before running.
  child.metadata.erase("passthroughTensors");
  child.metadata.erase("passthrough_tensors");
  // The causal-position policy names the assembled role graph's own inputs. An entry has a
  // different signature -- INIT's mask arrives as `attention_mask`, an attention entry's as
  // the reformat subgraph's output name -- and the parent already routes every one of them
  // from the step slots. Carrying the role-level names down made each child demand an input
  // its graph does not have.
  child.metadata.erase("positionInputPolicy");
  child.metadata.erase("attentionMaskInputName");
  child.metadata.erase("positionIdsInputName");
  child.metadata.erase("cachePositionInputName");
  // Always ask for an encoded bundle. The ordinary runner returns a single output's raw bytes
  // keyed by its scope, and a multi-output entry as one encoded bundle; the parent decodes what
  // it gets to find tensors by name. A raw payload cannot be decoded, so a single-output entry
  // -- the head is one -- would arrive unreadable, and the failure surfaces as "no encoded
  // logits" far from its cause. Declaring the bundle makes every entry uniform.
  child.metadata["forceOutputBundle"] = "true";
  // The entry's own graph names are the IO contract; the runner maps scopes.
  for (std::size_t index = 0; index < entry.inputNames.size(); ++index)
    child.metadata["inputScope." + std::to_string(index)] = entry.inputNames[index];
  for (std::size_t index = 0; index < entry.outputNames.size(); ++index)
    child.metadata["outputScope." + std::to_string(index)] = entry.outputNames[index];
  // A KV successor is an output the child returns; a past tensor is an input it consumes.
  // Declaring them makes the child a stateful session, which is what lets it seed a
  // zero-state first round instead of failing on a missing input -- and that is the only
  // way a fresh request reaches an attention entry at all.
  // The entry decides whether it is stateful, not the role. An INIT entry carries no KV
  // pair, and inheriting `statefulModel` from the parent made it look like a causal graph
  // that owes the runtime a position policy, so it failed before running.
  child.metadata.erase("statefulModel");
  child.metadata.erase("stateInputNames");
  child.metadata.erase("stateOutputNames");
  child.metadata.erase("stateSuccessorMap");
  std::vector<std::string> stateInputs;
  std::vector<std::string> stateOutputs;
  for (const auto& name : entry.inputNames)
    if (name.rfind("past_key_values.", 0) == 0) stateInputs.push_back(name);
  for (const auto& name : entry.outputNames)
    if (name.rfind("present.", 0) == 0) stateOutputs.push_back(name);
  if (stateInputs.size() == stateOutputs.size() && !stateInputs.empty()) {
    const auto join = [] (const std::vector<std::string>& values) {
      std::string result;
      for (const auto& value : values) {
        if (!result.empty()) result += ',';
        result += value;
      }
      return result;
    };
    child.metadata["statefulModel"] = "true";
    child.metadata["stateInputNames"] = join(stateInputs);
    child.metadata["stateOutputNames"] = join(stateOutputs);
    // The successor map pairs each past tensor with its own `present` output, which is how
    // the child knows the two are one lineage rather than unrelated tensors.
    std::string successor;
    for (const auto& name : stateInputs) {
      const auto suffix = name.substr(std::string("past_key_values.").size());
      if (!successor.empty()) successor += ',';
      successor += name + "=present." + suffix;
    }
    child.metadata["stateSuccessorMap"] = successor;
    // The entry reads positions and a mask the plan supplies as tensors, so the child takes
    // them from its input bundles rather than materializing them from a token lineage.
    // Declaring the policy would make the child demand an authenticated lineage this
    // request-level path does not carry.
    child.metadata["positionInputPolicy"] = "";
    child.metadata["attentionMaskInputName"] = "";
    child.metadata["positionIdsInputName"] = "";
  }
  return child;
}

} // namespace

void
registerQwenTensorBackend(RegistryNativeModelRunnerFactory& factory)
{
  // The children are created through this same registry, so the ONNX backend
  // must already be present. Registering this one first would make every child
  // construction fail at run time instead of at startup.
  if (!factory.hasBackend("onnxruntime")) {
    throw std::invalid_argument(
      "native-qwen-tensor requires the onnxruntime backend to be registered first");
  }
  factory.registerBackend(QWEN_TENSOR_RUNNER_BACKEND,
    [&factory] (const NativeModelRunnerSpec& spec) {
      auto entries = entriesFromSpec(spec);
      auto runner = std::make_shared<NativeQwenTensorRunner>(
        std::move(entries),
        [&factory] (const NativeModelRunnerSpec& child) {
          return factory.create(child);
        },
        [&spec] (const QwenTpEntryContract& entry) {
          return childSpec(spec, entry);
        });
      // The runner is the only place that holds both the table and, later, the
      // template the worker will drive. Validate the pair the caller declared, so
      // a mismatch surfaces at construction rather than after partials are
      // published. A spec without a declared template skips the check: the
      // worker supplies the template at admission.
      const auto declared = spec.metadata.find("tensorParallelSteps");
      if (declared != spec.metadata.end() && !declared->second.empty()) {
        const auto steps = parseDeclaredSteps(declared->second);
        const auto problem = runner->checkTemplate(steps, spec.role);
        if (!problem.empty()) {
          throw std::invalid_argument(
            "native-qwen-tensor entry table does not match its template: " + problem);
        }
      }
      return runner;
    });
}

} // namespace ndnsf::di::qwen
