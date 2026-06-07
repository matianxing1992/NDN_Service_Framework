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

struct NativeModelRunnerSpec
{
  std::string role;
  std::string kind;
  std::string backend;
  std::string path;
  std::map<std::string, std::string> metadata;
};

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

class NativeModelRunnerFactory
{
public:
  virtual ~NativeModelRunnerFactory() = default;

  virtual std::shared_ptr<NativeModelRunner>
  create(const NativeModelRunnerSpec& spec) const = 0;
};

class RegistryNativeModelRunnerFactory final : public NativeModelRunnerFactory
{
public:
  using Creator = std::function<std::shared_ptr<NativeModelRunner>(
    const NativeModelRunnerSpec&)>;

  void
  registerBackend(std::string backend, Creator creator)
  {
    if (backend.empty()) {
      throw std::invalid_argument("NativeModelRunner backend must not be empty");
    }
    if (!creator) {
      throw std::invalid_argument("NativeModelRunner creator must not be empty");
    }
    m_creators[std::move(backend)] = std::move(creator);
  }

  bool
  hasBackend(const std::string& backend) const
  {
    return m_creators.find(backend) != m_creators.end();
  }

  std::shared_ptr<NativeModelRunner>
  create(const NativeModelRunnerSpec& spec) const final
  {
    if (spec.backend.empty()) {
      throw std::invalid_argument("NativeModelRunnerSpec.backend must not be empty");
    }
    const auto found = m_creators.find(spec.backend);
    if (found == m_creators.end()) {
      throw std::out_of_range("no NativeModelRunner backend registered: " +
                              spec.backend);
    }
    auto runner = found->second(spec);
    if (!runner) {
      throw std::logic_error("NativeModelRunner backend returned null: " +
                             spec.backend);
    }
    return runner;
  }

private:
  std::map<std::string, Creator> m_creators;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP
