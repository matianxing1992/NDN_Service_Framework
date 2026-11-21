#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-whole-region-entry.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-entry-runner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-source-transform.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ndnsf::di::detail {

bool
bindNativeSourceProgramEntries(const NativeSelectionProjectionV3& projection,
                               NativeModelRunnerSpec& spec)
{
  spec.sourceProgramDependencyEntries.clear();
  const auto& role = projection.assemblyContract();
  const bool programRole = !role.sourceTransformV1.empty() &&
    nativeParseJson(role.sourceTransformV1).value("schema", std::string{}) ==
      NATIVE_SOURCE_ENTRY_PROGRAM_V1;
  const auto files = spec.metadata.find("sourceEntryFiles");
  if (!programRole) {
    if (files != spec.metadata.end() || spec.metadata.count("sourceEntryProgram") != 0)
      throw std::invalid_argument("unbound source program files");
    return false;
  }
  if (files == spec.metadata.end() || spec.entryContract || projection.generationContract.enabled ||
      spec.role != role.selectedRole || spec.backend != role.backend)
    throw std::invalid_argument("source program has no complete admitted runner file set");
  validateNativeAssembly(role);
  const auto program = NativeOnnxSourceEntryProgramV1::parse(role.sourceTransformV1);
  program.validateRank(projection.plan, role.selectedRole, role.tensorParallelRank);
  (void)program.bindSourceOrdinals(role.nodeIndices);
  const auto entries = decodeNativeSourceEntryFiles(files->second);
  if (entries.size() != program.entries.size())
    throw std::invalid_argument("source program files do not cover the admitted CALLs");
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].entryId != program.entries[index].entryId)
      throw std::invalid_argument("source program files have a different CALL order");
  }
  if (!program.tensorLayouts.empty()) {
    for (const auto& call : projection.plan.steps) {
      if (call.unitId == role.selectedRole && call.operation == "CALL_SUBGRAPH" &&
          std::any_of(call.inputSlots.begin(), call.inputSlots.end(), [] (const auto& slot) {
            return slot.source == "LOCAL_STEP" || slot.source == "REMOTE_STEP";
          }))
        spec.sourceProgramDependencyEntries.push_back(call.artifactEntry);
    }
  }
  spec.metadata["sourceEntryProgram"] = program.canonicalJson();
  return true;
}

/** Bind the exact source-name ABI of an ordinary, single-CALL region. */
bool
bindNativeWholeRegionEntry(const NativeSelectionProjectionV3& projection,
                         NativeModelRunnerSpec& spec)
{
  const auto& plan = projection.plan;
  const auto& role = projection.assemblyContract();
  if (plan.stepPlanVersion != NATIVE_STEP_PLAN_VERSION_V2 ||
      (role.adapterId == "qwen" && role.tensorParallelDegree != 0)) {
    return false;
  }
  const NativeStageLayout* owner = nullptr;
  std::size_t rank = 0;
  for (const auto& stage : plan.stageLayout) {
    const auto member = std::find(stage.unitRoleIds.begin(), stage.unitRoleIds.end(),
                                  role.selectedRole);
    if (member == stage.unitRoleIds.end()) continue;
    if (owner != nullptr) return false;
    owner = &stage;
    rank = static_cast<std::size_t>(std::distance(stage.unitRoleIds.begin(), member));
  }
  // A generic source rank is a member of a degree-N stage whose role kind is
  // HYBRID_RANK; it is the same component-set position for entry binding as a
  // Qwen sharded rank. Its authority is the admitted source transform.
  const bool sourceRank = role.roleKind == "HYBRID_RANK" ||
                          role.roleKind == "TENSOR_RANK";
  if (owner == nullptr ||
      (owner->degree() == 1 &&
        (role.tensorParallelDegree != 0 || role.tensorParallelRank != 0)) ||
      (owner->degree() > 1 &&
        ((role.roleKind != "COMPONENT_SET" && !sourceRank) ||
         role.tensorParallelDegree != owner->degree() ||
         role.tensorParallelRank != rank)))
    return false;
  const NativeExecutionStepSpec* call = nullptr;
  for (const auto& step : plan.steps) {
    if (step.unitId != role.selectedRole || step.operation == "SUM") continue;
    if (step.operation != "CALL_SUBGRAPH" || call != nullptr) return false;
    call = &step;
  }
  if (call == nullptr) return false;
  const auto& step = *call;
  if (step.sourceLayers.second != 0 &&
      step.sourceLayers != std::make_pair(role.layerBegin, role.layerEnd))
    throw std::invalid_argument("whole-region step layers differ from its source interval");
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
