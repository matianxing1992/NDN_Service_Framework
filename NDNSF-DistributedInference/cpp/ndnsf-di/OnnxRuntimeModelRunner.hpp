#ifndef NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_MODEL_RUNNER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_MODEL_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace ndnsf::di {

#ifndef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP

class OnnxRuntimeModelRunner final : public NativeModelRunner
{
public:
  explicit OnnxRuntimeModelRunner(NativeModelRunnerSpec spec)
    : m_spec(std::move(spec))
  {
    throw std::runtime_error(
      "C++ ONNX Runtime backend is not enabled; install the ONNX Runtime "
      "C++ development package and build with NDNSF_DI_ENABLE_ONNXRUNTIME_CPP");
  }

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext&) final
  {
    throw std::runtime_error("C++ ONNX Runtime backend is not enabled");
  }

private:
  NativeModelRunnerSpec m_spec;
};

inline void
registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory& factory)
{
  factory.registerBackend(
    "onnxruntime",
    [] (const NativeModelRunnerSpec& spec) -> std::shared_ptr<NativeModelRunner> {
      return std::make_shared<OnnxRuntimeModelRunner>(spec);
    });
}

#else

class OnnxRuntimeModelRunner final : public NativeModelRunner
{
public:
  explicit OnnxRuntimeModelRunner(NativeModelRunnerSpec spec);

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final;

private:
  NativeModelRunnerSpec m_spec;
};

void
registerOnnxRuntimeBackend(RegistryNativeModelRunnerFactory& factory);

#endif

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_ONNX_RUNTIME_MODEL_RUNNER_HPP
