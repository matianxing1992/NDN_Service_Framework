// Compare representative real YOLO26n shards to an uncut CPU ONNX oracle.
// This host-side qualification probe instruments only the graph outputs.
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-rank-transfers.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-onnx-source-transform.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <onnx/shape_inference/implementation.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <vector>

int
checkNativeYoloTpCheckpoints(const std::filesystem::path& catalogPath,
                             const std::filesystem::path& modelPath,
                             const std::filesystem::path& weightsPath,
                             const std::filesystem::path& inputPath,
                             const std::filesystem::path& root)
{
  using namespace ndnsf::di;
  std::ifstream catalogFile(catalogPath);
  const std::string wire((std::istreambuf_iterator<char>(catalogFile)), {});
  const auto catalog = nativeParseJson(wire);
  std::ifstream modelFile(modelPath, std::ios::binary);
  NativeCanonicalSource source;
  source.modelBytes = std::vector<std::uint8_t>((std::istreambuf_iterator<char>(modelFile)), {});
  const auto& metadata = catalog.at("source");
  const auto sourceDigest =
    nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  const std::string graphDigest =
    "sha256:671501236cc976cc526334bb1035bb544d16ddf7d9c77c55317dd31329900520";
  const std::string weightDigest =
    "sha256:f0820471225a9492718ae3cf637f34304e14734567d6ec698c6854642c0ed09b";
  if (sourceDigest != graphDigest || sourceDigest != metadata.at("digest") ||
      weightDigest != metadata.at("initializer_digest"))
    throw std::runtime_error("wrong source bytes");
  const NativeAssemblyControl bounds{std::chrono::steady_clock::now() + std::chrono::seconds(180),
    [] {}, 1U << 27, 1U << 28};
  source.initializerRangeSource = openNativeCanonicalInitializerFile(
    weightsPath, std::filesystem::file_size(weightsPath), weightDigest, bounds);
  const auto model =
    NativeModelDescriptor::fromCanonicalJson(nativeCanonicalJson(catalog.at("model")));
  std::vector<NativeOnnxNodeLayout> layouts;
  const auto inspection = inspectNativeOnnxTensorParallelGraph(source, model, bounds, layouts);
  const auto result = buildNativeOnnxTensorStepPlan(inspection, layouts,
    {"Rank0", "Rank1"}, model.modelName);
  const auto& plan = result.plan;
  namespace fs = std::filesystem;
  if (!fs::create_directory(root))
    throw std::runtime_error("checkpoint work directory already exists");
  NativeCanonicalSource material;
  material.materialManifest = deriveNativeCanonicalMaterialManifest(source, bounds);
  material.materialPayloads = material.materialManifest->payloads;
  std::ifstream inputFile(inputPath, std::ios::binary);
  NamedTensor frozen{"images", TensorElementType::Float32, {1, 3, 640, 640},
                     std::vector<std::uint8_t>((std::istreambuf_iterator<char>(inputFile)), {})};
  if (frozen.payload.size() != 3 * 640 * 640 * sizeof(float))
    throw std::runtime_error("invalid YOLO26n RGB input size");
  const auto inputDigest = nativePlanningDigest(frozen.payload.data(), frozen.payload.size());
  if (inputDigest != "sha256:830eadd834b9305b1472dcb60b1c7fffea53b0f0e12012fc36dc9f920fd56f2a" &&
      inputDigest != "sha256:fa805116e02853d811469067239505cfd7d020a278d1b795ea8f249844270bf5")
    throw std::runtime_error("input differs from the frozen YOLO26n cases");
  std::map<std::string, NamedTensor> outputs;
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "spec263");
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetIntraOpNumThreads(1).SetInterOpNumThreads(1);
  const auto cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  const auto run = [&] (const fs::path& path, const std::vector<const char*>& names,
                       const std::vector<NamedTensor*>& data,
                       const std::vector<const char*>& requested) {
    Ort::Session session(env, path.c_str(), sessionOptions);
    std::vector<Ort::Value> values;
    for (auto* tensor : data) {
      const auto type = tensor->elementType == TensorElementType::Float32
        ? ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT : ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
      values.push_back(Ort::Value::CreateTensor(cpu, tensor->payload.data(),
        tensor->payload.size(), tensor->shape.data(), tensor->shape.size(), type));
    }
    return session.Run(Ort::RunOptions{nullptr}, names.data(), values.data(),
                       values.size(), requested.data(), requested.size());
  };
  // Check cut boundaries in the backbone, both attention branches and the detection head.
  constexpr std::size_t CHECKPOINT_AXIS = 1;
  struct Checkpoint
  {
    explicit Checkpoint(std::size_t sourceOrdinal)
      : ordinal(sourceOrdinal)
    {
    }

    std::size_t ordinal;
    std::string sourceName;
    std::string sourceNode;
    std::array<bool, 2> covered{};
    std::array<std::size_t, 2> counts{};
    double maxDiff = 0;
  };
  std::array checkpoints{Checkpoint{105}, Checkpoint{184}, Checkpoint{191}, Checkpoint{385},
                         Checkpoint{433}};
  onnx::ModelProto uncut;
  if (!uncut.ParseFromArray(source.modelBytes.data(), source.modelBytes.size()))
    throw std::runtime_error("cannot parse reference ONNX");
  onnx::shape_inference::InferShapes(uncut);
  auto& graph = *uncut.mutable_graph();
  for (auto& cp : checkpoints) {
    cp.sourceName = graph.node(cp.ordinal).output(0);
    cp.sourceNode = graph.node(cp.ordinal).name();
    const auto info = std::find_if(graph.value_info().begin(), graph.value_info().end(),
      [&] (const auto& value) { return value.name() == cp.sourceName; });
    if (info == graph.value_info().end()) throw std::runtime_error("missing uncut checkpoint");
    const auto value = *info;
    *graph.add_output() = value;
  }
  const auto refPath = root / "checkpoint-reference.onnx";
  fs::copy_file(fs::absolute(weightsPath), root / "yolo26n.weights");
  std::ofstream referenceFile(refPath, std::ios::binary);
  if (!uncut.SerializeToOstream(&referenceFile)) throw std::runtime_error("cannot emit reference");
  referenceFile.close();
  std::vector<const char*> referenceNames{"predictions"};
  for (const auto& cp : checkpoints) referenceNames.push_back(cp.sourceName.c_str());
  auto reference = run(refPath, {"images"}, {&frozen}, referenceNames);
  for (std::size_t i = 0; i < checkpoints.size(); ++i) {
    const auto info = reference[i + 1].GetTensorTypeAndShapeInfo();
    if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
      throw std::runtime_error("non-float oracle checkpoint");
    const auto dim = info.GetShape().at(CHECKPOINT_AXIS);
    if (dim < 2 || dim % 2 != 0) throw std::runtime_error("unexpected checkpoint extent");

  }
  std::size_t callCount = 0;
  std::array<std::size_t, 2> entryIndex{};
  for (const auto& step : plan.steps) {
    const std::size_t rank = step.unitId == plan.roles.front() ? 0 : 1;
    const auto& program = result.programs[rank];
    const auto& entry = program.entries.at(entryIndex[rank]++);
    std::vector<std::uint64_t> ordinals;
    for (auto ordinal = step.sourceLayers.first; ordinal < step.sourceLayers.second; ++ordinal)
      ordinals.push_back(ordinal);
    std::vector<std::uint8_t> rawWeights;
    const auto raw = materializeNativeOnnxEntrySource(material, entry, ordinals, step, bounds,
      [&] (const auto& bytes) { rawWeights.insert(rawWeights.end(), bytes.begin(), bytes.end()); },
      &program);
    onnx::ModelProto original;
    if (!original.ParseFromArray(raw.modelBytes.data(), static_cast<int>(raw.modelBytes.size())))
      throw std::runtime_error("invalid materialized graph");
    const auto folder = root / std::to_string(callCount);
    fs::create_directory(folder);
    std::ofstream emittedWeights(folder / "model.onnx.data", std::ios::binary);
    auto graph = lowerNativeOnnxTensorEntry(std::move(original), program, step,
      rank, bounds, [&] (const auto&, auto offset, auto count) {
        if (offset > rawWeights.size() || count > rawWeights.size() - offset)
          throw std::runtime_error("bad initializer read");
        return std::vector<std::uint8_t>(rawWeights.begin() + offset,
                                         rawWeights.begin() + offset + count);
      }, [&] (const auto& bytes) {
        if (!emittedWeights.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
          throw std::runtime_error("could not write output initializer");
      });
    emittedWeights.close();
    std::vector<std::pair<std::size_t, std::string>> probes;
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
      const auto& cp = checkpoints[i];
      const auto& body = graph.graph().node();
      const auto owner = std::find_if(body.begin(), body.end(),
        [&] (const auto& node) { return node.name() == cp.sourceNode; });
      if (owner == body.end()) continue;
      const auto physical = owner->output(0);
      const auto shape = reference[i + 1].GetTensorTypeAndShapeInfo().GetShape();
      auto& output = *graph.mutable_graph()->add_output();
      output.set_name(physical);
      auto& tensor = *output.mutable_type()->mutable_tensor_type();
      tensor.set_elem_type(onnx::TensorProto::FLOAT);
      for (std::size_t axis = 0; axis < shape.size(); ++axis) {
        const auto extent = axis == CHECKPOINT_AXIS ? shape[axis] / 2 : shape[axis];
        tensor.mutable_shape()->add_dim()->set_dim_value(extent);
      }
      probes.emplace_back(i, physical);
    }
    const auto path = folder / "model.onnx";
    std::ofstream file(path, std::ios::binary);
    if (!graph.SerializeToOstream(&file)) throw std::runtime_error("cannot emit graph");
    file.close();
    std::vector<const char*> names;
    std::vector<NamedTensor*> tensors;
    for (const auto& port : step.inputSlots) {
      names.push_back(port.slotId.c_str());
      tensors.push_back(port.source == "EPOCH_INPUT" ? &frozen :
        &outputs.at(port.producerStepId + "\n" + port.producerSlotId));
    }
    std::vector<const char*> wanted;
    for (const auto& port : step.outputSlots) wanted.push_back(port.slotId.c_str());
    for (const auto& probe : probes) wanted.push_back(probe.second.c_str());
    auto values = run(path, names, tensors, wanted);
    for (std::size_t index = 0; index < probes.size(); ++index) {
      auto& cp = checkpoints[probes[index].first];
      const auto& ref = reference[probes[index].first + 1];
      const auto& value = values.at(step.outputSlots.size() + index);
      const auto info = value.GetTensorTypeAndShapeInfo();
      const auto refShape = ref.GetTensorTypeAndShapeInfo().GetShape();
      const auto extent = static_cast<std::size_t>(refShape.at(CHECKPOINT_AXIS));
      const auto width = extent / 2;
      auto expectedShape = refShape;
      expectedShape.at(CHECKPOINT_AXIS) = width;
      if (info.GetShape() != expectedShape ||
          info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || cp.covered[rank])
        throw std::runtime_error("unexpected checkpoint shape, type or duplicate owner");
      std::size_t stride = 1;
      for (auto axis = CHECKPOINT_AXIS + 1; axis < refShape.size(); ++axis)
        stride *= refShape[axis];
      const auto* actual = value.GetTensorData<float>();
      const auto* expected = ref.GetTensorData<float>();
      for (std::size_t j = 0; j < info.GetElementCount(); ++j) {
        const auto sourceIndex = j / (width * stride) * extent * stride +
                                 rank * width * stride + j % (width * stride);
        const auto difference = std::abs(double(actual[j]) - expected[sourceIndex]);
        if (!std::isfinite(difference) ||
            difference > 1e-4 + 1e-4 * std::abs(expected[sourceIndex]))
          throw std::runtime_error("checkpoint numerical mismatch at " + cp.sourceName);
        cp.maxDiff = std::max(cp.maxDiff, difference);
      }
      cp.covered[rank] = true;
      cp.counts[rank] = info.GetElementCount();
    }
    for (std::size_t index = 0; index < step.outputSlots.size(); ++index) {
      const auto& value = values[index];
      const auto& port = step.outputSlots[index];
      const auto info = value.GetTensorTypeAndShapeInfo();
      const auto type = port.dtype == "float32" ? ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT :
                                                 ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
      const auto shape = info.GetShape();
      std::vector<std::int64_t> expected;
      for (const auto& dim : port.shape) expected.push_back(std::get<std::int64_t>(dim));
      if (shape != expected || info.GetElementType() != type)
        throw std::runtime_error("ORT output does not match physical slot");
      const auto* first = static_cast<const std::uint8_t*>(value.GetTensorRawData());
      const auto bytes = info.GetElementCount() * (port.dtype == "float32" ? 4 : 8);
      outputs.emplace(step.stepId + "\n" + port.slotId, NamedTensor{port.slotId,
        port.dtype == "float32" ? TensorElementType::Float32 : TensorElementType::Int64,
        shape, std::vector<std::uint8_t>(first, first + bytes)});
    }
    ++callCount;
  }
  const auto& terminal = outputs.at(result.terminalStep + "\npredictions");
  const auto info = reference.front().GetTensorTypeAndShapeInfo();
  if (info.GetShape() != terminal.shape ||
      info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
    throw std::runtime_error("uncut terminal type or shape mismatch");
  const auto* full = reference.front().GetTensorData<float>();
  double maxDiff = 0.0;
  double squared = 0.0;
  std::size_t violations = 0;
  for (std::size_t i = 0; i < info.GetElementCount(); ++i) {
    float actual;
    std::memcpy(&actual, terminal.payload.data() + i * sizeof(float), sizeof(float));
    if (!std::isfinite(actual) || !std::isfinite(full[i]))
      throw std::runtime_error("nonfinite output");
    const auto diff = std::abs(static_cast<double>(actual) - full[i]);
    maxDiff = std::max(maxDiff, diff);
    squared += diff * diff;
    if (diff > 1e-4 + 1e-4 * std::abs(full[i])) ++violations;
  }
  auto checkpointReport = NativeJson::array();
  for (const auto& cp : checkpoints) {
    if (!cp.covered[0] || !cp.covered[1])
      throw std::runtime_error("checkpoint rank coverage incomplete at " + cp.sourceName);
    checkpointReport.push_back({{"ordinal", cp.ordinal}, {"source", cp.sourceName},
                                {"compared", cp.counts}, {"maxAbs", cp.maxDiff}});
  }
  if (violations != 0 || info.GetElementCount() != 1800 ||
      entryIndex != std::array<std::size_t, 2>{162, 119})
    throw std::runtime_error("terminal reference mismatch or incomplete rank execution");
  std::cout << nativeCanonicalJson(NativeJson{{"checkpoints", checkpointReport},
    {"calls", entryIndex}, {"elements", info.GetElementCount()},
    {"graphDigest", graphDigest}, {"weightDigest", weightDigest}, {"inputDigest", inputDigest},
    {"maxAbs", maxDiff}, {"rmse", std::sqrt(squared / info.GetElementCount())}}) << '\n';
  return 0;
}
