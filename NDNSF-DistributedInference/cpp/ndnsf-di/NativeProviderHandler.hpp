#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ExecutionLease.hpp"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeProviderHandlerConfig
{
  NativeExecutionPlan plan;
  NativeProviderAssignment assignment;
  std::shared_ptr<NativeModelRunnerFactory> runnerFactory;
  std::vector<NativeModelRunnerSpec> runnerSpecs;
  std::string finalResponseScope = "final-response";
  std::string localProviderName;
  int fetchTimeoutMs = 30000;
  std::size_t maxSegmentSize = 7000;
  int freshnessMs = 60000;
  std::size_t workerCount = 1;
  ndn_service_framework::ProviderExecutionLeaseTable* executionLeaseTable = nullptr;
  std::string executionLeaseTargetService;
  uint64_t executionLeaseHardDeadlineMs = 120000;
};

std::optional<std::vector<uint8_t>>
nativeProviderFinalResponsePayload(const RoleSpec& roleSpec,
                                   const ProviderRoleResult& result,
                                   const std::string& finalResponseScope);

inline std::map<std::string, std::string>
parseNativeProviderAssignmentFields(const ndn::Buffer& payload)
{
  std::map<std::string, std::string> fields;
  const std::string text(reinterpret_cast<const char*>(payload.data()),
                         payload.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    const auto eq = text.find('=', pos);
    if (eq == std::string::npos) {
      break;
    }
    const auto end = text.find(';', eq + 1);
    fields[text.substr(pos, eq - pos)] =
      text.substr(eq + 1, (end == std::string::npos ? text.size() : end) - eq - 1);
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  return fields;
}

inline std::string
nativeProviderFieldValue(const std::map<std::string, std::string>& fields,
                         std::initializer_list<const char*> names)
{
  for (const auto* name : names) {
    auto it = fields.find(name);
    if (it != fields.end()) {
      return it->second;
    }
  }
  return "";
}

inline std::optional<std::string>
validateNativeProviderAssignmentPayload(
  const std::vector<NativeModelRunnerSpec>& runnerSpecs,
  const std::string& role,
  const ndn::Buffer& assignmentPayload)
{
  const auto fields = parseNativeProviderAssignmentFields(assignmentPayload);
  const auto assignedRole = nativeProviderFieldValue(
    fields,
    {"role", "roleId", "diRole"});
  if (!assignedRole.empty() && assignedRole != role) {
    return "DI_BINDING_ROLE_MISMATCH";
  }
  const auto fragmentDigest = nativeProviderFieldValue(
    fields,
    {"fragmentDigest", "modelFragmentDigest", "diFragmentDigest"});
  if (fragmentDigest.empty()) {
    return std::nullopt;
  }
  auto specIt = std::find_if(runnerSpecs.begin(),
                             runnerSpecs.end(),
                             [&role] (const NativeModelRunnerSpec& spec) {
                               return spec.role == role;
                             });
  if (specIt == runnerSpecs.end()) {
    return "DI_BINDING_RUNNER_MISSING";
  }
  const auto expectedDigest = nativeProviderFieldValue(
    specIt->metadata,
    {"fragmentDigest", "fragment_digest", "sha256", "digest"});
  if (!expectedDigest.empty() && expectedDigest != fragmentDigest) {
    return "DI_BINDING_FRAGMENT_MISMATCH";
  }
  return std::nullopt;
}

ndn_service_framework::ServiceProvider::CollaborationHandler
makeNativeProviderCollaborationHandler(NativeProviderHandlerConfig config);

struct NativeProviderCollaborationRuntime
{
  ndn_service_framework::ServiceProvider::CollaborationHandler handler;
  std::function<ProviderRoleWorkerSnapshot()> capacitySnapshot;
};

NativeProviderCollaborationRuntime
makeNativeProviderCollaborationRuntime(NativeProviderHandlerConfig config);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
