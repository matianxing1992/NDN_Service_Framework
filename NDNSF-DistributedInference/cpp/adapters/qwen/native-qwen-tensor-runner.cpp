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
  for (const auto& graphInput : entry->inputNames) {
    // The step names its own slots; the contract lists graph names. A slot whose
    // id equals the graph input is the direct case, and the caller can also
    // provide an explicit mapping through the child spec's inputScope metadata.
    const auto direct = ctx.stepInputsBySlot.find(graphInput);
    if (direct != ctx.stepInputsBySlot.end())
      child.inputsByScope.emplace(graphInput, direct->second);
  }
  if (child.inputsByScope.empty())
    throw std::invalid_argument(
      "active step provides no input for this entry");

  const auto childRunner = m_children.find(entry->entryId);
  if (childRunner == m_children.end())
    throw std::logic_error("tensor-parallel runner lost a prepared child");

  const auto produced = childRunner->second->run(child);

  // Resolve the child's outputs back onto the step's declared output slots. The
  // contract's output order is the binding: a name mismatch is a contract
  // violation, not something to paper over.
  std::map<std::string, TensorBundle> outputs;
  for (const auto& graphOutput : entry->outputNames) {
    const auto found = produced.find(graphOutput);
    if (found == produced.end())
      throw std::runtime_error(
        "entry did not produce a declared output: " + graphOutput);
    outputs.emplace(graphOutput, found->second);
  }
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
  // The entry's own graph names are the IO contract; the runner maps scopes.
  for (std::size_t index = 0; index < entry.inputNames.size(); ++index)
    child.metadata["inputScope." + std::to_string(index)] = entry.inputNames[index];
  for (std::size_t index = 0; index < entry.outputNames.size(); ++index)
    child.metadata["outputScope." + std::to_string(index)] = entry.outputNames[index];
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
