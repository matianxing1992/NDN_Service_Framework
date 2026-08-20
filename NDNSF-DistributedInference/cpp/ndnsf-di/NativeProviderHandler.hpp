#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ExecutionLease.hpp"

#include <algorithm>
#include <cctype>
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
  using ProviderGroupCoordinatorFactory = std::function<
    std::shared_ptr<ProviderGroupCoordinator>(
      ndn_service_framework::ServiceProvider::CollaborationContext&,
      const std::map<std::string, std::string>&)>;
  using RunnerPreparationFactory = std::function<NativeModelRunnerSpec(
    const NativeSelectionProjectionV3&)>;
  using ProtectedRuntimeFactory = std::function<std::shared_ptr<ProtectedRuntime>(
    ndn_service_framework::ServiceProvider::CollaborationContext&,
    const NativeSelectionProjectionV3&,
    const std::shared_ptr<ProviderGroupCoordinator>&)>;

  NativeExecutionPlan plan;
  NativeProviderAssignment assignment;
  std::shared_ptr<NativeModelRunnerFactory> runnerFactory;
  std::vector<NativeModelRunnerSpec> runnerSpecs;
  // Normal V3 path: invoked on a bounded Provider worker after Selection to
  // fetch/assemble/validate one exact role, then return its local ORT spec.
  RunnerPreparationFactory runnerPreparationFactory;
  // Explicit rollback-only compatibility for old preassembled V3 fixtures.
  bool allowPreassembledV3Compatibility = false;
  std::string finalResponseScope = "final-response";
  std::string localProviderName;
  std::string providerBootId;
  std::string planDigest;
  // Sealed NDNSF-DI execution contract. The default is per-role,
  // dependency-driven execution; V1 is rollback-only and must be paired with
  // both legacy activation switches plus an exact assignment field.
  std::string executionPolicy = "DATA_DRIVEN_V2";
  bool requireExecutionAttemptBinding = false;
  int fetchTimeoutMs = 30000;
  std::size_t maxSegmentSize = 7000;
  int freshnessMs = 60000;
  // Optional request-scoped cross-Provider data-plane coordinator.  When
  // absent, dependencies retain the ordinary COLLAB-LARGE path.
  std::shared_ptr<ProviderGroupCoordinator> groupCoordinator;
  // Preferred production seam: decode and validate the capability carried by
  // this Selection assignment, unwrap only the local Provider's epoch key,
  // and return a coordinator owned by this request.  A process-wide
  // groupCoordinator remains only for focused fixtures/backward compatibility.
  ProviderGroupCoordinatorFactory groupCoordinatorFactory;
  // Required for protected V3 roles. The factory fetches and verifies the
  // Selection-referenced KeyGrant, unwraps it inside the Provider boundary,
  // installs exact dataflow/capability bindings, and returns GrantVerified.
  ProtectedRuntimeFactory protectedRuntimeFactory;
  std::size_t workerCount = 1;
  std::size_t workerQueueCapacity = 1024;
  ndn_service_framework::ProviderExecutionLeaseTable* executionLeaseTable = nullptr;
  uint64_t executionLeaseCleanupIntervalMs = 1000;
  std::string executionLeaseTargetService;
  uint64_t executionLeaseHardDeadlineMs = 120000;
  // R0 rollback-only compatibility: Core validates the signed
  // ExecutionActivateMessage before invoking the legacy path. R1 execution
  // uses local readiness plus authenticated direct-predecessor evidence and
  // never treats this message as authority.
  bool requireExecutionActivation = false;
  // Explicit rollback-only compatibility switch. New configurations never
  // enable the former peer readiness barrier; Core owns READY aggregation.
  bool allowLegacyPeerReadinessBarrier = false;
  std::shared_ptr<KvStateStore> kvStateStore;
  std::string kvOutputScope = "kv-state";
  std::uint64_t kvSecurityEpoch = 0;
  std::shared_ptr<std::function<void(std::chrono::milliseconds)>>
    stageServiceTimeObserver;
  std::shared_ptr<std::function<void(const ExecutionEvidence&)>>
    executionEvidenceObserver;
};

struct NativeProviderExecutionBindingResult
{
  bool status = false;
  std::string reason;
  ExecutionAttemptKey attempt;
};

void
validateNativeProviderExecutionPolicy(
  const NativeProviderHandlerConfig& config);

NativeProviderExecutionBindingResult
validateNativeProviderExecutionBinding(
  const std::map<std::string, std::string>& fields,
  const std::string& expectedProviderBootId,
  const std::string& expectedPlanDigest,
  ExecutionAttemptAuthority& authority);

std::optional<std::string>
validateNativeProviderRuntimeReadiness(
  const ExecutionEvidence& evidence,
  const std::string& expectedRole,
  const std::string& expectedBackend,
  const std::string& expectedDevice,
  const std::string& expectedArtifactDigest);

std::optional<std::string>
validateNativePreparedRunnerSpec(
  const NativeSelectionProjectionV3& projection,
  const NativeModelRunnerSpec& spec);

std::optional<std::string>
validateProtectedRuntimeBinding(
  const NativeSelectionProjectionV3& projection,
  const ProtectedRuntime& runtime,
  const std::shared_ptr<ProviderGroupCoordinator>& groupCoordinator,
  const std::string& expectedProviderBootId,
  const std::string& expectedFencingToken);

struct NativeProviderExecutionControlResult
{
  bool recognized = false;
  bool status = false;
  std::string reason;
  ExecutionAttemptKey attempt;
};

NativeProviderExecutionControlResult
applyNativeProviderExecutionControl(
  const std::map<std::string, std::string>& fields,
  ExecutionAttemptAuthority& authority);

std::optional<std::vector<uint8_t>>
nativeProviderFinalResponsePayload(const RoleSpec& roleSpec,
                                   const ProviderRoleResult& result,
                                   const std::string& finalResponseScope);

/**
 * Return whether the current request may execute the complete plan inside
 * this Provider.  The current role may be an intermediate role with outputs;
 * that does not disable the same-Provider full-plan path.
 */
bool
nativeProviderShouldExecuteLocalPlan(
  const NativeExecutionPlan& plan,
  const NativeProviderAssignment& assignment,
  const RoleSpec& currentRole,
  const std::string& localProvider);

std::map<std::string, std::string>
parseNativeProviderAssignmentFields(const ndn::Buffer& payload,
                                    const std::string& selectedRole = "");

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

inline bool
nativeProviderDigestEquals(const std::string& left, const std::string& right)
{
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [] (unsigned char lhs, unsigned char rhs) {
                      return std::tolower(lhs) == std::tolower(rhs);
                    });
}

inline std::optional<std::string>
validateNativeProviderAssignmentPayload(
  const std::vector<NativeModelRunnerSpec>& runnerSpecs,
  const std::string& role,
  const ndn::Buffer& assignmentPayload)
{
  const auto fields = parseNativeProviderAssignmentFields(assignmentPayload, role);
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
  if (!expectedDigest.empty() &&
      !nativeProviderDigestEquals(expectedDigest, fragmentDigest)) {
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
  std::vector<ExecutionEvidence> executionEvidence;
};

NativeProviderCollaborationRuntime
makeNativeProviderCollaborationRuntime(NativeProviderHandlerConfig config);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
