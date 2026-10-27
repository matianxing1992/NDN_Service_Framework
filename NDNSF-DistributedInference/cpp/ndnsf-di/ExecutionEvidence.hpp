#ifndef NDNSF_DISTRIBUTED_INFERENCE_EXECUTION_EVIDENCE_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_EXECUTION_EVIDENCE_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeModelRunnerSpec;

enum class RunnerKind
{
  SyntheticDelay,
  WiringOnly,
  OnnxRuntimeCpu,
  OnnxRuntimeCuda,
  NativeYoloPostprocess,
  Transformers,
  LlamaServer,
  Unknown,
};

std::string toString(RunnerKind kind);
RunnerKind runnerKindFromString(const std::string& value);

struct ExecutionEvidence
{
  struct NodeProviderAssignment
  {
    std::string role;
    std::string nodeName;
    std::string provider;
    bool modelNode = true;
  };

  using NodeProviderAssignments = std::vector<NodeProviderAssignment>;

  std::string schema = "ndnsf-di-execution-evidence-v1";
  std::string providerName;
  std::string providerBootId;
  std::uint64_t evidenceEpoch = 0;
  RunnerKind runnerKind = RunnerKind::Unknown;
  bool realCompute = false;
  std::string deviceKind;
  std::string deviceId;
  // A provider may execute different roles on different devices.  Keep the
  // scalar deviceId for single-device/backward-compatible evidence, and use
  // deviceIds when the provider aggregate spans more than one device.
  std::vector<std::string> deviceIds;
  std::string runtimeVersion;
  std::string modelDigest;
  std::string planDigest;
  std::map<std::string, std::string> artifactDigests;
  std::vector<std::string> roles;
  bool cpuFallbackUsed = false;
  bool loadCompleted = false;
  bool warmupCompleted = false;
  std::string gpuUuid;
  std::vector<std::string> gpuUuids;
  std::string providerProfilePath;
  std::uint64_t createdAtMs = 0;
  // Optional additive observations; absent in older v1 records. They are not
  // caller metadata, and preparation/warmup alone cannot mark execution done.
  std::uint64_t processId = 0;
  std::string cudaVisibleDevices;
  std::string gpuIdentitySource;
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  bool executionCompleted = false;
  bool exactForwardCacheHit = false;
  std::string profileRequestId;
  std::uint64_t profileAttemptEpoch = 0;

  void validate() const;

  /**
   * Borrow the immutable inventory. The reference requires an owning observation
   * to retain this inventory; replacement or destruction of its last owner can
   * invalidate it. Default and moved-from observations have an empty inventory.
   */
  const NodeProviderAssignments&
  getNodeProviderAssignments() const noexcept;

  /** Copy caller storage, including rvalues, without changing older snapshots. */
  void
  setNodeProviderAssignments(const NodeProviderAssignments& assignments);

  /**
   * Copy groups once in their supplied order, preserving duplicate rows/groups.
   * Input references are borrowed only during this call. Publish the complete
   * replacement after construction succeeds; retain no mutable builder aliases.
   */
  void
  concatenateNodeProviderAssignments(
    const std::vector<std::reference_wrapper<const NodeProviderAssignments>>& groups);

private:
  std::shared_ptr<const NodeProviderAssignments> m_nodeProviderAssignments;
};

void bindExecutionObservation(ExecutionEvidence& evidence,
                              const std::string& requestId,
                              std::uint64_t attemptEpoch, bool cacheHit);

void applyOnnxRuntimeProviderProfile(ExecutionEvidence& evidence,
                                     const std::string& profilePath,
                                     const std::string& role,
                                     bool cpuFallbackUsed,
                                     const std::string& gpuUuid = "");

std::string executionEvidenceToJson(const ExecutionEvidence& evidence);
ExecutionEvidence executionEvidenceFromJson(const std::string& json);

ExecutionEvidence executionEvidenceFromRunnerSpec(const NativeModelRunnerSpec& spec,
                                                  RunnerKind kind,
                                                  std::string runtimeVersion,
                                                  std::string deviceKind,
                                                  std::string deviceId = "");

enum class TerminalReason
{
  None,
  ProviderLost,
  StragglerDeadline,
  DependencyMissing,
  DependencyHashMismatch,
  PlanStale,
  TelemetryStale,
  CacheMissFullContextRequired,
  AttemptCancelled,
  NoCompatibleReplacement,
  RequestDeadline,
};

std::string toString(TerminalReason reason);
TerminalReason terminalReasonFromString(const std::string& value);

struct ExecutionAttemptMetadata
{
  std::string schema = "ndnsf-di-execution-attempt-v1";
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  std::string planId;
  TerminalReason terminalReason = TerminalReason::None;

  void validate() const;
};

std::string executionAttemptToJson(const ExecutionAttemptMetadata& attempt);
ExecutionAttemptMetadata executionAttemptFromJson(const std::string& json);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_EXECUTION_EVIDENCE_HPP
