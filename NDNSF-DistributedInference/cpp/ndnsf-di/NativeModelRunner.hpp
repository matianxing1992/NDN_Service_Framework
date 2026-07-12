#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
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

  virtual const std::optional<ExecutionEvidence>&
  executionEvidence() const;
};

class LambdaModelRunner final : public NativeModelRunner
{
public:
  explicit LambdaModelRunner(RoleRunner runner,
                             std::optional<ExecutionEvidence> evidence = std::nullopt);

  std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) final;

  const std::optional<ExecutionEvidence>&
  executionEvidence() const final;

private:
  RoleRunner m_runner;
  std::optional<ExecutionEvidence> m_evidence;
};

std::shared_ptr<NativeModelRunner>
makeNativeModelRunner(RoleRunner runner);

std::shared_ptr<NativeModelRunner>
makeNativeModelRunner(RoleRunner runner, ExecutionEvidence evidence);

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
