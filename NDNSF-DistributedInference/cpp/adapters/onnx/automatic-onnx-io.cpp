#include "automatic-onnx-io.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace ndnsf::di {
namespace {

constexpr std::size_t MAX_IO_NAMES = 256;
constexpr std::size_t MAX_IO_NAME_BYTES = 1024;
constexpr const char* IO_SCHEMA = "ndnsf-di-automatic-onnx-io-v1";
constexpr const char* LEGACY_KEYS[] = {
  "inputNames", "input_names", "input_tensors", "input_tensor",
  "outputNames", "output_names", "output_tensors", "output_tensor",
  "stateInputNames", "state_input_names", "stateInputTensors", "state_input_tensors",
  "stateOutputNames", "state_output_names", "stateOutputTensors", "state_output_tensors",
  "unrankedInputNames", "unranked_input_names", "passthroughTensors", "passthrough_tensors",
  "kvOutputTensors", "kv_output_tensors",
  "stateSuccessorMap", "state_successor_map", "kvTensorMap", "kv_tensor_map",
  "positionInputPolicy", "position_input_policy",
  "attentionMaskInputName", "attention_mask_input_name",
  "positionIdsInputName", "position_ids_input_name",
  "cachePositionInputName", "cache_position_input_name"
};

[[noreturn]] void
rejectIo(const char* reason)
{
  throw std::invalid_argument(reason);
}

void
validateNames(const std::vector<std::string>& names)
{
  if (names.size() > MAX_IO_NAMES)
    rejectIo("AUTOMATIC_IO_NAME_COUNT");
  std::set<std::string> unique;
  std::vector<TensorBundleEncodingMember> framing;
  framing.reserve(names.size());
  for (const auto& name : names) {
    if (name.empty() || name.size() > MAX_IO_NAME_BYTES ||
        name.find('\0') != std::string::npos || !unique.insert(name).second)
      rejectIo("AUTOMATIC_IO_NAME");
    framing.push_back({name.size(), 0, 0});
  }
  if (!framing.empty())
    (void)tensorBundleEncodedSize(framing);
}

bool
contains(const std::vector<std::string>& names, const std::string& name)
{
  return std::find(names.begin(), names.end(), name) != names.end();
}

void
bindPairs(StatefulOnnxIoContractV1& io)
{
  if (io.stateInputNames.size() != io.stateOutputNames.size())
    rejectIo("AUTOMATIC_IO_PAIR_COUNT");
  io.stateFamilies.clear();
  io.successorInputByOutput.clear();
  for (std::size_t index = 0; index < io.stateInputNames.size(); ++index)
    io.successorInputByOutput.emplace(io.stateOutputNames[index], io.stateInputNames[index]);
}

} // namespace

AutomaticOnnxIoContract::AutomaticOnnxIoContract()
{
  io.stateFamilies.clear();
}

void
AutomaticOnnxIoContract::validate() const
{
  for (const auto* names : {&io.inputNames, &io.outputNames, &io.stateInputNames,
                           &io.stateOutputNames, &unrankedInputNames, &passthroughTensors})
    validateNames(*names);
  if (!io.stateFamilies.empty())
    rejectIo("AUTOMATIC_IO_LEGACY_FAMILIES");
  io.validate();
  for (std::size_t index = 0; index < io.stateInputNames.size(); ++index) {
    if (!contains(io.inputNames, io.stateInputNames[index]) ||
        !contains(io.outputNames, io.stateOutputNames[index]) ||
        io.stateInputForOutput(io.stateOutputNames[index]) != io.stateInputNames[index])
      rejectIo("AUTOMATIC_IO_PAIR_BOUNDARY");
  }
  for (const auto& name : unrankedInputNames) {
    if (!contains(io.inputNames, name))
      rejectIo("AUTOMATIC_IO_UNRANKED_BOUNDARY");
  }
  for (const auto& name : passthroughTensors) {
    if (!contains(io.inputNames, name) || contains(io.stateInputNames, name))
      rejectIo("AUTOMATIC_IO_PASSTHROUGH_BOUNDARY");
  }
}

std::string
AutomaticOnnxIoContract::canonicalJson() const
{
  validate();
  const auto encoded = nativeCanonicalJson(NativeJson{
    {"schema", IO_SCHEMA}, {"inputNames", io.inputNames}, {"outputNames", io.outputNames},
    {"stateInputNames", io.stateInputNames}, {"stateOutputNames", io.stateOutputNames},
    {"unrankedInputNames", unrankedInputNames}, {"passthroughTensors", passthroughTensors}});
  if (encoded.size() > NATIVE_AUTOMATIC_ONNX_IO_MAX_BYTES)
    rejectIo("AUTOMATIC_IO_LIMIT");
  return encoded;
}

AutomaticOnnxIoContract
AutomaticOnnxIoContract::fromCanonicalJson(const std::string& encoded)
{
  if (encoded.size() > NATIVE_AUTOMATIC_ONNX_IO_MAX_BYTES)
    rejectIo("AUTOMATIC_IO_LIMIT");
  const auto value = nativeParseJson(encoded, 64);
  if (!value.is_object() || value.size() != 7 ||
      value.value("schema", NativeJson{}) != IO_SCHEMA)
    rejectIo("AUTOMATIC_IO_SCHEMA");
  const auto names = [&](const char* field) {
    if (!value.contains(field) || !value.at(field).is_array() ||
        value.at(field).size() > MAX_IO_NAMES)
      rejectIo("AUTOMATIC_IO_FIELDS");
    std::vector<std::string> result;
    for (const auto& item : value.at(field)) {
      if (!item.is_string())
        rejectIo("AUTOMATIC_IO_NAME_TYPE");
      const auto& name = item.get_ref<const std::string&>();
      if (name.empty() || name.size() > MAX_IO_NAME_BYTES ||
          name.find('\0') != std::string::npos)
        rejectIo("AUTOMATIC_IO_NAME");
      result.push_back(name);
    }
    validateNames(result);
    return result;
  };
  AutomaticOnnxIoContract result;
  result.io.inputNames = names("inputNames");
  result.io.outputNames = names("outputNames");
  result.io.stateInputNames = names("stateInputNames");
  result.io.stateOutputNames = names("stateOutputNames");
  result.unrankedInputNames = names("unrankedInputNames");
  result.passthroughTensors = names("passthroughTensors");
  bindPairs(result.io);
  if (result.canonicalJson() != encoded)
    rejectIo("AUTOMATIC_IO_NONCANONICAL");
  return result;
}

AutomaticOnnxIoContract
automaticOnnxIoContractForProjection(const NativeSelectionProjectionV3& projection)
{
  const auto& assembly = projection.assemblyContract();
  if (!projection.automaticShapeSeed ||
      assembly.automaticContractDigest != projection.automaticShapeSeed->contractDigest)
    rejectIo("AUTOMATIC_IO_CONTRACT_IDENTITY");
  if (assembly.expectedInputs.size() > MAX_IO_NAMES ||
      assembly.expectedOutputs.size() > MAX_IO_NAMES)
    rejectIo("AUTOMATIC_IO_NAME_COUNT");
  AutomaticOnnxIoContract result;
  for (const auto& tensor : assembly.expectedInputs) {
    result.io.inputNames.push_back(tensor.name);
    if (tensor.shape.empty())
      result.unrankedInputNames.push_back(tensor.name);
  }
  for (const auto& tensor : assembly.expectedOutputs)
    result.io.outputNames.push_back(tensor.name);
  const auto& generation = projection.generationContract;
  if (generation.enabled) {
    if (!generation.stateSuccessorMap.empty() || !generation.positionInputPolicy.empty())
      rejectIo("AUTOMATIC_IO_LEGACY_GENERATION_POLICY");
    validateNames(generation.stateInputNames);
    validateNames(generation.stateOutputNames);
    if (generation.stateInputNames.empty() ||
        generation.stateInputNames.size() != generation.stateOutputNames.size())
      rejectIo("AUTOMATIC_IO_PAIR_COUNT");
    for (std::size_t index = 0; index < generation.stateInputNames.size(); ++index) {
      const auto& input = generation.stateInputNames[index];
      const auto& output = generation.stateOutputNames[index];
      const bool localInput = contains(result.io.inputNames, input);
      const bool localOutput = contains(result.io.outputNames, output);
      if (localInput != localOutput)
        rejectIo("AUTOMATIC_IO_CROSSED_PAIR");
      if (localInput) {
        result.io.stateInputNames.push_back(input);
        result.io.stateOutputNames.push_back(output);
      }
    }
    if (result.io.stateInputNames.empty())
      rejectIo("AUTOMATIC_IO_STATELESS_GENERATION_ROLE");
  }
  bindPairs(result.io);
  std::set<std::string> passthrough;
  if (!projection.dataflow.terminalResponseOwner) {
    for (const auto& endpoint : projection.dataflow.mayPublish) {
      for (const auto& name : endpoint.bundleTensorNames) {
        if (contains(result.io.inputNames, name) && !contains(result.io.stateInputNames, name))
          passthrough.insert(name);
      }
    }
  }
  result.passthroughTensors.assign(passthrough.begin(), passthrough.end());
  result.validate();
  return result;
}

void
bindAutomaticOnnxRunnerIo(NativeModelRunnerSpec& spec,
                          const NativeSelectionProjectionV3& projection)
{
  if (!projection.automaticShapeSeed) {
    if (spec.metadata.count("automaticIoContract") != 0)
      rejectIo("AUTOMATIC_IO_LEGACY_PROJECTION");
    return;
  }
  const auto value = automaticOnnxIoContractForProjection(projection);
  const auto encoded = value.canonicalJson();
  for (const auto* key : LEGACY_KEYS)
    spec.metadata.erase(key);
  spec.metadata.erase("stateful_model");
  spec.metadata.erase("stateful");
  spec.metadata["statefulModel"] = value.io.stateInputNames.empty() ? "false" : "true";
  spec.metadata["automaticIoContract"] = encoded;
}

std::optional<AutomaticOnnxIoContract>
readAutomaticOnnxRunnerIo(const NativeModelRunnerSpec& spec)
{
  const auto found = spec.metadata.find("automaticIoContract");
  if (found == spec.metadata.end())
    return std::nullopt;
  for (const auto* key : LEGACY_KEYS) {
    if (spec.metadata.count(key) != 0)
      rejectIo("AUTOMATIC_IO_MIXED_LEGACY_METADATA");
  }
  if (spec.metadata.count("stateful_model") != 0 || spec.metadata.count("stateful") != 0)
    rejectIo("AUTOMATIC_IO_MIXED_LEGACY_METADATA");
  auto result = AutomaticOnnxIoContract::fromCanonicalJson(found->second);
  const auto stateful = spec.metadata.find("statefulModel");
  if (stateful != spec.metadata.end() &&
      stateful->second != (result.io.stateInputNames.empty() ? "false" : "true"))
    rejectIo("AUTOMATIC_IO_STATEFUL_FLAG");
  return result;
}

} // namespace ndnsf::di
