#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace ndnsf::di {

class NativeModelRunner
{
public:
  virtual ~NativeModelRunner() = default;

  virtual std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) = 0;
};

class LambdaModelRunner final : public NativeModelRunner
{
public:
  explicit LambdaModelRunner(RoleRunner runner)
    : m_runner(std::move(runner))
  {
    if (!m_runner) {
      throw std::invalid_argument("LambdaModelRunner requires a runner");
    }
  }

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final
  {
    return m_runner(ctx);
  }

private:
  RoleRunner m_runner;
};

inline std::shared_ptr<NativeModelRunner>
makeNativeModelRunner(RoleRunner runner)
{
  return std::make_shared<LambdaModelRunner>(std::move(runner));
}

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP
