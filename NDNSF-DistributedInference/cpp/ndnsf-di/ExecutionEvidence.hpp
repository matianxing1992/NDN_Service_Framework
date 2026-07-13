#ifndef NDNSF_DISTRIBUTED_INFERENCE_EXECUTION_EVIDENCE_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_EXECUTION_EVIDENCE_HPP

#include <cstdint>
#include <map>
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

  std::string schema = "ndnsf-di-execution-evidence-v1";
  std::string providerName;
  std::string providerBootId;
  std::uint64_t evidenceEpoch = 0;
  RunnerKind runnerKind = RunnerKind::Unknown;
  bool realCompute = false;
  std::string deviceKind;
  std::string deviceId;
  std::string runtimeVersion;
  std::string modelDigest;
  std::string planDigest;
  std::map<std::string, std::string> artifactDigests;
  std::vector<std::string> roles;
  std::vector<NodeProviderAssignment> nodeProviderAssignments;
  bool cpuFallbackUsed = false;
  std::string gpuUuid;
  std::string providerProfilePath;
  std::uint64_t createdAtMs = 0;

  void validate() const;
};

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
