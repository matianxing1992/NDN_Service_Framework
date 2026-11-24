#include "wan-activation-tiles.hpp"

#include "wan-entry-plan.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

namespace ndnsf::di::wan {
namespace {

using Port = NativeAssemblyTensorContractV3;
constexpr std::string_view TILE_PREFIX = "__ndnsf_wan_wire/";
constexpr std::uint64_t MAX_EXACT_BUNDLE_BYTES = 64U << 20;
constexpr std::uint64_t LOCAL_BUNDLE_BYTES = 512U << 20;
constexpr std::size_t MAX_TILES_PER_PORT = 256;

Port
port(const NativeStepSlotSpec& slot)
{
  return {slot.slotId, slot.dtype, slot.shape};
}

struct TileRange
{
  std::size_t axis;
  std::int64_t extent;
  std::int64_t perTile;
};

std::uint64_t
elementBytes(const Port& value)
{
  if (value.dtype == "float32") return sizeof(float);
  if (value.dtype == "int64") return sizeof(std::int64_t);
  if (value.dtype == "bool" || value.dtype == "uint8") return 1;
  throw std::invalid_argument("Wan exact tile has an unsupported element type");
}

std::optional<TileRange>
chooseTiles(const Port& value, std::uint64_t maxBytes)
{
  auto payloadBytes = elementBytes(value);
  std::size_t axis = 0;
  std::int64_t largest = 1;
  for (std::size_t i = 0; i < value.shape.size(); ++i) {
    const auto* dim = std::get_if<std::int64_t>(&value.shape[i]);
    if (!dim || *dim <= 0)
      throw std::invalid_argument("Wan exact tile requires positive fixed dimensions");
    if (static_cast<std::uint64_t>(*dim) >
        std::numeric_limits<std::uint64_t>::max() / payloadBytes)
      throw std::overflow_error("Wan exact tile size overflows");
    payloadBytes *= static_cast<std::uint64_t>(*dim);
    if (*dim > largest) {
      axis = i;
      largest = *dim;
    }
  }
  const auto framing = tensorBundleEncodedSize({{value.name.size(), value.shape.size(), 0}});
  if (framing <= maxBytes && payloadBytes <= maxBytes - framing) return std::nullopt;
  // Every tile name contains its complete range. Budget using the longest ends.
  const auto prefix = std::string(TILE_PREFIX) + value.name + "/" + std::to_string(axis) + "/";
  const auto nameBytes = prefix.size() + 2 * std::to_string(largest).size() + 1;
  const auto tileFraming = tensorBundleEncodedSize({{nameBytes, value.shape.size(), 0}});
  const auto rowBytes = payloadBytes / largest;
  if (largest <= 1 || tileFraming >= maxBytes || rowBytes > maxBytes - tileFraming)
    throw std::invalid_argument("Wan exact tile cannot fit one slice");
  const auto rows = std::min<std::uint64_t>(largest, (maxBytes - tileFraming) / rowBytes);
  if ((static_cast<std::uint64_t>(largest) - 1) / rows >= MAX_TILES_PER_PORT)
    throw std::invalid_argument("Wan exact tile count exceeds the tensor bundle limit");
  return TileRange{axis, largest, static_cast<std::int64_t>(rows)};
}

std::vector<Port>
makeTiles(const Port& value, const TileRange& range)
{
  std::vector<Port> tiles;
  for (std::int64_t begin = 0; begin < range.extent;) {
    const auto end = begin + std::min(range.perTile, range.extent - begin);
    auto tile = value;
    tile.shape[range.axis] = end - begin;
    tile.name = std::string(TILE_PREFIX) + value.name + "/" + std::to_string(range.axis) +
      "/" + std::to_string(begin) + "-" + std::to_string(end);
    if (tiles.size() == MAX_TILES_PER_PORT)
      throw std::invalid_argument("Wan exact tile count exceeds the tensor bundle limit");
    tiles.push_back(std::move(tile));
    begin = end;
  }
  return tiles;
}

void
setAxis(onnx::NodeProto& node, std::size_t axis)
{
  auto& attribute = *node.add_attribute();
  attribute.set_name("axis");
  attribute.set_type(onnx::AttributeProto::INT);
  attribute.set_i(axis);
}

void
replaceCapturedInputs(onnx::NodeProto& node, const std::map<std::string, std::string>& aliases);

void
replaceInSubgraph(onnx::GraphProto& graph, std::map<std::string, std::string> aliases)
{
  for (const auto& input : graph.input()) aliases.erase(input.name());
  for (const auto& initializer : graph.initializer()) aliases.erase(initializer.name());
  for (const auto& node : graph.node())
    for (const auto& output : node.output()) aliases.erase(output);
  if (aliases.empty()) return;
  for (auto& node : *graph.mutable_node()) replaceCapturedInputs(node, aliases);
  for (auto& output : *graph.mutable_output()) {
    const auto found = aliases.find(output.name());
    if (found != aliases.end()) output.set_name(found->second);
  }
}

void
replaceCapturedInputs(onnx::NodeProto& node, const std::map<std::string, std::string>& aliases)
{
  if (aliases.empty()) return;
  for (auto& input : *node.mutable_input()) {
    const auto found = aliases.find(input);
    if (found != aliases.end()) input = found->second;
  }
  for (auto& attribute : *node.mutable_attribute()) {
    if (attribute.has_g()) replaceInSubgraph(*attribute.mutable_g(), aliases);
    for (auto& graph : *attribute.mutable_graphs()) replaceInSubgraph(graph, aliases);
  }
}

} // namespace

TiledActivationLayout
tileWanExactActivations(onnx::ModelProto model, OnnxParallelPlan execution,
                        std::uint64_t maxBundleBytes)
{
  if (maxBundleBytes == 0 || maxBundleBytes > MAX_EXACT_BUNDLE_BYTES ||
      execution.plan.stepPlanVersion != NATIVE_STEP_PLAN_VERSION_V2 ||
      execution.plan.streamingOperationStride != 0)
    throw std::invalid_argument("Wan exact tile wire budget is invalid");
  execution.plan.validateSteps();
  std::map<std::string, Port> ports;
  for (const auto& step : execution.plan.steps) {
    for (const auto& slot : step.outputSlots) ports.emplace(slot.slotId, port(slot));
  }
  std::map<std::string, TileRange> tiled;
  for (const auto& edge : execution.plan.dependencies) {
    if (!edge.useNdnsfDataV1 || edge.producerSlotId != edge.consumerSlotId ||
        edge.tensors != std::vector<std::string>{edge.producerSlotId})
      throw std::invalid_argument("Wan exact tile requires a single-port named dependency");
    if (const auto range = chooseTiles(ports.at(edge.producerSlotId), maxBundleBytes))
      tiled.emplace(edge.producerSlotId, *range);
  }
  // Host tensor views crossing a local CALL boundary use the same per-member
  // size checks as the codec. Keep oversized activations inside ONNX there too.
  for (const auto& [name, value] : ports) {
    if (tiled.count(name) != 0 || std::any_of(value.shape.begin(), value.shape.end(),
          [](const auto& dim) { return !std::holds_alternative<std::int64_t>(dim); }))
      continue;
    if (const auto range = chooseTiles(value, LOCAL_BUNDLE_BYTES))
      tiled.emplace(name, *range);
  }
  if (tiled.empty()) return {std::move(model), std::move(execution)};
  if (std::none_of(model.opset_import().begin(), model.opset_import().end(),
        [](const auto& opset) {
          return (opset.domain().empty() || opset.domain() == "ai.onnx") && opset.version() >= 13;
        }))
    throw std::invalid_argument("Wan exact tiles require the ONNX Split input contract");

  // Every operand and output of a tiled SUM must have the same partition.
  // Traverse to closure before generating any graph nodes.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& step : execution.plan.steps) {
      if (step.operation != "SUM") continue;
      const std::array<std::string, 3> names{
        step.inputSlots.at(0).slotId, step.inputSlots.at(1).slotId,
        step.outputSlots.at(0).slotId};
      const auto first = std::find_if(names.begin(), names.end(), [&](const auto& name) {
        return tiled.count(name) != 0;
      });
      if (first == names.end()) continue;
      auto common = tiled.at(*first);
      for (const auto& name : names) {
        const auto existing = tiled.find(name);
        if (existing != tiled.end())
          common.perTile = std::min(common.perTile, existing->second.perTile);
        const auto range = chooseTiles(ports.at(name), maxBundleBytes);
        if (range) common.perTile = std::min(common.perTile, range->perTile);
      }
      for (const auto& name : names) {
        const auto it = tiled.find(name);
        if (it == tiled.end() || it->second.perTile > common.perTile) {
          tiled.insert_or_assign(name, common);
          changed = true;
        }
      }
    }
  }

  auto& graph = *model.mutable_graph();
  for (const auto& input : graph.input())
    if (input.name().compare(0, TILE_PREFIX.size(), TILE_PREFIX) == 0)
      throw std::invalid_argument("Wan source uses the reserved activation tile prefix");
  for (const auto& tensor : graph.initializer())
    if (tensor.name().compare(0, TILE_PREFIX.size(), TILE_PREFIX) == 0)
      throw std::invalid_argument("Wan source uses the reserved activation tile prefix");
  for (const auto& node : graph.node())
    for (const auto& output : node.output())
      if (output.compare(0, TILE_PREFIX.size(), TILE_PREFIX) == 0)
        throw std::invalid_argument("Wan source uses the reserved activation tile prefix");
  for (const auto& output : graph.output())
    if (tiled.count(output.name()) != 0)
      throw std::invalid_argument("Wan exact tile unexpectedly crosses the application result");
  google::protobuf::RepeatedPtrField<onnx::NodeProto> original;
  graph.mutable_node()->Swap(&original);
  const auto byOutput = [&] {
    std::map<std::string, int> result;
    for (int i = 0; i < original.size(); ++i)
      for (const auto& output : original.Get(i).output())
        if (!output.empty()) result.emplace(output, i);
    return result;
  }();
  OnnxParallelPlan plan;
  EntryPlanBuilder builder(graph, execution.plan.stageLayout, plan.plan, plan.entries);
  if (plan.plan.roles != execution.plan.roles)
    throw std::invalid_argument("Wan exact tile role order differs from its stages");
  plan.plan.version = execution.plan.version;
  plan.plan.serviceName = execution.plan.serviceName;
  plan.plan.modelName = execution.plan.modelName;
  plan.plan.modelFamily = execution.plan.modelFamily;
  plan.plan.modelFormat = execution.plan.modelFormat;
  plan.plan.plannerKind = execution.plan.plannerKind;
  plan.plan.executionPolicy = execution.plan.executionPolicy;
  std::map<std::string, std::vector<Port>> tilesByPort;
  for (const auto& [name, range] : tiled)
    tilesByPort.emplace(name, makeTiles(ports.at(name), range));
  using RolePort = std::pair<std::string, std::string>;
  std::map<RolePort, std::size_t> callsRemaining;
  for (const auto& step : execution.plan.steps) {
    if (step.operation != "CALL_SUBGRAPH") continue;
    for (const auto& slot : step.inputSlots) {
      if (slot.source == "REMOTE_STEP" && tiled.count(slot.slotId) != 0)
        ++callsRemaining[{step.unitId, slot.slotId}];
    }
  }
  std::map<RolePort, std::vector<Port>> receivedTiles;
  std::set<std::string> addedSums;
  std::set<int> usedNodes;
  for (const auto& step : execution.plan.steps) {
    const auto unit = std::distance(execution.plan.roles.begin(), std::find(
      execution.plan.roles.begin(), execution.plan.roles.end(), step.unitId));
    const auto label = step.stepId;
    if (step.operation == "SUM") {
      const auto& left = ports.at(step.inputSlots[0].slotId);
      const auto& right = ports.at(step.inputSlots[1].slotId);
      const auto& output = ports.at(step.outputSlots[0].slotId);
      const auto source = byOutput.at(output.name);
      const bool firstSum = addedSums.insert(output.name).second;
      if (firstSum) {
        const auto& add = original.Get(source);
        if (!usedNodes.insert(source).second || !add.domain().empty() || add.op_type() != "Add" ||
            add.input_size() != 2 || add.output_size() != 1 ||
            add.input(0) != left.name || add.input(1) != right.name)
          throw std::invalid_argument("Wan tile SUM source is not a distinct Add");
        if (!tiled.count(output.name)) *graph.add_node() = original.Get(source);
      }
      if (!tiled.count(output.name)) {
        builder.sum(label, unit, left, right, output);
        continue;
      }
      const auto& lhs = tilesByPort.at(left.name);
      const auto& rhs = tilesByPort.at(right.name);
      const auto& outputs = tilesByPort.at(output.name);
      for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (firstSum) {
          auto& add = *graph.add_node();
          add.set_op_type("Add");
          add.add_input(lhs.at(i).name);
          add.add_input(rhs.at(i).name);
          add.add_output(outputs[i].name);
        }
        builder.sum(label + "/tile-" + std::to_string(i), unit, lhs.at(i), rhs.at(i), outputs[i]);
      }
      continue;
    }

    auto cut = execution.entries.at(step.artifactEntry);
    const auto first = graph.node_size();
    std::map<std::string, std::string> aliases;
    std::vector<Port> inputs, outputs;
    std::size_t callOutputCount = 0;
    for (const auto& output : cut.outputs) {
      const auto found = tilesByPort.find(output.name);
      callOutputCount += found == tilesByPort.end() ? 1 : found->second.size();
    }
    for (const auto& input : cut.inputs) {
      const auto found = tilesByPort.find(input.name);
      if (found == tilesByPort.end()) {
        inputs.push_back(input);
        continue;
      }
      const auto* tiles = &found->second;
      const RolePort key{step.unitId, input.name};
      const auto remaining = callsRemaining.find(key);
      if (remaining != callsRemaining.end()) {
        const auto received = receivedTiles.find(key);
        if (received != receivedTiles.end()) {
          tiles = &received->second;
        }
        else if (--remaining->second != 0 && callOutputCount + tiles->size() <= MAX_TILES_PER_PORT) {
          // The first CALL admits the sender's tiles and exposes local aliases
          // for later CALLs. SUMs retain their canonical operand bindings.
          auto& local = receivedTiles[key];
          for (const auto& tile : *tiles) {
            auto alias = tile;
            alias.name = std::string(TILE_PREFIX) + "local/" + std::to_string(graph.node_size());
            auto& identity = *graph.add_node();
            identity.set_op_type("Identity");
            identity.add_input(tile.name);
            identity.add_output(alias.name);
            local.push_back(alias);
            outputs.push_back(std::move(alias));
          }
          callOutputCount += tiles->size();
        }
      }
      const auto merged = std::string(TILE_PREFIX) + "join/" +
        std::to_string(first) + "/" + input.name;
      aliases.emplace(input.name, merged);
      auto& concat = *graph.add_node();
      concat.set_op_type("Concat");
      setAxis(concat, tiled.at(input.name).axis);
      for (const auto& tile : *tiles) concat.add_input(tile.name);
      concat.add_output(merged);
      inputs.insert(inputs.end(), tiles->begin(), tiles->end());
    }
    std::sort(cut.nodeIndices.begin(), cut.nodeIndices.end());
    for (const auto index : cut.nodeIndices) {
      if (index >= static_cast<std::uint64_t>(original.size()) ||
          !usedNodes.insert(static_cast<int>(index)).second)
        throw std::invalid_argument("Wan exact tile cannot map canonical source nodes");
      auto& node = *graph.add_node();
      node = original.Get(static_cast<int>(index));
      replaceCapturedInputs(node, aliases);
    }
    for (const auto& output : cut.outputs) {
      const auto found = tilesByPort.find(output.name);
      if (found == tilesByPort.end()) {
        outputs.push_back(output);
        continue;
      }
      auto& sizes = *graph.add_initializer();
      sizes.set_name(std::string(TILE_PREFIX) + "sizes/" + output.name);
      sizes.set_data_type(onnx::TensorProto::INT64);
      sizes.add_dims(found->second.size());
      auto& split = *graph.add_node();
      split.set_op_type("Split");
      split.add_input(output.name);
      split.add_input(sizes.name());
      const auto axis = tiled.at(output.name).axis;
      setAxis(split, axis);
      for (const auto& tile : found->second) {
        sizes.add_int64_data(std::get<std::int64_t>(tile.shape[axis]));
        split.add_output(tile.name);
        outputs.push_back(tile);
      }
    }
    if (inputs.size() > MAX_TILES_PER_PORT || outputs.size() > MAX_TILES_PER_PORT)
      throw std::invalid_argument("Wan tiled CALL exceeds the tensor bundle member limit");
    std::vector<std::uint64_t> indices;
    for (auto i = first; i < graph.node_size(); ++i) indices.push_back(i);
    builder.call(label, unit, {std::move(indices), std::move(inputs), std::move(outputs)});
  }
  if (usedNodes.size() != static_cast<std::size_t>(original.size()))
    throw std::invalid_argument("Wan exact tile leaves source nodes unassigned");
  auto* infos = graph.mutable_value_info();
  infos->erase(std::remove_if(infos->begin(), infos->end(), [&](const auto& info) {
    return addedSums.count(info.name()) != 0 && tiled.count(info.name()) != 0;
  }), infos->end());
  plan.plan.validateSteps();
  return {std::move(model), std::move(plan)};
}

} // namespace ndnsf::di::wan
