#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::di {
namespace {

using boost::property_tree::ptree;

bool isRealRunner(RunnerKind kind)
{
  return kind == RunnerKind::OnnxRuntimeCpu ||
         kind == RunnerKind::OnnxRuntimeCuda ||
         kind == RunnerKind::Transformers ||
         kind == RunnerKind::LlamaServer;
}

void requireSchema(const std::string& actual, const std::string& expected)
{
  if (actual != expected) {
    throw std::invalid_argument("unsupported or missing schema: " + actual);
  }
}

void rejectSecretFields(const ptree& root)
{
  static const std::set<std::string> forbidden{
    "key", "privateKey", "token", "userToken", "providerToken",
    "prompt", "payload", "tensor", "kvPayload"
  };
  for (const auto& item : root) {
    if (forbidden.count(item.first) != 0) {
      throw std::invalid_argument("execution evidence contains forbidden field: " + item.first);
    }
  }
}

ptree stringArray(const std::vector<std::string>& values)
{
  ptree output;
  for (const auto& value : values) {
    ptree item;
    item.put_value(value);
    output.push_back({"", item});
  }
  return output;
}

std::vector<std::string> readStringArray(const ptree& root, const std::string& key)
{
  const auto child = root.get_child_optional(key);
  if (!child) {
    return {};
  }
  std::vector<std::string> values;
  for (const auto& item : child.get()) {
    values.push_back(item.second.get_value<std::string>());
  }
  return values;
}

std::string writeJson(const ptree& root)
{
  std::ostringstream output;
  boost::property_tree::write_json(output, root, false);
  auto value = output.str();
  if (!value.empty() && value.back() == '\n') value.pop_back();
  return value;
}

ptree readJson(const std::string& json)
{
  std::istringstream input(json);
  ptree root;
  boost::property_tree::read_json(input, root);
  return root;
}

} // namespace

std::string toString(RunnerKind kind)
{
  switch (kind) {
    case RunnerKind::SyntheticDelay: return "synthetic-delay";
    case RunnerKind::WiringOnly: return "wiring-only";
    case RunnerKind::OnnxRuntimeCpu: return "onnxruntime-cpu";
    case RunnerKind::OnnxRuntimeCuda: return "onnxruntime-cuda";
    case RunnerKind::Transformers: return "transformers";
    case RunnerKind::LlamaServer: return "llama-server";
    case RunnerKind::Unknown: return "unknown";
  }
  return "unknown";
}

RunnerKind runnerKindFromString(const std::string& value)
{
  if (value == "synthetic-delay") return RunnerKind::SyntheticDelay;
  if (value == "wiring-only") return RunnerKind::WiringOnly;
  if (value == "onnxruntime-cpu") return RunnerKind::OnnxRuntimeCpu;
  if (value == "onnxruntime-cuda") return RunnerKind::OnnxRuntimeCuda;
  if (value == "transformers") return RunnerKind::Transformers;
  if (value == "llama-server") return RunnerKind::LlamaServer;
  if (value == "unknown") return RunnerKind::Unknown;
  throw std::invalid_argument("unknown execution runner kind: " + value);
}

void ExecutionEvidence::validate() const
{
  requireSchema(schema, "ndnsf-di-execution-evidence-v1");
  if (providerName.empty() || providerBootId.empty() || runtimeVersion.empty() ||
      modelDigest.empty() || planDigest.empty() || deviceKind.empty() ||
      roles.empty() || artifactDigests.empty() || createdAtMs == 0) {
    throw std::invalid_argument("execution evidence missing required field");
  }
  if (runnerKind == RunnerKind::Unknown || realCompute != isRealRunner(runnerKind)) {
    throw std::invalid_argument("execution evidence real-compute classification mismatch");
  }
  if (runnerKind == RunnerKind::OnnxRuntimeCuda && deviceId.empty()) {
    throw std::invalid_argument("CUDA execution evidence missing device id");
  }
}

std::string executionEvidenceToJson(const ExecutionEvidence& evidence)
{
  evidence.validate();
  ptree root;
  root.put("schema", evidence.schema);
  root.put("providerName", evidence.providerName);
  root.put("providerBootId", evidence.providerBootId);
  root.put("evidenceEpoch", evidence.evidenceEpoch);
  root.put("runnerKind", toString(evidence.runnerKind));
  root.put("realCompute", evidence.realCompute);
  root.put("device.kind", evidence.deviceKind);
  root.put("device.id", evidence.deviceId);
  root.put("runtimeVersion", evidence.runtimeVersion);
  root.put("modelDigest", evidence.modelDigest);
  root.put("planDigest", evidence.planDigest);
  ptree artifacts;
  for (const auto& item : evidence.artifactDigests) artifacts.put(item.first, item.second);
  root.add_child("artifactDigests", artifacts);
  root.add_child("roles", stringArray(evidence.roles));
  root.put("createdAtMs", evidence.createdAtMs);
  return writeJson(root);
}

ExecutionEvidence executionEvidenceFromJson(const std::string& json)
{
  const auto root = readJson(json);
  rejectSecretFields(root);
  ExecutionEvidence evidence;
  evidence.schema = root.get<std::string>("schema", "");
  requireSchema(evidence.schema, "ndnsf-di-execution-evidence-v1");
  evidence.providerName = root.get<std::string>("providerName", "");
  evidence.providerBootId = root.get<std::string>("providerBootId", "");
  evidence.evidenceEpoch = root.get<std::uint64_t>("evidenceEpoch", 0);
  evidence.runnerKind = runnerKindFromString(root.get<std::string>("runnerKind", "unknown"));
  evidence.realCompute = root.get<bool>("realCompute", false);
  evidence.deviceKind = root.get<std::string>("device.kind", "");
  evidence.deviceId = root.get<std::string>("device.id", "");
  evidence.runtimeVersion = root.get<std::string>("runtimeVersion", "");
  evidence.modelDigest = root.get<std::string>("modelDigest", "");
  evidence.planDigest = root.get<std::string>("planDigest", "");
  if (const auto artifacts = root.get_child_optional("artifactDigests")) {
    for (const auto& item : artifacts.get()) {
      evidence.artifactDigests[item.first] = item.second.get_value<std::string>();
    }
  }
  evidence.roles = readStringArray(root, "roles");
  evidence.createdAtMs = root.get<std::uint64_t>("createdAtMs", 0);
  evidence.validate();
  return evidence;
}

std::string toString(TerminalReason reason)
{
  switch (reason) {
    case TerminalReason::None: return "NONE";
    case TerminalReason::ProviderLost: return "PROVIDER_LOST";
    case TerminalReason::StragglerDeadline: return "STRAGGLER_DEADLINE";
    case TerminalReason::DependencyMissing: return "DEPENDENCY_MISSING";
    case TerminalReason::DependencyHashMismatch: return "DEPENDENCY_HASH_MISMATCH";
    case TerminalReason::PlanStale: return "PLAN_STALE";
    case TerminalReason::TelemetryStale: return "TELEMETRY_STALE";
    case TerminalReason::CacheMissFullContextRequired: return "CACHE_MISS_FULL_CONTEXT_REQUIRED";
    case TerminalReason::AttemptCancelled: return "ATTEMPT_CANCELLED";
    case TerminalReason::NoCompatibleReplacement: return "NO_COMPATIBLE_REPLACEMENT";
    case TerminalReason::RequestDeadline: return "REQUEST_DEADLINE";
  }
  return "NONE";
}

TerminalReason terminalReasonFromString(const std::string& value)
{
  for (const auto reason : {TerminalReason::None, TerminalReason::ProviderLost,
                            TerminalReason::StragglerDeadline, TerminalReason::DependencyMissing,
                            TerminalReason::DependencyHashMismatch, TerminalReason::PlanStale,
                            TerminalReason::TelemetryStale, TerminalReason::CacheMissFullContextRequired,
                            TerminalReason::AttemptCancelled, TerminalReason::NoCompatibleReplacement,
                            TerminalReason::RequestDeadline}) {
    if (toString(reason) == value) return reason;
  }
  throw std::invalid_argument("unknown terminal reason: " + value);
}

void ExecutionAttemptMetadata::validate() const
{
  requireSchema(schema, "ndnsf-di-execution-attempt-v1");
  if (requestId.empty() || planId.empty() || attemptEpoch > 1) {
    throw std::invalid_argument("invalid execution attempt metadata");
  }
}

std::string executionAttemptToJson(const ExecutionAttemptMetadata& attempt)
{
  attempt.validate();
  ptree root;
  root.put("schema", attempt.schema);
  root.put("requestId", attempt.requestId);
  root.put("attemptEpoch", attempt.attemptEpoch);
  root.put("planId", attempt.planId);
  root.put("terminalReason", toString(attempt.terminalReason));
  return writeJson(root);
}

ExecutionAttemptMetadata executionAttemptFromJson(const std::string& json)
{
  const auto root = readJson(json);
  ExecutionAttemptMetadata attempt;
  attempt.schema = root.get<std::string>("schema", "");
  attempt.requestId = root.get<std::string>("requestId", "");
  attempt.attemptEpoch = root.get<std::uint64_t>("attemptEpoch", 2);
  attempt.planId = root.get<std::string>("planId", "");
  attempt.terminalReason = terminalReasonFromString(root.get<std::string>("terminalReason", "NONE"));
  attempt.validate();
  return attempt;
}

} // namespace ndnsf::di
