#ifndef NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_MODEL_RUNNER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_MODEL_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

namespace ndnsf::di {

class OnnxRuntimeModelRunner final : public NativeModelRunner
{
public:
  explicit OnnxRuntimeModelRunner(NativeModelRunnerSpec spec);
  ~OnnxRuntimeModelRunner() final;

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final;

  const std::optional<ExecutionEvidence>&
  executionEvidence() const final;

private:
#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  class Impl;
#endif
  NativeModelRunnerSpec m_spec;
  std::optional<ExecutionEvidence> m_evidence;
#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  std::unique_ptr<Impl> m_impl;
#endif
};

void
registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory& factory);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_MODEL_RUNNER_HPP
