#ifndef NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_MODEL_RUNNER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_MODEL_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>

namespace ndnsf::di {

struct OnnxRuntimeProviderSelection
{
  std::string requestedProvider;
  std::string selectedProvider;
  std::string deviceId;
  bool usedCpuFallback = false;
};

/** Adapter-certified stateful I/O names for one persistent ORT session. */
struct StatefulOnnxIoContractV1
{
  std::vector<std::string> inputNames;
  std::vector<std::string> outputNames;
  std::vector<std::string> stateInputNames;
  std::vector<std::string> stateOutputNames;
  std::vector<std::string> stateFamilies{
    "attention_kv", "recurrent_state", "convolution_state"};
  // Dynamic-past models may use names that do not follow the legacy *_in /
  // *_out convention.  The adapter supplies an explicit output-to-input map
  // for those contracts; an empty map retains the legacy family rules.
  std::map<std::string, std::string> successorInputByOutput;

  std::string stateInputForOutput(const std::string& outputName) const
  {
    if (std::find(stateOutputNames.begin(), stateOutputNames.end(), outputName) ==
        stateOutputNames.end()) {
      throw std::invalid_argument(
        "stateful ONNX output has no declared successor input: " + outputName);
    }
    const auto mapped = successorInputByOutput.find(outputName);
    const auto inputName = mapped != successorInputByOutput.end()
      ? mapped->second
      : (outputName.size() > 4 &&
         outputName.compare(outputName.size() - 4, 4, "_out") == 0
           ? outputName.substr(0, outputName.size() - 4) + "_in"
           : std::string{});
    if (std::find(stateInputNames.begin(), stateInputNames.end(), inputName) ==
        stateInputNames.end()) {
      throw std::invalid_argument(
        "stateful ONNX output is missing its successor input: " + outputName);
    }
    return inputName;
  }

  void validate() const
  {
    if (inputNames.empty() || outputNames.empty()) {
      throw std::invalid_argument("stateful ONNX I/O contract is empty");
    }
    const auto uniqueNonEmpty = [] (const std::vector<std::string>& values,
                                    const char* label) {
      const auto duplicate = std::any_of(
        values.begin(), values.end(), [&values] (const std::string& value) {
          return value.empty() ||
                 std::count(values.begin(), values.end(), value) != 1;
        });
      if (duplicate) {
        throw std::invalid_argument(std::string("stateful ONNX I/O ") + label +
                                    " contains an empty or duplicate name");
      }
    };
    uniqueNonEmpty(inputNames, "inputs");
    uniqueNonEmpty(outputNames, "outputs");
    uniqueNonEmpty(stateInputNames, "state inputs");
    uniqueNonEmpty(stateOutputNames, "state outputs");
    if (stateInputNames.size() != stateOutputNames.size()) {
      throw std::invalid_argument(
        "stateful ONNX state input/output counts differ");
    }
    if (!stateFamilies.empty() && stateFamilies != std::vector<std::string>{
          "attention_kv", "recurrent_state", "convolution_state"}) {
      throw std::invalid_argument("stateful ONNX I/O state families are not canonical");
    }
    const auto contains = [] (const std::vector<std::string>& values,
                              const std::string& name) {
      return std::find(values.begin(), values.end(), name) != values.end();
    };
    for (const auto& family : stateFamilies) {
      if (!contains(stateInputNames, family + "_in") ||
          !contains(stateOutputNames, family + "_out") ||
          !contains(inputNames, family + "_in") ||
          !contains(outputNames, family + "_out")) {
        throw std::invalid_argument(
          "stateful ONNX I/O family is incomplete: " + family);
      }
    }
    if (stateFamilies.empty() && successorInputByOutput.size() != stateOutputNames.size()) {
      throw std::invalid_argument(
        "dynamic stateful ONNX I/O successor map is incomplete");
    }
    for (const auto& outputName : stateOutputNames) {
      stateInputForOutput(outputName);
    }
    for (const auto& inputName : stateInputNames) {
      if (successorInputByOutput.empty() &&
          (inputName.size() <= 3 ||
           inputName.compare(inputName.size() - 3, 3, "_in") != 0)) {
        throw std::invalid_argument(
          "stateful ONNX input has no declared predecessor output: " + inputName);
      }
      if (successorInputByOutput.empty()) {
        const auto outputName = inputName.substr(0, inputName.size() - 3) + "_out";
        if (!contains(stateOutputNames, outputName)) {
          throw std::invalid_argument(
            "stateful ONNX input is missing its predecessor output: " + inputName);
        }
      }
      else if (std::none_of(successorInputByOutput.begin(), successorInputByOutput.end(),
                            [&inputName] (const auto& item) {
                              return item.second == inputName;
                            })) {
        throw std::invalid_argument(
          "stateful ONNX input is missing its predecessor output: " + inputName);
      }
    }
  }
};

/** Adapter-certified graph inputs derived from authenticated token lineage. */
struct CausalPositionInputContractV1
{
  std::string policy;
  std::string attentionMaskInputName;
  std::string positionIdsInputName;
  std::string cachePositionInputName;

  void
  validate(const StatefulOnnxIoContractV1& io) const;
};

std::map<std::string, TensorBundle>
materializeCausalPositionInputsV1(
  const CausalPositionInputContractV1& contract,
  const StatefulOnnxIoContractV1& io,
  const GenerationEpochLineageV1& lineage,
  std::uint32_t newTokenCount);

OnnxRuntimeProviderSelection
resolveOnnxRuntimeProviderSelection(const NativeModelRunnerSpec& spec,
                                    const std::vector<std::string>& availableProviders);

class OnnxRuntimeModelRunner final : public NativeModelRunner
{
public:
  explicit OnnxRuntimeModelRunner(NativeModelRunnerSpec spec);
  ~OnnxRuntimeModelRunner() final;

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final;

  void
  releaseSessionState(const std::string& sessionId) final;

  std::optional<std::map<std::string, TensorBundle>>
  runStreamed(const RoleExecutionContext& ctx) final;

  const std::optional<ExecutionEvidence>&
  executionEvidence() const final;

  std::optional<ExecutionEvidence>
  executionEvidenceSnapshot() const final;

  std::optional<NativeRuntimeMetrics>
  runtimeMetricsSnapshot() const final;

  bool
  supportsOpaqueStateHandles() const final;

  std::optional<NativeOpaqueStateHandleV1>
  stateHandleSnapshot(const std::string& sessionId) const final;

  bool
  supportsConversationStateTransfer() const final;

  std::optional<NativeConversationStateHandleV1>
  promoteSessionStateToConversation(const std::string& sessionId,
                                    const std::string& conversationKey) final;

  bool
  restoreConversationState(const NativeConversationStateHandleV1& state,
                           const std::string& sessionId) final;

  bool
  pauseConversationStateToHost(const NativeConversationStateHandleV1& state) final;

  std::future<bool>
  prefetchConversationStateToGpu(const NativeConversationStateHandleV1& state) final;

  bool
  cancelConversationStatePrefetch(const NativeConversationStateHandleV1& state) final;

  bool
  releaseConversationState(const NativeConversationStateHandleV1& state) final;

private:
  class Impl;
  std::optional<std::map<std::string, TensorBundle>>
  runStreamedImpl(const RoleExecutionContext& ctx);
  NativeModelRunnerSpec m_spec;
  std::optional<ExecutionEvidence> m_evidence;
  std::unique_ptr<Impl> m_impl;
};

void
registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory& factory);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_MODEL_RUNNER_HPP
