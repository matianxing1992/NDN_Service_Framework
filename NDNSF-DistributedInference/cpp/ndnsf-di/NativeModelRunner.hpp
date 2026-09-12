#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"

#include <cstdint>
#include <cstddef>
#include <future>
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

/**
 * Runtime transfer/state counters owned by a model adapter.
 *
 * The counters deliberately distinguish model state movement from ordinary
 * activation/control I/O.  They are metadata-only evidence; no tensor bytes
 * or opaque state contents are exposed through this structure.
 */
struct NativeRuntimeMetrics
{
  std::uint64_t stateDeviceToHostBytes = 0;
  std::uint64_t stateHostToDeviceBytes = 0;
  std::uint64_t activationInputBytes = 0;
  std::uint64_t activationOutputBytes = 0;
  std::uint64_t activationHostToDeviceBytes = 0;
  std::uint64_t activationDeviceToHostBytes = 0;
  std::uint64_t controlBytes = 0;
  std::uint64_t stateInputHits = 0;
  std::uint64_t stateInputMisses = 0;
  std::uint64_t stateRecomputes = 0;
  std::uint64_t stateReleases = 0;
};

/**
 * A reference to adapter-owned request state.  The token is deliberately
 * opaque to the coordinator and ProviderRoleWorker: it identifies a
 * Provider-local transaction, but never contains tensor bytes or a device
 * pointer.  CPU adapters may use the existing host-tensor state path instead.
 */
struct NativeOpaqueStateHandleV1
{
  std::string providerIdentity;
  std::string providerBootId;
  std::string sessionId;
  std::string role;
  std::string token;
  std::uint64_t stateInferenceEpoch = 0;

  void
  validate() const;
};

/**
 * Provider-owned conversation state reference.  The coordinator may carry
 * this value across the store boundary, but it never contains tensor bytes or
 * a device address.  The adapter owns the state named by conversationKey.
 */
struct NativeConversationStateHandleV1
{
  NativeOpaqueStateHandleV1 opaque;
  std::string conversationKey;
  std::size_t logicalBytes = 0;

  void
  validate() const;
};

class NativeModelRunner
{
public:
  virtual ~NativeModelRunner() = default;

  virtual std::map<std::string, TensorBundle>
  run(const RoleExecutionContext& ctx) = 0;

  /**
   * Execute one request-scoped incremental generation on the same native
   * runner/session.  The default is deliberately unsupported: ordinary role
   * runners remain one-shot and ProviderRoleWorker falls back to run().  A
   * streaming adapter must return the final role outputs only after every
   * externally visible event has been accepted by ctx.streamEventSink.
   */
  virtual std::optional<std::map<std::string, TensorBundle>>
  runStreamed(const RoleExecutionContext& ctx);

  virtual const std::optional<ExecutionEvidence>&
  executionEvidence() const;

  virtual std::optional<ExecutionEvidence>
  executionEvidenceSnapshot() const;

  /** Return adapter-owned metadata counters for the current Provider. */
  virtual std::optional<NativeRuntimeMetrics>
  runtimeMetricsSnapshot() const;

  /** Whether this adapter owns streamed state behind an opaque handle. */
  virtual bool
  supportsOpaqueStateHandles() const;

  /** Return the current Provider-local state transaction handle, if any. */
  virtual std::optional<NativeOpaqueStateHandleV1>
  stateHandleSnapshot(const std::string& sessionId) const;

  /** Release request-scoped opaque adapter state after terminal cleanup. */
  virtual void
  releaseSessionState(const std::string& sessionId);

  /** Whether this runner can retain conversation state without a bundle. */
  virtual bool
  supportsConversationStateTransfer() const;

  /** Move request-local adapter state into a Provider-owned conversation key. */
  virtual std::optional<NativeConversationStateHandleV1>
  promoteSessionStateToConversation(const std::string& sessionId,
                                    const std::string& conversationKey);

  /** Bind a retained conversation state as the initial state for a new session. */
  virtual bool
  restoreConversationState(const NativeConversationStateHandleV1& state,
                           const std::string& sessionId);

  /** Perform real adapter-owned GPU-to-host movement for one conversation. */
  virtual bool
  pauseConversationStateToHost(const NativeConversationStateHandleV1& state);

  /** Perform real adapter-owned host-to-GPU movement for one conversation. */
  virtual std::future<bool>
  prefetchConversationStateToGpu(const NativeConversationStateHandleV1& state);

  /** Cancel an in-flight adapter prefetch, if supported. */
  virtual bool
  cancelConversationStatePrefetch(const NativeConversationStateHandleV1& state);

  /** Release all adapter-owned buffers for one conversation state. */
  virtual bool
  releaseConversationState(const NativeConversationStateHandleV1& state);
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

  void
  replaceBackend(std::string backend, Creator creator);

  void
  freeze();

  bool
  frozen() const noexcept { return m_frozen; }

  bool
  hasBackend(const std::string& backend) const;

  std::shared_ptr<NativeModelRunner>
  create(const NativeModelRunnerSpec& spec) const final;

private:
  std::map<std::string, Creator> m_creators;
  bool m_frozen = false;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_MODEL_RUNNER_HPP
