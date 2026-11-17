#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-whole-region-entry.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-entry-runner.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ndnsf::di::detail {

/** Bind the exact source-name ABI of an ordinary, single-CALL region. */
bool
bindNativeWholeRegionEntry(const NativeSelectionProjectionV3& projection,
                         NativeModelRunnerSpec& spec)
{
  const auto& plan = projection.plan;
  const auto& role = projection.assemblyContract();
  if (plan.stepPlanVersion != NATIVE_STEP_PLAN_VERSION_V2 ||
      plan.degreeForRole(role.selectedRole) != 1 || role.tensorParallelDegree != 0)
    return false;
  std::vector<const NativeExecutionStepSpec*> steps;
  for (const auto& step : plan.steps)
    if (step.unitId == role.selectedRole) steps.push_back(&step);
  if (steps.size() != 1 || steps.front()->operation != "CALL_SUBGRAPH")
    return false;
  const auto& step = *steps.front();
  NativeSubgraphEntryContract entry;
  entry.entryId = step.artifactEntry;
  entry.unitId = role.selectedRole;
  entry.relativeGraphPath = "model.onnx";
  entry.graphDigest = spec.metadata.at("assembledModelDigest");
  std::set<std::string> inputs;
  std::set<std::string> outputs;
  for (const auto& port : role.expectedInputs) inputs.insert(port.name);
  for (const auto& port : role.expectedOutputs) outputs.insert(port.name);
  const auto& generation = projection.generationContract;
  if (generation.enabled) {
    for (const auto& pair : parseNativeStateSuccessorMap(generation.stateSuccessorMap)) {
      const bool localInput = inputs.count(pair.first) != 0;
      const bool localOutput = outputs.count(pair.second) != 0;
      if (localInput != localOutput)
        throw std::invalid_argument("whole-region state successor crosses its source boundary");
      if (localInput) entry.stateSuccessorByInputGraphName.emplace(pair);
    }
    for (const auto& name : generation.stateInputNames) {
      if (inputs.count(name) != 0 && entry.stateSuccessorByInputGraphName.count(name) == 0)
        throw std::invalid_argument("whole-region state input has no explicit successor");
    }
    for (const auto& name : generation.stateOutputNames) {
      if (outputs.count(name) != 0 &&
          std::none_of(entry.stateSuccessorByInputGraphName.begin(),
                       entry.stateSuccessorByInputGraphName.end(),
                       [&name] (const auto& pair) { return pair.second == name; }))
        throw std::invalid_argument("whole-region state output has no explicit input");
    }
  }
  if (step.localStateSlots.size() != entry.stateSuccessorByInputGraphName.size())
    throw std::invalid_argument("whole-region local state has no complete successor contract");
  std::set<std::string> stateInputs;
  std::set<std::string> stateOutputs;
  for (const auto& pair : entry.stateSuccessorByInputGraphName) {
    stateInputs.insert(pair.first);
    stateOutputs.insert(pair.second);
  }
  const auto bind = [&] (const auto& boundary, const auto& slots, const auto& state,
                        auto& names, auto& bindings) {
    std::set<std::string> used;
    for (const auto& port : boundary) {
      names.push_back(port.name);
      if (state.count(port.name) != 0) continue;
      const auto slot = std::find_if(slots.begin(), slots.end(),
        [&port] (const auto& value) { return value.slotId == port.name; });
      if (slot == slots.end() || slot->dtype != port.dtype || slot->shape != port.shape)
        throw std::invalid_argument("whole-region typed port differs from its source boundary");
      used.insert(slot->slotId);
      bindings.emplace(port.name, slot->slotId);
    }
    if (used.size() != slots.size())
      throw std::invalid_argument("whole-region step declares a foreign or duplicate port");
  };
  bind(role.expectedInputs, step.inputSlots, stateInputs,
       entry.inputNames, entry.inputSlotByGraphName);
  bind(role.expectedOutputs, step.outputSlots, stateOutputs,
       entry.outputNames, entry.outputSlotByGraphName);
  entry.validate();
  spec.entryContract = std::make_shared<const NativeSubgraphEntryContract>(std::move(entry));
  return true;
}

} // namespace ndnsf::di::detail
