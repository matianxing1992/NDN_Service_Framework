#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeStepExecutor.hpp"

#include <algorithm>
#include <stdexcept>

namespace ndnsf::di {

NativeStepExecutor::NativeStepExecutor(const NativeExecutionPlan& plan, std::string unitId,
                                       StepRunner runner)
  : m_plan(plan), m_unitId(std::move(unitId)), m_runner(std::move(runner))
{
  if (m_unitId.empty() || !m_runner)
    throw std::invalid_argument("step executor requires a unit and an adapter port");
  // The plan's own validator is the authority on template shape, so a caller
  // cannot hand this executor a template that the contract would reject.
  m_plan.validateSteps();
  if (std::find(m_plan.roles.begin(), m_plan.roles.end(), m_unitId) == m_plan.roles.end())
    throw std::invalid_argument("step executor unit is not declared by the plan");
  for (const auto& step : m_plan.steps)
    if (step.unitId == m_unitId) m_steps.push_back(&step);
  if (m_steps.empty())
    throw std::invalid_argument("step executor unit has no steps");
}

const NativeExecutionStepSpec*
NativeStepExecutor::findStep(const std::string& stepId) const
{
  const auto found = std::find_if(m_steps.begin(), m_steps.end(),
    [&stepId] (const auto* step) { return step->stepId == stepId; });
  return found == m_steps.end() ? nullptr : *found;
}

bool
NativeStepExecutor::admit(const std::string& stepId, const std::string& slotId,
                          TensorBundle contribution)
{
  const auto* step = findStep(stepId);
  if (step == nullptr) return false;
  const auto slot = std::find_if(step->inputSlots.begin(), step->inputSlots.end(),
    [&slotId] (const auto& candidate) { return candidate.slotId == slotId; });
  // Accepting an undeclared slot would let a caller inject a value the sealed
  // contract never bound, so it is a refusal rather than a silent store.
  if (slot == step->inputSlots.end() || slot->source != "REMOTE_STEP") return false;
  // One dependency carries one contribution; repeating it would double-count.
  if (m_admitted[stepId].count(slotId) != 0) return false;
  m_admitted[stepId].emplace(slotId, std::move(contribution));
  return true;
}

StepExecutionResult
NativeStepExecutor::advance(const std::map<std::string, TensorBundle>& epochInputs,
                            const std::string& sessionId, const std::string& requestId,
                            std::uint64_t attemptEpoch, std::uint64_t inferenceEpoch,
                            const StatisticsSink& statistics)
{
  StepExecutionResult result;
  // A single pass in template order is enough because the template is validated
  // as a DAG: every producer appears before its consumers.
  for (const auto* step : m_steps) {
    if (m_done.count(step->stepId) != 0) continue;

    std::map<std::string, TensorBundle> inputs;
    std::map<std::string, DependencyEdge> edges;
    bool ready = true;
    for (const auto& slot : step->inputSlots) {
      if (slot.source == "EPOCH_INPUT") {
        const auto found = epochInputs.find(slot.slotId);
        if (found == epochInputs.end()) {
          ready = false;
          break;
        }
        inputs.emplace(slot.slotId, found->second);
        continue;
      }
      if (slot.source == "LOCAL_STEP") {
        // Slot ids are step-local: a consumer's input name need not match the
        // producer's output name. The producer step is the binding, so resolve
        // by producer identity first and fall back to the shared name only when
        // the producer declared several outputs.
        const auto* producer = findStep(slot.producerStepId);
        if (producer == nullptr) {
          ready = false;
          break;
        }
        std::string producerSlot;
        if (producer->outputSlots.size() == 1) {
          producerSlot = producer->outputSlots.front().slotId;
        }
        else {
          const auto named = std::find_if(producer->outputSlots.begin(),
            producer->outputSlots.end(),
            [&slot] (const auto& candidate) { return candidate.slotId == slot.slotId; });
          if (named == producer->outputSlots.end()) {
            ready = false;
            break;
          }
          producerSlot = named->slotId;
        }
        const auto found = m_produced.find(slot.producerStepId + "\n" + producerSlot);
        if (found == m_produced.end()) {
          ready = false;
          break;
        }
        inputs.emplace(slot.slotId, found->second);
        continue;
      }
      if (slot.source == "REMOTE_STEP") {
        const auto admitted = m_admitted.find(step->stepId);
        if (admitted == m_admitted.end()) {
          ready = false;
          break;
        }
        const auto found = admitted->second.find(slot.slotId);
        if (found == admitted->second.end()) {
          ready = false;
          break;
        }
        inputs.emplace(slot.slotId, found->second);
        const auto edge = std::find_if(m_plan.dependencies.begin(), m_plan.dependencies.end(),
          [step, &slot] (const auto& dependency) {
            return dependency.consumerStepId == step->stepId &&
                   dependency.consumerSlotId == slot.slotId;
          });
        if (edge != m_plan.dependencies.end())
          edges.emplace(slot.slotId,
            DependencyEdge{edge->keyScope, edge->producers.front(), edge->consumers.front(),
                           edge->objectNameTemplate, edge->expectedSegments,
                           edge->expectedBytes, edge->tensors});
        continue;
      }
      // LOCAL_STATE and TOKEN_FEEDBACK are not step-bound inputs in this
      // template; leaving them out keeps this executor from inventing values.
      ready = false;
      break;
    }
    if (!ready) continue;

    const auto* stepToRun = step;
    StepExecutionContext context;
    context.workStatistics = statistics ? statistics() : nullptr;
    context.sessionId = sessionId;
    context.role = m_unitId;
    context.stepId = stepToRun->stepId;
    context.requestId = requestId;
    context.attemptEpoch = attemptEpoch;
    context.inferenceEpoch = inferenceEpoch;
    context.operation = stepToRun->operation;
    context.artifactEntry = stepToRun->artifactEntry;
    context.inputsBySlot = std::move(inputs);
    context.inputEdgesBySlot = std::move(edges);

    auto produced = m_runner(context);
    if (!produced) {
      // A refusing adapter must not be mistaken for a step that produced nothing.
      throw std::runtime_error("step adapter refused step " + stepToRun->stepId);
    }
    for (auto& output : produced->outputsBySlot) {
      // Each declared output slot is recorded under its own producer identity so
      // two steps that happen to reuse a slot name cannot overwrite each other.
      m_produced[stepToRun->stepId + "\n" + output.first] = output.second;
      result.outputsBySlot[output.first] = std::move(output.second);
    }
    m_done.insert(stepToRun->stepId);
  }
  return result;
}

bool
NativeStepExecutor::complete() const
{
  return m_done.size() == m_steps.size();
}

std::vector<std::string>
NativeStepExecutor::pendingSteps() const
{
  std::vector<std::string> pending;
  for (const auto* step : m_steps)
    if (m_done.count(step->stepId) == 0) pending.push_back(step->stepId);
  return pending;
}

} // namespace ndnsf::di
