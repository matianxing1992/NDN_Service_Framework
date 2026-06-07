#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>

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
  explicit LambdaModelRunner(RoleRunner runner);

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final;

private:
  RoleRunner m_runner;
};

std::shared_ptr<NativeModelRunner>
makeNativeModelRunner(RoleRunner runner);

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
  registerBackend(std::string backend, Creator creator);

  bool
  hasBackend(const std::string& backend) const;

  std::shared_ptr<NativeModelRunner>
  create(const NativeModelRunnerSpec& spec) const final;

private:
  std::map<std::string, Creator> m_creators;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP
