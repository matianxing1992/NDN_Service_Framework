#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-runner.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace ndnsf::di::qwen {

namespace {

std::vector<NativeSubgraphEntryContract>
runtimeEntries(const std::vector<QwenTpEntryContract>& entries)
{
  std::vector<NativeSubgraphEntryContract> result;
  for (const auto& source : entries) {
    source.validate();
    NativeSubgraphEntryContract entry;
    entry.entryId = source.entryId;
    entry.unitId = source.unitId;
    entry.inputNames = source.inputNames;
    entry.outputNames = source.outputNames;
    entry.relativeGraphPath = source.relativeGraphPath;
    entry.graphDigest = source.graphDigest;
    entry.inputSlotByGraphName = source.inputSlotByGraphName;
    entry.outputSlotByGraphName = source.outputSlotByGraphName;
    entry.passthroughSlotByInputGraphName = source.passthroughSlotByInputGraphName;
    std::set<std::string> stateOutputs(source.stateOutputGraphNames.begin(),
                                       source.stateOutputGraphNames.end());
    for (const auto& name : source.outputNames) {
      if (name.rfind("present.", 0) == 0) stateOutputs.insert(name);
    }
    for (const auto& name : stateOutputs) {
      if (std::find(entry.outputNames.begin(), entry.outputNames.end(), name) ==
          entry.outputNames.end())
        entry.outputNames.push_back(name);
    }
    for (const auto& name : source.inputNames) {
      const std::string prefix = "past_key_values.";
      if (name.rfind(prefix, 0) != 0) continue;
      const auto successor = "present." + name.substr(prefix.size());
      if (stateOutputs.erase(successor) == 0)
        throw std::invalid_argument("Qwen state input has no declared successor");
      entry.stateSuccessorByInputGraphName.emplace(name, successor);
    }
    if (!stateOutputs.empty())
      throw std::invalid_argument("Qwen state output has no declared input");
    result.push_back(std::move(entry));
  }
  return result;
}

} // namespace

NativeQwenTensorRunner::NativeQwenTensorRunner(
  std::vector<QwenTpEntryContract> entries, ChildFactory childFactory,
  std::function<NativeModelRunnerSpec(const QwenTpEntryContract&)> specFactory)
  : NativeEntryRunner(runtimeEntries(entries), std::move(childFactory),
      [&entries, &specFactory] (const NativeSubgraphEntryContract& entry) {
        if (!specFactory)
          throw std::invalid_argument("Qwen runner requires a spec factory");
        const auto source = std::find_if(entries.begin(), entries.end(),
          [&entry] (const auto& value) { return value.entryId == entry.entryId; });
        if (source == entries.end())
          throw std::logic_error("Qwen runtime entry has no source contract");
        return specFactory(*source);
      })
  , m_entries(std::move(entries))
{
}

std::map<std::string, TensorBundle>
NativeQwenTensorRunner::run(const RoleExecutionContext& ctx)
{
  if (!ctx.activeStep || !ctx.generationLineage || ctx.generationInputTokenCount == 0)
    return NativeEntryRunner::run(ctx);
  const auto& entries = entryContracts();
  const auto entry = std::find_if(entries.begin(), entries.end(), [&] (const auto& item) {
    return item.entryId == ctx.activeStep->artifactEntry;
  });
  if (entry == entries.end() || entry->unitId != ctx.role)
    return NativeEntryRunner::run(ctx);
  const auto epochSlot = [&] (const std::string& graphName) -> const std::string* {
    const auto binding = entry->inputSlotByGraphName.find(graphName);
    if (binding == entry->inputSlotByGraphName.end())
      return nullptr;
    const auto& slots = ctx.activeStep->inputSlots;
    const auto found = std::find_if(slots.begin(), slots.end(), [&] (const auto& slot) {
      return slot.slotId == binding->second && slot.source == "EPOCH_INPUT";
    });
    return found == slots.end() ? nullptr : &found->slotId;
  };
  const auto* mask = epochSlot("attention_mask");
  const auto* positions = epochSlot("position_ids");
  if (mask == nullptr || positions == nullptr ||
      (ctx.stepInputsBySlot.count(*mask) != 0 && ctx.stepInputsBySlot.count(*positions) != 0))
    return NativeEntryRunner::run(ctx);

  CausalPositionInputContractV1 contract;
  contract.policy = "qwen-causal-position-v1";
  contract.attentionMaskInputName = "attention_mask";
  contract.positionIdsInputName = "position_ids";
  StatefulOnnxIoContractV1 io;
  io.inputNames = entry->inputNames;
  auto generated = materializeCausalPositionInputsV1(
    contract, io, *ctx.generationLineage, ctx.generationInputTokenCount);
  auto bound = ctx;
  bound.stepInputsBySlot.try_emplace(*mask, std::move(generated.at("attention_mask")));
  bound.stepInputsBySlot.try_emplace(*positions, std::move(generated.at("position_ids")));
  return NativeEntryRunner::run(bound);
}

std::string
NativeQwenTensorRunner::checkTemplate(const std::vector<NativeExecutionStepSpec>& steps,
                                      const std::string& unitId) const
{
  return NativeEntryRunner::checkTemplate(steps, unitId);
}

bool
NativeQwenTensorRunner::supportsOpaqueStateHandles() const
{
  return NativeEntryRunner::supportsOpaqueStateHandles();
}

std::optional<NativeOpaqueStateHandleV1>
NativeQwenTensorRunner::stateHandleSnapshot(const std::string& sessionId) const
{
  return NativeEntryRunner::stateHandleSnapshot(sessionId);
}

void
NativeQwenTensorRunner::releaseSessionState(const std::string& sessionId)
{
  NativeEntryRunner::releaseSessionState(sessionId);
}

bool
NativeQwenTensorRunner::supportsConversationStateTransfer() const
{
  return NativeEntryRunner::supportsConversationStateTransfer();
}

std::optional<NativeConversationStateHandleV1>
NativeQwenTensorRunner::promoteSessionStateToConversation(const std::string& sessionId, const std::string& conversationKey)
{
  return NativeEntryRunner::promoteSessionStateToConversation(sessionId, conversationKey);
}

bool
NativeQwenTensorRunner::restoreConversationState(const NativeConversationStateHandleV1& state,
                                               const std::string& sessionId)
{
  return NativeEntryRunner::restoreConversationState(state, sessionId);
}

bool
NativeQwenTensorRunner::releaseConversationState(const NativeConversationStateHandleV1& state)
{
  return NativeEntryRunner::releaseConversationState(state);
}

std::optional<ExecutionEvidence>
NativeQwenTensorRunner::executionEvidenceSnapshot() const
{
  return NativeEntryRunner::executionEvidenceSnapshot();
}

std::optional<NativeRuntimeMetrics>
NativeQwenTensorRunner::runtimeMetricsSnapshot() const
{
  return NativeEntryRunner::runtimeMetricsSnapshot();
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
  // Keep the admitted compute backend separate from this dispatch wrapper. Legacy direct
  // entry tables retain their generic default; production admission requires the binding.
  const auto backend = parent.metadata.find("tensorParallelChildBackend");
  child.backend = backend == parent.metadata.end() ? "onnxruntime" : backend->second;
  if (child.backend != "onnxruntime" && child.backend != "onnxruntime-cpu" &&
      child.backend != "onnxruntime-cuda")
    throw std::invalid_argument("tensor-parallel child requires an ordinary ONNX backend");
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
  // Ask for an encoded bundle on the wire path. The ordinary runner returns a single output's raw
  // bytes keyed by its scope, and a multi-output entry as one encoded bundle; a raw payload cannot
  // be decoded, so a single-output entry -- the head is one -- would arrive unreadable and the
  // failure would surface as "no logits" far from its cause. Declaring the bundle makes every
  // entry uniform for a caller that only reads bytes.
  //
  // A local value is always one bundle keyed by name, so the preference on the child context takes
  // precedence over this and the local path stays reachable. Without that precedence the child
  // would encode, the parent would decode, and every value would take the round trip this change
  // exists to remove.
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
  // The successor may be recorded either as an output bound to a slot or as a declared state
  // name. The assembler puts `present.*` in the latter and leaves it out of `outputNames`, so
  // reading only `outputNames` found no state at all -- which made no child stateful, so the
  // parent never returned the KV its children computed and nothing above the worker could see it.
  for (const auto& name : entry.outputNames)
    if (name.rfind("present.", 0) == 0) stateOutputs.push_back(name);
  for (const auto& name : entry.stateOutputGraphNames)
    if (name.rfind("present.", 0) == 0 &&
        std::find(stateOutputs.begin(), stateOutputs.end(), name) == stateOutputs.end())
      stateOutputs.push_back(name);
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
