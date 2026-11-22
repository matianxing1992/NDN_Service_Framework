#include "wan-preparation.hpp"

#include "wan-activation-tiles.hpp"
#include "wan-checkpoint.hpp"
#include "wan-entry-plan.hpp"
#include "wan-umt5.hpp"
#include "wan-vae.hpp"
#include "wan-video-block.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-step-template-json.hpp"

#include <boost/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace ndnsf::di::wan {
namespace {

NativeJson
bindPorts(const std::vector<NativeAssemblyTensorContractV3>& ports,
          const std::vector<NativeStepSlotSpec>& slots, const NativeJson& tensors)
{
  if (ports.size() != slots.size()) throw std::invalid_argument("Wan entry port count disagrees");
  auto bound = NativeJson::array();
  for (std::size_t i = 0; i < ports.size(); ++i) {
    const auto& port = ports[i];
    const auto& slot = slots[i];
    if (port.name != slot.slotId || port.dtype != slot.dtype || port.shape != slot.shape)
      throw std::invalid_argument("Wan entry port differs from its planned slot: " + port.name);
    NativeJson shape = NativeJson::array();
    for (const auto& dim : port.shape)
      std::visit([&] (const auto& value) { shape.push_back(value); }, dim);
    const auto found = tensors.find(port.name);
    if (found != tensors.end()) {
      const auto& dtype = found->at("dtype");
      const auto& actual = found->at("shape");
      if (dtype != "onnx_type_0" && dtype != port.dtype)
        throw std::invalid_argument("Wan entry port differs from source dtype: " + port.name);
      if (!actual.empty() && (actual.size() != shape.size() ||
          !std::equal(actual.begin(), actual.end(), shape.begin(),
            [] (const auto& left, const auto& right) {
              return !left.is_number_integer() || left == right;
            })))
        throw std::invalid_argument("Wan entry port differs from source extent: " + port.name);
    }
    bound.push_back(NativeJson{{"name", port.name}, {"dtype", port.dtype}, {"shape", shape}});
  }
  return bound;
}

} // namespace

CompiledComponent
compileWanComponent(onnx::ModelProto model, OnnxParallelPlan layout,
                    std::uint64_t maxExactBundleBytes, NativeCanonicalSource source,
                    NativeJson catalog, const NativeAssemblyControl& control)
{
  control.requireActive();
  if (!source.modelBytes.empty() || !source.preparedMetadataJson.empty() ||
      source.materialManifest || !source.materialPayloads.empty() || source.materializedRole ||
      !source.layerPayloads.empty() ||
      (source.initializerBytes && source.initializerRangeSource))
    throw std::invalid_argument("Wan component requires a fresh initializer source");
  auto descriptor = NativeModelDescriptor::fromCanonicalJson(nativeCanonicalJson(catalog.at("model")));
  auto& split = catalog.at("splitter");
  if (catalog.at("schema") != "ndnsf-di-native-request-catalog-v1" ||
      split.at("kind") != "ONNX_SOURCE" || descriptor.modelFormat != "onnx" ||
      descriptor.adapterId != "wan")
    throw std::invalid_argument("Wan component requires an ONNX source catalog");
  auto tiled = tileWanExactActivations(std::move(model), std::move(layout), maxExactBundleBytes);
  const auto size = tiled.model.ByteSizeLong();
  if (size == 0 || size > control.maxSourceBytes ||
      size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::invalid_argument("Wan compiled graph exceeds the source bound");
  source.modelBytes.resize(size);
  if (!tiled.model.SerializeToArray(source.modelBytes.data(), static_cast<int>(size)))
    throw std::invalid_argument("Wan compiled graph serialization failed");
  onnx::ModelProto{}.Swap(&tiled.model);
  const auto inspection = inspectNativeOnnxSourceGraph(source, descriptor, control);
  descriptor.graphDigest = inspection.graph.graphDigest;
  catalog["model"] = nativeParseJson(descriptor.canonicalJson());
  auto& sourceConfig = catalog.at("source");
  sourceConfig["digest"] = nativePlanningDigest(source.modelBytes.data(), size);
  sourceConfig["canonical_graph_digest"] = inspection.canonicalIdentity.graphDigest;
  if (source.initializerBytes || source.initializerRangeSource)
    sourceConfig["initializer_digest"] = nativeCanonicalInitializerDigest(source, control);
  else
    sourceConfig.erase("initializer_digest");
  catalog["node_mapping"] = makeWanMaterialNodeMap(inspection);
  catalog.at("publication")["material_bundle_policy"] = "prepared-role-v1";
  for (auto& step : tiled.execution.plan.steps) {
    if (step.operation != "CALL_SUBGRAPH") continue;
    step.onnxNodeIndices = tiled.execution.entries.at(step.artifactEntry).nodeIndices;
    std::sort(step.onnxNodeIndices.begin(), step.onnxNodeIndices.end());
  }
  split["step_template_v2"] = nativeV2StepTemplateJson(tiled.execution.plan);
  split["dependencies"] = nativeDependenciesJson(tiled.execution.plan.dependencies);
  control.requireActive();
  return {std::move(source), nativeCanonicalJson(catalog)};
}

CompiledComponent
compileWanComponent(NativeCanonicalSource source, OnnxParallelPlan layout,
                    std::uint64_t maxExactBundleBytes, NativeJson catalog,
                    const NativeAssemblyControl& control)
{
  control.requireActive();
  const auto size = source.modelBytes.size();
  if (size == 0 || size > control.maxSourceBytes ||
      size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::invalid_argument("Wan lowered graph exceeds the source bound");
  onnx::ModelProto model;
  if (!model.ParseFromArray(source.modelBytes.data(), static_cast<int>(size)))
    throw std::invalid_argument("Wan lowered graph decoding failed");
  std::vector<std::uint8_t>().swap(source.modelBytes);
  return compileWanComponent(std::move(model), std::move(layout), maxExactBundleBytes,
                             std::move(source), std::move(catalog), control);
}

std::filesystem::path
writeWanComponent(const CompiledComponent& component, const std::filesystem::path& directory,
                  const NativeAssemblyControl& control)
{
  if (directory.empty())
    throw std::invalid_argument("Wan source output requires a fresh directory");
  const auto destination = std::filesystem::absolute(directory).lexically_normal();
  if (destination.filename().empty() || std::filesystem::exists(destination))
    throw std::invalid_argument("Wan source output requires a fresh directory");
  control.requireActive();
  auto config = nativeParseJson(component.configurationJson);
  auto& sourceConfig = config.at("source");
  const auto& source = component.source;
  const auto graphBytes = source.modelBytes.size();
  if (graphBytes == 0 || graphBytes > control.maxSourceBytes ||
      sourceConfig.at("digest") != nativePlanningDigest(source.modelBytes.data(), graphBytes))
    throw std::invalid_argument("Wan source graph differs from its compiled catalog");
  const auto locations = inspectNativeOnnxExternalLocations(source.modelBytes, control);
  const auto initializerBytes = nativeCanonicalInitializerByteSize(source);
  if (locations.size() != (initializerBytes != 0 ? 1U : 0U) ||
      initializerBytes > control.maxSourceBytes ||
      sourceConfig.contains("initializer_digest") != (initializerBytes != 0))
    throw std::invalid_argument("Wan source output requires a single bound external object");
  const std::filesystem::path sidecar = locations.empty() ? "" : locations.front();
  if (!sidecar.empty() && (sidecar.has_parent_path() || sidecar == "." || sidecar == ".." ||
                           sidecar == "catalog.json" || sidecar == "model.onnx"))
    throw std::invalid_argument("Wan source sidecar filename is invalid");

  auto pattern = (destination.parent_path() /
                  (destination.filename().string() + "-XXXXXX")).string();
  if (!::mkdtemp(pattern.data()))
    throw std::runtime_error("Wan source output staging directory could not be created");
  const std::filesystem::path staging(pattern);
  bool committed = false;
  BOOST_SCOPE_EXIT_ALL(&) {
    if (!committed) {
      std::error_code cleanup;
      std::filesystem::remove_all(staging, cleanup);
    }
  };
  const auto write = [&] (const auto& name, std::uint64_t total, const auto& writeChunk) {
    std::ofstream output(staging / name, std::ios::binary);
    for (std::uint64_t offset = 0; offset < total;) {
      control.requireActive();
      const auto size = std::min(total - offset, NativeCanonicalMaterialBundleMaxBytes);
      writeChunk(output, offset, size);
      if (!output) throw std::runtime_error("Wan source output could not be written");
      offset += size;
    }
    output.close();
    if (!output) throw std::runtime_error("Wan source output could not be closed");
  };
  write("model.onnx", graphBytes, [&] (auto& output, auto offset, auto size) {
    output.write(reinterpret_cast<const char*>(source.modelBytes.data() + offset), size);
  });
  sourceConfig["file"] = "model.onnx";
  sourceConfig["bytes"] = graphBytes;
  if (initializerBytes != 0) {
    write(sidecar, initializerBytes, [&] (auto& output, auto offset, auto size) {
      const auto bytes = readNativeCanonicalInitializerRange(source, offset, size, control);
      output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    });
    (void)openNativeCanonicalInitializerFile((staging / sidecar).string(), initializerBytes,
      sourceConfig.at("initializer_digest").get<std::string>(), control);
    sourceConfig["initializer_file"] = sidecar.string();
    sourceConfig["initializer_bytes"] = initializerBytes;
  }
  else {
    sourceConfig.erase("initializer_file");
    sourceConfig.erase("initializer_bytes");
  }
  const auto catalog = nativeCanonicalJson(config);
  write("catalog.json", catalog.size(), [&] (auto& output, auto offset, auto size) {
    output.write(catalog.data() + offset, size);
  });
  control.requireActive();
  if (::renameat2(AT_FDCWD, staging.c_str(), AT_FDCWD, destination.c_str(),
                  RENAME_NOREPLACE) != 0)
    throw std::runtime_error("Wan source output directory could not be committed");
  committed = true;
  return destination / "catalog.json";
}

std::filesystem::path
prepareWanCheckpointComponent(NativeJson request, const std::filesystem::path& base,
                              const std::filesystem::path& directory,
                              const NativeAssemblyControl& control)
{
  if (request.at("schema") != "ndnsf-di-wan-checkpoint-preparation-v1" || directory.empty() ||
      std::filesystem::exists(directory) || std::filesystem::is_symlink(directory))
    throw std::invalid_argument("Wan preparation requires a supported request and fresh output");
  auto catalog = std::move(request.at("catalog"));
  const auto& bounds = request.at("geometry");
  const auto kind = request.at("component").get<std::string>();
  if (kind != "umt5" && kind != "dit" && kind != "vae")
    throw std::invalid_argument("unknown Wan checkpoint component: " + kind);
  std::vector<NativeStageLayout> stages;
  for (const auto& data : request.at("stages")) {
    auto& stage = stages.emplace_back(NativeStageLayout{
      data.at("id").get<std::string>(), {}, data.at("roles").get<std::vector<std::string>>()});
    stage.validate();
  }
  if ((stages.size() == 1 && stages.front().degree() > 2) ||
      (stages.size() == 2 && (stages[0].degree() > 2 || stages[1].degree() > 2 ||
                            stages[0].degree() + stages[1].degree() > (kind == "dit" ? 3 : 2))) ||
      stages.empty() || stages.size() > 2)
    throw std::invalid_argument("unsupported Wan component stage layout");
  const auto staged = stages.size() == 2;
  const auto single = !staged && stages.front().degree() == 1;
  const auto pipeline = [&] { return std::array{stages.at(0), stages.at(1)}; };
  const auto tileBytes = request.at("max_exact_bundle_bytes").get<std::uint64_t>();
  const auto& files = request.at("shards");
  if (!files.is_object() || files.empty() || files.size() > 64)
    throw std::invalid_argument("Wan preparation requires a bounded checkpoint shard map");
  WanCheckpointShards shards;
  struct FileIdentity
  {
    std::filesystem::path path;
    std::filesystem::file_time_type modified;
  };
  std::vector<FileIdentity> identities;
  auto guarded = control;
  guarded.requireActive = [&] {
    control.requireActive();
    for (const auto& file : identities) {
      if (std::filesystem::last_write_time(file.path) != file.modified)
        throw std::runtime_error("Wan checkpoint changed during preparation");
    }
  };
  const auto open = [&](const NativeJson& file) {
    const auto path = base / file.at("file").get<std::string>();
    identities.push_back({path, std::filesystem::last_write_time(path)});
    return openNativeCanonicalInitializerFile(path.string(),
      file.at("bytes").get<std::uint64_t>(), file.at("digest").get<std::string>(), guarded);
  };
  std::string index;
  if (request.contains("index")) {
    const auto& file = request.at("index");
    if (file.at("bytes").get<std::uint64_t>() > (4U << 20))
      throw std::invalid_argument("Wan checkpoint index exceeds its bound");
    const auto owner = open(file);
    const auto bytes = owner->read(0, owner->size());
    index.assign(bytes.begin(), bytes.end());
  }
  for (const auto& [label, file] : files.items())
    shards.emplace(label, open(file));
  const WanCheckpoint checkpoint(std::move(shards), index);
  guarded.requireActive();

  auto compiled = [&] {
    if (kind == "umt5") {
      auto encoder = lowerWan21Umt5Encoder(checkpoint, bounds.at("text_tokens"),
                                           bounds.at("score_bytes"));
      const NativeStageLayout ranked{"umt5", {}, {"rank0", "rank1"}};
      auto layout = staged ? makeUmt5PipelinePlan(encoder, bounds.at("prefix_layers"), pipeline())
                           : makeUmt5TensorParallelPlan(encoder, single ? ranked : stages[0]);
      if (single) {
        onnx::ModelProto graph;
        if (encoder.source.modelBytes.size() > std::numeric_limits<int>::max() ||
            !graph.ParseFromArray(encoder.source.modelBytes.data(),
                                  static_cast<int>(encoder.source.modelBytes.size())))
          throw std::invalid_argument("invalid UMT5 baseline source graph");
        layout = makeWanSingleProviderPlan(graph.graph(), layout, stages[0]);
      }
      return compileWanComponent(std::move(encoder.source), std::move(layout), tileBytes,
                                 std::move(catalog), guarded);
    }
    NativeCanonicalSource source;
    if (kind == "dit") {
      const auto& grid = bounds.at("grid");
      const WanDenoiserGeometry geometry{
        {grid.at("frames"), grid.at("height"), grid.at("width"), grid.at("padded_tokens")},
        bounds.at("heads"), bounds.at("text_tokens"), bounds.at("video_key_tile"),
        bounds.at("text_key_tile"), bounds.at("score_bytes"), bounds.at("rotary_bytes")};
      // Staged compilation derives its cuts from the same two-rank graph.
      const NativeStageLayout ranked{"dit", {}, {"rank0", "rank1"}};
      auto denoiser = lowerWan21Denoiser(checkpoint, geometry, (staged || single) ? ranked : stages[0]);
      auto layout = staged ? makeVideoDenoiserStagedPlan(denoiser.layout,
                                    bounds.at("prefix_blocks"), pipeline())
                           : std::move(denoiser.layout.tensorParallel);
      if (single)
        layout = makeWanSingleProviderPlan(denoiser.layout.model.graph(), layout, stages[0]);
      source.initializerRangeSource = std::move(denoiser.initializerRangeSource);
      return compileWanComponent(std::move(denoiser.layout.model), std::move(layout), tileBytes,
                                 std::move(source), std::move(catalog), guarded);
    }
    const VaeConvTile tile{1, bounds.at("height"), bounds.at("width")};
    const auto mean = bounds.at("latent_mean").get<std::vector<float>>();
    const auto inverseStd = bounds.at("latent_inverse_std").get<std::vector<float>>();
    const auto& head = checkpoint.parameters().at("decoder.head.2.weight");
    if (head.dims_size() != 5 || (head.dims(0) != 3 && head.dims(0) != 12))
      throw std::invalid_argument("unsupported Wan VAE checkpoint output projection");
    const auto lowerDecoder = head.dims(0) == 12 ? lowerWan22VaeClip : lowerWan21VaeClip;
    auto decoder = lowerDecoder(checkpoint, tile, mean, inverseStd,
      bounds.at("latent_frames"), bounds.at("key_tile"), bounds.at("score_bytes"),
      catalog.at("recipe").at("max_nodes"));
    const NativeStageLayout ranked{"vae", {}, {"rank0", "rank1"}};
    auto layout = staged ? makeVaePipelinePlan(decoder.layout, pipeline())
                         : makeVaeTensorParallelPlan(decoder.layout, single ? ranked : stages[0]);
    if (single) layout = makeWanSingleProviderPlan(decoder.layout.model.graph(), layout, stages[0]);
    source.initializerRangeSource = std::move(decoder.sidecar);
    return compileWanComponent(std::move(decoder.layout.model), std::move(layout), tileBytes,
                               std::move(source), std::move(catalog), guarded);
  }();
  const auto loaded = NativeRequestCatalog::load(compiled.configurationJson, compiled.source,
                                                 guarded);
  if (!loaded.planningCache || loaded.planningCache->candidates.size() != 1)
    throw std::invalid_argument("Wan preparation requires one admitted layout candidate");
  auto configuration = nativeParseJson(compiled.configurationJson);
  const auto& source = configuration.at("source");
  configuration["preparation_facts"] = {
    {"schema", "ndnsf-di-wan-preparation-facts-v1"},
    {"candidate_digest", loaded.planningCache->candidates.front().candidate.candidateDigest},
    {"source_digest", source.at("digest")},
    {"initializer_digest", source.value("initializer_digest", std::string{})}};
  compiled.configurationJson = nativeCanonicalJson(configuration);
  return writeWanComponent(compiled, directory, guarded);
}

NativeCanonicalRolePreparer::NodeMap
makeWanMaterialNodeMap(const NativeOnnxGraphInspection& source)
{
  NativeCanonicalRolePreparer::NodeMap result;
  std::set<std::uint64_t> indices;
  for (const auto& node : source.graph.nodes) {
    const auto index = source.canonicalNodeIndices.at(node.id);
    if (index >= source.nodeNames.size() || !indices.insert(index).second ||
        !result.emplace(node.id, std::vector<std::uint64_t>{index}).second)
      throw std::invalid_argument("Wan canonical mapping is not one-to-one");
  }
  if (result.empty() || result.size() != source.nodeNames.size())
    throw std::invalid_argument("Wan canonical mapping is incomplete");
  return result;
}

NativeSplitCandidate
makeWanSplitCandidate(const NativeInspectedModel& model, const NativeOnnxGraphInspection& source,
                      const OnnxParallelPlan& layout, const std::string& serviceName,
                      std::map<std::string, NativeRoleResourceRequirement> resources,
                      const NativeRequestControl& control)
{
  control.requireActive();
  model.validate();
  layout.plan.validateSteps();
  if (source.graph.graphDigest != model.graph.graphDigest ||
      source.canonicalIdentity.graphDigest != model.canonicalGraphDigest ||
      (!model.canonicalInitializerDigest.empty() &&
       source.canonicalIdentity.initializerDigest != model.canonicalInitializerDigest) ||
      serviceName.empty() || layout.plan.stageLayout.empty())
    throw std::invalid_argument("Wan split source is not bound to the inspected model");
  const auto mapping = makeWanMaterialNodeMap(source);
  std::vector<std::string> nodeIds(mapping.size());
  for (const auto& [id, indices] : mapping) nodeIds.at(indices.front()) = id;
  const auto metadata = nativeParseJson(source.graphMetadataJson);
  const auto& nodes = metadata.at("nodes");
  const auto& tensors = metadata.at("tensors");
  const auto& producers = metadata.at("tensorProducers");
  const auto& consumers = metadata.at("tensorConsumers");
  const auto graphInputs = metadata.at("inputs").get<std::set<std::string>>();
  const auto graphOutputs = metadata.at("outputs").get<std::set<std::string>>();
  if (nodes.size() != nodeIds.size())
    throw std::invalid_argument("Wan inspected node metadata is incomplete");
  if (graphOutputs.size() != 1)
    throw std::invalid_argument("Wan component must declare exactly one graph output");

  NativeSplitCandidate result;
  result.source = "GENERATED";
  result.model = model.descriptor;
  result.graphDigest = model.graph.graphDigest;
  result.executionPlan = layout.plan;
  result.executionPlan.serviceName = serviceName;
  result.executionPlan.modelName = model.descriptor.modelName;
  result.executionPlan.modelFamily = "wan";
  result.executionPlan.modelFormat = model.descriptor.modelFormat;
  result.executionPlan.plannerKind = "native-wan-canonical";
  result.requirementsByRole = std::move(resources);
  std::map<std::string, NativeJson> roleEntries;
  std::map<std::string, std::set<std::uint64_t>> roleNodes;
  std::set<std::string> usedEntries;
  std::set<std::uint64_t> sumNodes;
  std::set<std::pair<std::string, std::string>> consumedSlots;
  for (const auto& edge : result.executionPlan.dependencies) {
    if (!edge.useNdnsfDataV1 || edge.producers.size() != 1 || edge.consumers.size() != 1 ||
        edge.producerStepId.empty() || edge.consumerStepId.empty() ||
        !edge.redistributions.empty() || edge.producerSlotId != edge.consumerSlotId ||
        edge.tensors != std::vector<std::string>{edge.producerSlotId})
      throw std::invalid_argument("Wan entry edge must deliver its exact canonical port");
  }
  for (auto& step : result.executionPlan.steps) {
    control.requireActive();
    for (const auto& slot : step.inputSlots) {
      if (slot.source == "EPOCH_INPUT" ? graphInputs.count(slot.slotId) == 0 :
          slot.producerSlotId != slot.slotId)
        throw std::invalid_argument("Wan step input does not bind its canonical producer");
      if (!slot.producerStepId.empty())
        consumedSlots.emplace(slot.producerStepId, slot.producerSlotId);
    }
    if (step.operation == "SUM") {
      const auto node = producers.at(step.outputSlots.at(0).slotId).get<std::uint64_t>();
      const auto& facts = nodes.at(node);
      if (step.inputSlots.size() != 2 || step.outputSlots.size() != 1 ||
          facts.at("opType") != "Add" || facts.at("inputs").size() != 2 ||
          facts.at("inputs").at(0) != step.inputSlots[0].slotId ||
          facts.at("inputs").at(1) != step.inputSlots[1].slotId)
        throw std::invalid_argument("Wan SUM does not bind a canonical Add");
      if (sumNodes.insert(node).second && !result.nodeRoles.emplace(nodeIds.at(node), step.unitId).second)
        throw std::invalid_argument("Wan SUM overlaps a physical entry");
      continue;
    }
    const auto& cut = layout.entries.at(step.artifactEntry);
    const std::set<std::uint64_t> owned(cut.nodeIndices.begin(), cut.nodeIndices.end());
    if (owned.empty() || owned.size() != cut.nodeIndices.size() || cut.inputs.empty() ||
        cut.outputs.empty() || !usedEntries.insert(step.artifactEntry).second)
      throw std::invalid_argument("Wan split has an empty or repeated physical entry");
    step.onnxNodeIndices.assign(owned.begin(), owned.end());
    const auto inputs = bindPorts(cut.inputs, step.inputSlots, tensors);
    const auto outputs = bindPorts(cut.outputs, step.outputSlots, tensors);
    std::set<std::string> needed, exported;
    for (const auto node : owned) {
      control.requireActive();
      if (!result.nodeRoles.emplace(nodeIds.at(node), step.unitId).second)
        throw std::invalid_argument("Wan split has overlapping node ownership");
      const auto& facts = nodes.at(node);
      for (const auto& input : facts.at("inputs")) {
        const auto name = input.get<std::string>();
        const auto upstream = producers.find(name);
        if (upstream == producers.end() || owned.count(upstream->get<std::uint64_t>()) == 0)
          needed.insert(name);
      }
      for (const auto& output : facts.at("outputs")) {
        const auto name = output.get<std::string>();
        const auto users = consumers.find(name);
        if (graphOutputs.count(name) != 0 || (users != consumers.end() &&
            std::any_of(users->begin(), users->end(), [&] (const auto& consumer) {
              return owned.count(consumer.template get<std::uint64_t>()) == 0;
            })))
          exported.insert(name);
      }
    }
    for (const auto& input : cut.inputs) {
      if (needed.erase(input.name) == 0)
        throw std::invalid_argument("Wan split entry has an unused canonical input: " + input.name);
    }
    for (const auto& output : cut.outputs) {
      if (owned.count(producers.at(output.name).get<std::uint64_t>()) == 0)
        throw std::invalid_argument("Wan split exports output from a foreign entry");
      exported.erase(output.name);
      if (graphOutputs.count(output.name) != 0) {
        if (!result.resultEgressRole.empty() && result.resultEgressRole != step.unitId)
          throw std::invalid_argument("Wan result has more than one egress role");
        result.resultEgressRole = step.unitId;
      }
    }
    if (!needed.empty() || !exported.empty())
      throw std::invalid_argument("Wan split omits a canonical entry dependency: " + step.artifactEntry);
    roleNodes[step.unitId].insert(owned.begin(), owned.end());
    roleEntries[step.unitId].push_back(NativeJson{{"id", step.artifactEntry},
      {"nodes", cut.nodeIndices}, {"inputs", inputs}, {"outputs", outputs}});
  }
  if (usedEntries.size() != layout.entries.size() || result.resultEgressRole.empty())
    throw std::invalid_argument("Wan split has unused entries or no egress");
  std::size_t terminalSlots = 0;
  for (const auto& step : result.executionPlan.steps) {
    for (const auto& slot : step.outputSlots) {
      if (consumedSlots.count({step.stepId, slot.slotId}) != 0) continue;
      if (step.unitId != result.resultEgressRole || graphOutputs.count(slot.slotId) != 1)
        throw std::invalid_argument("Wan terminal slot does not bind the graph output");
      ++terminalSlots;
    }
  }
  if (terminalSlots != 1)
    throw std::invalid_argument("Wan component must have exactly one terminal slot");
  result.inputIngressRole = layout.plan.roles.at(0);
  for (const auto& stage : layout.plan.stageLayout) {
    for (std::size_t rank = 0; rank < stage.unitRoleIds.size(); ++rank) {
      const auto& role = stage.unitRoleIds[rank];
      if (stage.degree() > 1)
        result.tensorIdentityByRole.emplace(role, NativeSplitCandidate::RoleTensorIdentity{
          stage.degree(), rank, {}});
      auto& ids = result.roleNodeIdsByRole[role];
      for (const auto index : roleNodes.at(role)) ids.push_back(nodeIds[index]);
      const auto digest = nativePlanningDigest(nativeCanonicalJson(NativeJson{
        {"graph", model.canonicalGraphDigest}, {"initializers", source.canonicalIdentity.initializerDigest},
        {"entries", roleEntries.at(role)}}));
      result.fragmentsByRole[role] = digest;
      result.artifactsByRole[role] = {digest};
    }
  }
  std::set<std::string> cuts;
  for (const auto& edge : result.executionPlan.dependencies)
    cuts.insert(edge.tensors.begin(), edge.tensors.end());
  result.crossPartitionTensors.assign(cuts.begin(), cuts.end());
  result.splitter = {"native-wan-canonical", "1",
    nativePlanningDigest(nativeCanonicalJson(NativeJson(result.fragmentsByRole)))};
  result.candidateDigest = result.computedDigest();
  result.validate(model.graph);
  control.requireActive();
  return result;
}

NativeWanSplitStrategy::NativeWanSplitStrategy(NativeSplitCandidate candidate)
  : m_candidate(std::move(candidate))
{
  if (m_candidate.splitter.name != "native-wan-canonical")
    throw std::invalid_argument("Wan splitter requires an admitted Wan candidate");
}

NativeStrategyIdentity
NativeWanSplitStrategy::identity() const
{
  return m_candidate.splitter;
}

std::vector<NativeSplitCandidate>
NativeWanSplitStrategy::enumerate(const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
                                 const NativeCandidateBudget& budget) const
{
  return enumerate(model, graph, budget, {std::chrono::steady_clock::time_point::max(), {}});
}

std::vector<NativeSplitCandidate>
NativeWanSplitStrategy::enumerate(const NativeModelDescriptor& model, const NativeGraphSnapshot& graph,
                                 const NativeCandidateBudget& budget,
                                 const ExtensionControl& control) const
{
  control.requireActive();
  budget.validate();
  if (m_candidate.model.canonicalJson() != model.canonicalJson())
    throw std::invalid_argument("Wan candidate is bound to a different model");
  m_candidate.validate(graph);
  control.requireActive();
  return {m_candidate};
}

} // namespace ndnsf::di::wan
