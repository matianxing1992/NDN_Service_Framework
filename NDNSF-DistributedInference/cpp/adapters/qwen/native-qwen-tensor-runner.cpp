#include "NDNSF-DistributedInference/cpp/adapters/qwen/native-qwen-tensor-runner.hpp"

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

} // namespace ndnsf::di::qwen
