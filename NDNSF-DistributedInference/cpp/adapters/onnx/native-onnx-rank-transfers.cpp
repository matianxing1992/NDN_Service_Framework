#include "native-onnx-rank-transfers.hpp"
#include "NativeOnnxRecipeAssembler.hpp"
#include "native-onnx-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace ndnsf::di {
namespace {

NativeTensorContract
makeTransferTensor(const NativeJson& facts, const std::string& name,
                   const std::optional<NativeOnnxTransferRange>& range)
{
  const auto& value = facts.at(name);
  NativeTensorContract tensor;
  tensor.name = name;
  tensor.dtype = value.at("dtype").get<std::string>();
  const auto& bytes = value.at("sizeBytes");
  if (!bytes.is_number_unsigned() || bytes.get<std::uint64_t>() == 0)
    throw std::invalid_argument("cross-rank operand has no positive static size: " + name);
  tensor.estimatedBytes = bytes.get<std::uint64_t>();
  for (const auto& dim : value.at("shape")) {
    if (!dim.is_number_integer() || dim.get<std::int64_t>() <= 0)
      throw std::invalid_argument("cross-rank operand has no positive static shape: " + name);
    tensor.shape.emplace_back(dim.get<std::int64_t>());
  }
  if (range) {
    auto& axis = tensor.shape.at(range->axis);
    const auto extent = static_cast<std::uint64_t>(std::get<std::int64_t>(axis));
    if (range->begin >= range->end || range->end > extent || *tensor.estimatedBytes % extent != 0)
      throw std::invalid_argument("cross-rank operand range differs from its source axis: " + name);
    const auto width = range->end - range->begin;
    *tensor.estimatedBytes = *tensor.estimatedBytes / extent * width;
    axis = static_cast<std::int64_t>(width);
  }
  tensor.validate();
  return tensor;
}

} // namespace

std::vector<NativeOnnxRankTransfer>
deriveNativeOnnxRankTransfers(const NativeOnnxGraphInspection& inspection,
                              const std::vector<NativeOnnxNodeLayout>& layouts)
{
  const auto facts = nativeParseJson(inspection.graphMetadataJson);
  const auto& nodes = facts.at("nodes");
  const auto& producers = facts.at("tensorProducers");
  const auto& tensors = facts.at("tensors");
  const auto graphInputs = facts.at("inputs").get<std::set<std::string>>();
  if (nodes.size() != layouts.size() || nodes.size() != inspection.graph.nodes.size())
    throw std::invalid_argument("cross-rank lowering lacks a complete source-node cover");
  std::set<std::uint64_t> initializerConsumers;
  for (const auto& item : inspection.initializerConsumers)
    initializerConsumers.insert(item.second.begin(), item.second.end());
  std::vector<NativeOnnxRankTransfer> result;
  for (std::size_t ordinal = 0; ordinal < layouts.size(); ++ordinal) {
    const auto& decision = layouts[ordinal];
    const auto& inputs = nodes.at(ordinal).at("inputs");
    const auto append = [&] (const std::string& name, std::size_t source, std::size_t target,
                             std::optional<NativeOnnxTransferRange> range) {
      const auto producer = producers.at(name).get<std::uint64_t>();
      if (producer >= ordinal || source == target)
        throw std::invalid_argument("cross-rank operand has an invalid producer");
      result.push_back({producer, ordinal, source, target,
                        makeTransferTensor(tensors, name, range), range});
    };
    if (!decision.concatPieces.empty()) {
      if (initializerConsumers.count(ordinal))
        throw std::invalid_argument("sharded Concat requires all the original operand positions");
      for (const auto& piece : decision.concatPieces) {
        if (piece.producer == piece.consumer) continue;
        const auto name = inputs.at(piece.input).get<std::string>();
        const auto axis = decision.outputs.begin()->second.axis;
        append(name, piece.producer, piece.consumer,
               NativeOnnxTransferRange{axis, piece.begin, piece.end});
      }
      continue;
    }
    std::set<std::string> visited;
    for (const auto& input : inputs) {
      const auto name = input.get<std::string>();
      if (!visited.insert(name).second || graphInputs.count(name)) continue;
      const auto parent = producers.at(name).get<std::size_t>();
      if (parent >= ordinal)
        throw std::invalid_argument("ONNX operand is produced after its consumer");
      const auto& outputs = layouts.at(parent).outputs;
      const auto shard = outputs.find(name);
      const auto rankCount = decision.outputs.empty() ? 1U : 2U;
      const bool gather = std::find(decision.reconstructInputs.begin(),
        decision.reconstructInputs.end(), name) != decision.reconstructInputs.end();
      const bool groupInput = decision.convolution && input == inputs.front() &&
                              decision.convolution->back().inputBegin != 0;
      for (std::size_t target = 0; target < rankCount; ++target) {
        const auto source = shard == outputs.end() ? 0U : 1 - target;
        if (source == target || (shard != outputs.end() && !gather)) continue;
        std::optional<NativeOnnxTransferRange> range;
        if (shard != outputs.end()) {
          const auto [axis, cut] = shard->second;
          const auto extent = tensors.at(name).at("shape").at(axis).get<std::uint64_t>();
          range = NativeOnnxTransferRange{axis, source == 0 ? 0 : cut,
                                          source == 0 ? cut : extent};
        }
        if (groupInput && (!range || range->axis == 1)) {
          const auto& conv = decision.convolution->at(target);
          const auto begin = range ? std::max(range->begin, conv.inputBegin) : conv.inputBegin;
          const auto end = range ? std::min(range->end, conv.inputEnd) : conv.inputEnd;
          if (begin >= end) continue;
          range = NativeOnnxTransferRange{1, begin, end};
        }
        append(name, source, target, range);
      }
    }
  }
  return result;
}

NativeOnnxTensorStepPlan
buildNativeOnnxTensorStepPlan(const NativeOnnxGraphInspection& inspection,
  const std::vector<NativeOnnxNodeLayout>& layouts, const std::array<std::string, 2>& roles,
  const std::string& modelName)
{
  if (roles[0].empty() || roles[1].empty() || roles[0] == roles[1] || modelName.empty())
    throw std::invalid_argument("ONNX tensor plan requires two distinct roles and a model");
  const auto transfers = deriveNativeOnnxRankTransfers(inspection, layouts);
  const auto facts = nativeParseJson(inspection.graphMetadataJson);
  const auto& nodes = facts.at("nodes");
  const auto& producers = facts.at("tensorProducers");
  const auto& tensors = facts.at("tensors");
  const auto graphInputs = facts.at("inputs").get<std::set<std::string>>();
  const auto terminalPorts = facts.at("outputs").get<std::vector<std::string>>();
  if (layouts.empty() || terminalPorts.size() != 1)
    throw std::invalid_argument("ONNX tensor plan requires a single declared terminal port");

  NativeOnnxTensorStepPlan result;
  auto& plan = result.plan;
  plan.version = 2;
  plan.modelName = modelName;
  plan.modelFormat = "onnx";
  plan.plannerKind = "native-onnx-tensor-parallel";
  plan.roles.assign(roles.begin(), roles.end());
  plan.tensorDegree = roles.size();
  plan.stepPlanVersion = NATIVE_STEP_PLAN_VERSION_V2;
  plan.stageLayout = {{"source", inspection.graph.graphDigest, plan.roles}};
  constexpr auto unavailable = std::numeric_limits<std::size_t>::max();
  std::vector<std::array<std::size_t, 2>> callByOrdinal(layouts.size(), {unavailable, unavailable});
  std::set<std::size_t> incoming;
  for (const auto& transfer : transfers) incoming.insert(transfer.consumerOrdinal);
  std::size_t previousRankZero = unavailable;
  for (std::size_t begin = 0; begin < layouts.size();) {
    const bool rankLocal = layouts[begin].outputs.empty();
    auto end = begin + 1;
    while (end < layouts.size() && incoming.count(end) == 0 &&
           layouts[end].outputs.empty() == rankLocal)
      ++end;
    const bool constantsOnly = rankLocal &&
      std::all_of(nodes.begin() + begin, nodes.begin() + end, [] (const auto& node) {
        return node.at("opType") == "Constant" && node.at("inputs").empty();
      });
    if (constantsOnly && previousRankZero != unavailable) {
      // Constant has no incoming edge. Moving it into the preceding Rank0 CALL
      // preserves its source ordinal and cannot add a dependency to that CALL.
      plan.steps[previousRankZero].sourceLayers.second = end;
      for (auto ordinal = begin; ordinal < end; ++ordinal)
        callByOrdinal[ordinal][0] = previousRankZero;
      begin = end;
      continue;
    }
    for (std::size_t rank = 0; rank < (rankLocal ? 1U : 2U); ++rank) {
      auto& step = plan.steps.emplace_back();
      step.stepId = roles[rank] + "/call/" + std::to_string(begin);
      step.unitId = roles[rank];
      step.operation = "CALL_SUBGRAPH";
      step.artifactEntry = step.stepId;
      step.sourceLayers = {begin, end};
      if (rank == 0) previousRankZero = plan.steps.size() - 1;
      for (auto ordinal = begin; ordinal < end; ++ordinal)
        callByOrdinal[ordinal][rank] = plan.steps.size() - 1;
    }
    begin = end;
  }

  const auto makeSlot = [&] (const std::string& name,
                             const std::optional<NativeOnnxTransferRange>& range) {
    NativeStepSlotSpec slot;
    NativeOnnxEntryPort binding{name, range};
    slot.slotId = binding.slotId();
    slot.source = "LOCAL_STEP";
    if (range) {
      auto tensor = makeTransferTensor(tensors, name, range);
      slot.dtype = std::move(tensor.dtype);
      slot.shape = std::move(tensor.shape);
    }
    else {
      const auto& logical = tensors.at(name);
      slot.dtype = logical.at("dtype").get<std::string>();
      for (const auto& dim : logical.at("shape")) {
        if (dim.is_number_integer()) slot.shape.emplace_back(dim.get<std::int64_t>());
        else slot.shape.emplace_back(dim.get<std::string>());
      }
    }
    slot.layoutDigest = binding.layoutDigest(inspection.graph.graphDigest);
    const auto [port, inserted] = result.portsBySlot.try_emplace(
      slot.slotId, std::move(binding));
    if (!inserted && (port->second.sourceName != name || !(port->second.range == range)))
      throw std::invalid_argument("ONNX tensor slot aliases two source operands: " + slot.slotId);
    return slot;
  };
  const auto addSlot = [] (auto& slots, const NativeStepSlotSpec& slot) {
    const auto found = std::find_if(slots.begin(), slots.end(),
      [&] (const auto& existing) { return existing.slotId == slot.slotId; });
    if (found == slots.end()) {
      slots.push_back(slot);
      return true;
    }
    if (found->source != slot.source || found->producerStepId != slot.producerStepId ||
        found->producerSlotId != slot.producerSlotId || found->layoutDigest != slot.layoutDigest ||
        found->dtype != slot.dtype || found->shape != slot.shape)
      throw std::invalid_argument("ONNX tensor plan has ambiguous bindings for " + slot.slotId);
    return false;
  };
  const auto bind = [&] (std::size_t sourceCall, std::size_t consumerCall, NativeStepSlotSpec slot) {
    if (sourceCall == consumerCall) return;
    auto& source = plan.steps.at(sourceCall);
    auto& consumer = plan.steps.at(consumerCall);
    addSlot(source.outputSlots, slot);
    slot.producerStepId = source.stepId;
    slot.producerSlotId = slot.slotId;
    const bool remote = source.unitId != consumer.unitId;
    slot.source = remote ? "REMOTE_STEP" : "LOCAL_STEP";
    if (!addSlot(consumer.inputSlots, slot) || !remote) return;
    auto& dependency = plan.dependencies.emplace_back();
    dependency.dependencyId = "source-tp-" + std::to_string(plan.dependencies.size());
    dependency.keyScope = dependency.dependencyId;
    dependency.topicPrefix = "/NDNSF/DI/ONNX_SOURCE";
    dependency.objectNameTemplate =
      "{producerProvider}/NDNSF/DI/ONNX_SOURCE/{sessionId}/{producerRole}/{consumerRole}/{sequence}";
    dependency.producers = {source.unitId};
    dependency.consumers = {consumer.unitId};
    dependency.operationKind = "ACTIVATION";
    dependency.tensors = {slot.slotId};
    dependency.producerStepId = source.stepId;
    dependency.producerSlotId = slot.slotId;
    dependency.consumerStepId = consumer.stepId;
    dependency.consumerSlotId = slot.slotId;
  };
  for (std::size_t ordinal = 0; ordinal < layouts.size(); ++ordinal) {
    const auto& decision = layouts[ordinal];
    const auto& inputs = nodes.at(ordinal).at("inputs");
    for (std::size_t rank = 0; rank < roles.size(); ++rank) {
      const auto consumer = callByOrdinal[ordinal][rank];
      if (consumer == unavailable) continue;
      for (std::size_t port = 0; port < inputs.size(); ++port) {
        const auto name = inputs.at(port).get<std::string>();
        if (graphInputs.count(name)) {
          auto slot = makeSlot(name, {});
          slot.source = "EPOCH_INPUT";
          addSlot(plan.steps[consumer].inputSlots, slot);
          continue;
        }
        const auto parent = producers.at(name).get<std::size_t>();
        const auto& outputLayouts = layouts.at(parent).outputs;
        const auto local = outputLayouts.find(name);
        if (local == outputLayouts.end() && rank != 0) continue;
        const auto source = callByOrdinal.at(parent)[rank];
        if (source == consumer) continue;
        std::optional<NativeOnnxTransferRange> range;
        if (!decision.concatPieces.empty()) {
          for (const auto& piece : decision.concatPieces) {
            if (piece.input == port && piece.producer == rank && piece.consumer == rank)
              bind(source, consumer, makeSlot(name, NativeOnnxTransferRange{
                decision.outputs.begin()->second.axis, piece.begin, piece.end}));
          }
          continue;
        }
        if (local != outputLayouts.end()) {
          const auto [axis, split] = local->second;
          const auto extent = tensors.at(name).at("shape").at(axis).get<std::uint64_t>();
          range = NativeOnnxTransferRange{axis, rank == 0 ? 0 : split,
                                          rank == 0 ? split : extent};
        }
        bind(source, consumer, makeSlot(name, range));
      }
    }
  }
  for (const auto& transfer : transfers)
    bind(callByOrdinal.at(transfer.producerOrdinal)[transfer.producerRank],
         callByOrdinal.at(transfer.consumerOrdinal)[transfer.consumerRank],
         makeSlot(transfer.tensor.name, transfer.range));
  const auto& port = terminalPorts.front();
  const auto ordinal = producers.at(port).get<std::size_t>();
  if (!layouts.at(ordinal).outputs.empty())
    throw std::invalid_argument("ONNX tensor plan terminal is not owned by the result rank");
  auto& terminal = plan.steps.at(callByOrdinal[ordinal][0]);
  addSlot(terminal.outputSlots, makeSlot(port, {}));
  result.terminalStep = terminal.stepId;
  plan.validateSteps();
  if (std::any_of(plan.steps.begin(), plan.steps.end(), [] (const auto& step) {
        return step.inputSlots.empty() || step.outputSlots.empty();
      }))
    throw std::invalid_argument("ONNX tensor entry has no complete CALL tensor boundary");
  for (const auto& step : plan.steps) {
    const std::size_t rank = step.unitId == roles[0] ? 0 : 1;
    auto& program = result.programs[rank];
    program.entries.push_back(NativeOnnxSourceEntryRecipeV1::fromTensorStep(step));
    for (const auto* ports : {&step.inputSlots, &step.outputSlots}) {
      for (const auto& port : *ports) {
        const auto& binding = result.portsBySlot.at(port.slotId);
        binding.validateSourceTensor(port, makeTransferTensor(tensors, binding.sourceName, {}));
        program.tensorPorts.try_emplace(port.slotId, binding);
      }
    }
  }
  for (std::size_t rank = 0; rank < roles.size(); ++rank) {
    result.programs[rank].tensorLayouts = layouts;
    result.programs[rank].validateRank(plan, roles[rank], rank);
  }
  return result;
}

} // namespace ndnsf::di
