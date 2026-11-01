#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvCheckpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvPublication.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeDurableStorageOwner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePortableKvCheckpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ExecutionLease.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ndnsf::di {

/**
 * Request-local monotonicity gate for authenticated conversation controls.
 * The provider receives the complete request-scoped history on each poll, so
 * validate the active role/turn binding before advancing its replay state.
 */
class NativeProviderControlSequenceGuard
{
public:
  enum class Decision
  {
    Accepted,
    Duplicate,
    Reordered,
    Unbound,
  };

  Decision
  observeBound(std::uint64_t sequence, bool bindingMatches)
  {
    return bindingMatches ? observe(sequence) : Decision::Unbound;
  }

  Decision
  observe(std::uint64_t sequence)
  {
    if (m_processed.count(sequence) != 0) {
      ++m_duplicateCount;
      return Decision::Duplicate;
    }
    if (m_hasHighest && sequence <= m_highest) {
      ++m_reorderedCount;
      return Decision::Reordered;
    }
    m_processed.insert(sequence);
    m_highest = sequence;
    m_hasHighest = true;
    return Decision::Accepted;
  }

  std::size_t
  duplicateCount() const noexcept
  {
    return m_duplicateCount;
  }

  std::size_t
  reorderedCount() const noexcept
  {
    return m_reorderedCount;
  }

private:
  std::set<std::uint64_t> m_processed;
  std::uint64_t m_highest = 0;
  bool m_hasHighest = false;
  std::size_t m_duplicateCount = 0;
  std::size_t m_reorderedCount = 0;
};

struct NativeProviderKvRecoveryConfigV1
{
  std::filesystem::path privateScratchRoot;
  std::vector<ndn::Name> repoForwardingHints;
  NativeProtectedKvCheckpointLimitsV1 protectedLimits;
  NativePortableKvCheckpointLimitsV1 portableLimits;
  std::uint64_t maxTransportEnvelopeBytes = 0;
  void validate() const;
};

/** Operator policy only; Selection cannot supply paths, limits or key owners. */
NativeProviderKvRecoveryConfigV1 nativeProviderKvRecoveryConfigFromJson(const std::string& wire);

struct NativeProviderHandlerConfig
{
  using ProviderGroupCoordinatorFactory = std::function<
    std::shared_ptr<ProviderGroupCoordinator>(
      ndn_service_framework::ServiceProvider::CollaborationContext&,
      const std::map<std::string, std::string>&)>;
  using RunnerPreparationFactory = std::function<NativeModelRunnerSpec(
    ndn_service_framework::ServiceProvider::CollaborationContext&,
    const NativeSelectionProjectionV3&,
    const std::shared_ptr<ProtectedRuntime>&)>;
  using RunnerReuseLookup = std::function<std::shared_ptr<NativeModelRunner>(
    const NativeSelectionProjectionV3&,
    const std::shared_ptr<ProtectedRuntime>&)>;
  using RunnerReusePublisher = std::function<void(
    const NativeSelectionProjectionV3&,
    const std::shared_ptr<ProtectedRuntime>&,
    const std::shared_ptr<NativeModelRunner>&)>;
  using ProtectedRuntimeFactory = std::function<std::shared_ptr<ProtectedRuntime>(
    ndn_service_framework::ServiceProvider::CollaborationContext&,
    const NativeSelectionProjectionV3&,
    const std::shared_ptr<ProviderGroupCoordinator>&)>;
  // Optional exact-name grant transport.  An empty callback keeps the
  // production NDN transport; deployment/test adapters may provide the same
  // bounded, cancellation-aware fetch without replacing the runtime factory.
  using ProtectedGrantFetcher = std::function<std::string(
    const std::string&, int, const std::function<bool()>&)>;
  using EpochCoordinatorCompletionObserver = std::function<void(
    const std::string&, const NativeEpochCoordinatorResult&)>;
  using NativeFailureObserver = std::function<void(
    const std::string&, const std::string&)>;
  using GenerationTextDecoderFactory = std::function<
    std::function<std::string(const std::vector<std::int64_t>&)>(
      const std::string& tokenizerDigest)>;
  using GenerationDecodersFactory = std::function<
    NativeGenerationTextDecoders(const std::string& tokenizerDigest)>;

  // Only explicitly configured native deployments may omit raw options.
  bool allowSelectionGenerationOptions = false;
  NativeExecutionPlan plan;
  NativeProviderAssignment assignment;
  std::shared_ptr<NativeModelRunnerFactory> runnerFactory;
  std::vector<NativeModelRunnerSpec> runnerSpecs;
  // Normal V3 path: invoked on a bounded Provider worker after Selection to
  // fetch/assemble/validate one exact role, then return its local ORT spec.
  RunnerPreparationFactory runnerPreparationFactory;
  // Optional Provider-process live-runner reuse.  The lookup is performed
  // only after the current Selection/protected guard is admitted; publication
  // happens after a successful inference, so a failed request cannot publish
  // a runner carrying partial request state.
  RunnerReuseLookup runnerReuseLookup;
  RunnerReusePublisher runnerReusePublisher;
  // Provider-owned gate for protected resident sessions.  The handler never
  // stores request content keys in this owner; it only receives a per-request
  // Use token through the prepared runner spec.
  std::shared_ptr<ProtectedResidentAuthority> protectedResidentAuthority;
  // Explicit rollback-only compatibility for old preassembled V3 fixtures.
  bool allowPreassembledV3Compatibility = false;
  std::string finalResponseScope = "final-response";
  // Required host identity. NativeInferenceProvider::serve rejects an empty
  // or non-matching name before installing a target; every served target on a
  // host therefore uses the same ServiceProvider identity and boot epoch.
  std::string localProviderName;
  std::string providerBootId;
  std::optional<NativeProviderKvRecoveryConfigV1> kvRecovery;
  std::string planDigest;
  // T011's Y-N-I mutation is injected at the native Provider boundary.  It
  // is empty for every ordinary deployment and cannot alter the authenticated
  // Selection projection or the production role assignment.
  std::string spec180YnMutation;
  // Sealed NDNSF-DI execution contract. The default is per-role,
  // dependency-driven execution; V1 is rollback-only and must be paired with
  // both legacy activation switches plus an exact assignment field.
  std::string executionPolicy = "DATA_DRIVEN_V2";
  // Request-scoped streamed pipeline mode.  Each local role executes once per
  // prefill/decode epoch through NativeModelRunner::run(); the terminal role
  // owns token events and the final response.  Disabled for legacy/unary
  // handlers unless the plan explicitly contains TOKEN_FEEDBACK edges.
  bool enableNativeEpochCoordinator = false;
  std::size_t maxGenerationEpochs = 0;
  std::string generationTokenInputName = "input_ids";
  std::vector<std::string> generationStateInputNames;
  std::vector<std::string> generationStateOutputNames;
  std::set<std::int64_t> generationEosTokenIds;
  std::string generationSamplingDigest;
  std::string generationSamplingMode = "Greedy";
  double generationSamplingTemperature = 0.0;
  std::size_t generationSamplingTopK = 1;
  double generationSamplingTopP = 1.0;
  double generationSamplingRepetitionPenalty = 1.0;
  std::uint64_t generationSamplingSeed = 1'750'001;
  std::vector<std::string> generationStopStrings;
  // Provider-local standalone tokenizer callback.  It is intentionally
  // injected by the adapter/deployment layer; Core never imports a model
  // framework or exposes tokenizer paths on the wire.
  std::function<std::string(const std::vector<std::int64_t>&)>
    generationTextDecoder;
  GenerationTextDecoderFactory generationTextDecoderFactory;
  GenerationDecodersFactory generationDecodersFactory;
  bool requireGenerationTextOutput = false;
  std::vector<std::int64_t> generationCommittedPrefixTokenIds;
  // User-generated request scope shared only with the selected Provider roles
  // and retained by the requester. Provider conversation Ready/receipt records
  // use this scope so the outer Data is Provider-signed and the payload remains
  // requester-encrypted without introducing a new NDNSF Core message type.
  std::string conversationStateKeyScope = "ndnsf-di-conversation-state-v1";
  bool requireExecutionAttemptBinding = false;
  // Timeout for authenticated inter-Provider dependency reads only. It is
  // independent from fetchTimeoutMs, which bounds readiness and control ACKs.
  int dependencyFetchTimeoutMs = 30000;
  int fetchTimeoutMs = 30000;
  // Match the maintained Python provider path. Exact V3 Data names carry
  // request/plan/role/tensor bindings, so the signed wire packet needs room
  // beyond the plaintext segment payload.
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
  // The production factory may be overlapped with protected-grant acquisition.
  // The handler joins the request-local coordinator before binding validation;
  // this only removes independent preparation latency from the critical path.
  // Keep disabled for factories whose callback uses mutable CollaborationContext
  // state beyond its immutable identity/assignment getters.
  bool overlapGroupCoordinatorFactory = false;
  // Required for protected V3 roles. The factory fetches and verifies the
  // Selection-referenced KeyGrant, unwraps it inside the Provider boundary,
  // installs exact dataflow/capability bindings, and returns GrantVerified.
  ProtectedRuntimeFactory protectedRuntimeFactory;
  ProtectedGrantFetcher protectedGrantFetcher;
  std::size_t workerCount = 1;
  std::size_t workerQueueCapacity = 1024;
  // Finite per-host KV retention policy; authenticated turn deadlines may
  // shorten it. Valid range: 1..3600000 ms. No per-turn sliding extension.
  std::uint64_t conversationRetentionMs = 300'000;
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
  // Optional observability seams. They do not participate in execution or
  // error handling; tests and operators may use them to require every local
  // epoch coordinator to terminate cleanly instead of accepting a final-role
  // Response while an upstream role fails asynchronously.
  std::shared_ptr<EpochCoordinatorCompletionObserver>
    epochCoordinatorCompletionObserver;
  std::shared_ptr<NativeFailureObserver> nativeFailureObserver;
};

struct NativeProviderTimeoutBudget
{
  int dependencyFetchMs = 30000;
  int readinessMs = 30000;
  int conversationControlMs = 30000;
};

NativeProviderTimeoutBudget
nativeProviderTimeoutBudget(const NativeProviderHandlerConfig& config);

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
                                    const std::string& selectedRole = "",
                                    const NativeSelectionProjectionV3* validatedProjection = nullptr);

bool
nativeRequestContractDigestMatches(const std::string& expectedDigest,
                                   const ndn::Buffer& requestPayload);

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
  const ndn::Buffer& assignmentPayload,
  const NativeSelectionProjectionV3* validatedProjection = nullptr)
{
  const auto fields = parseNativeProviderAssignmentFields(assignmentPayload, role, validatedProjection);
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
  // Worker-only protected-KV publisher bound to this host's existing
  // ServiceProvider. The Owner resolver and original-packet store are supplied
  // per call by the authenticated handoff owner. Pass the Core-authenticated
  // requester identity from the targeted-handler invocation; the callback
  // rejects an Owner context bound to any other identity.
  std::function<NativeProtectedKvPublicationV1(
    const ndn::Name& authenticatedOwner,
    // Copy these identities from Core's authenticated targeted-handler call;
    // the publisher builds its internal PreparedServiceRequest from them.
    const ndn::Name& serviceName,
    const ndn::Name& requestId,
    const std::filesystem::path&,
    const NativeProtectedKvCheckpointReferenceV1&,
    const NativeDurableStorageOwner&,
    std::shared_ptr<ndn_service_framework::EncryptedLargeDataRangeStore>,
    const NativeProtectedKvCheckpointLimitsV1&,
    const std::function<void()>&)> publishProtectedKvCheckpoint;
  std::function<ProviderRoleWorkerSnapshot()> capacitySnapshot;
  std::function<ProviderDecodeStateSnapshot()> decodeStateSnapshot;
  std::function<ConversationStateSnapshot()> conversationStateSnapshot;
  // Trusted save owner; this closure retains the actual Handler Runtime.
  std::function<NativePortableKvSnapshotV1(const ConversationStateReferenceV1&,
    const NativePortableKvLimitsV1&, std::uint64_t)> exportCommittedConversationState;
  // Event owners use these same-Runtime operations to fence admissions,
  // atomically pin the affected committed set and export rows without
  // reacquiring snapshot ownership.
  std::function<std::optional<ConversationStateStore::SaveFence>(
    const std::string&, const std::string&, const std::string&)>
      beginConversationSaveFence;
  std::function<std::optional<ConversationStateStore::SaveFence>(
    const std::string&, const std::string&, std::uint64_t)> beginServiceWithdrawalSaveFence;
  std::function<std::optional<ConversationStateStore::SaveFence>(
    const std::string&, std::uint64_t)>
    beginProviderWithdrawalSaveFence;
  std::function<std::optional<ConversationStateStore::SaveFence>(
    const ConversationStateStore::SaveFence&, const std::string&,
    const std::string&, std::uint64_t)>
    borrowServiceWithdrawalSaveFence;
  std::function<bool(const ConversationStateStore::SaveFence&,
    const std::string&, const std::string&, std::uint64_t)>
    matchesServiceWithdrawalSaveFence;
  std::function<std::optional<ConversationStateStore::SaveFence>(
    const ConversationStateStore::SaveFence&, const std::string&,
    const std::string&, const std::string&, const std::string&,
    std::uint64_t)>
    borrowConversationWithdrawalSaveFence;
  std::function<bool(const ConversationStateStore::SaveFence&)>
    isSaveFenceQuiescent;
  std::function<ConversationStateStore::SaveBatchResult(
    const ConversationStateStore::SaveFence&,
    ConversationSaveBatchLimitsV1, std::uint64_t)>
      tryAcquireCommittedSaveBatch;
  std::function<NativePortableKvSnapshotV1(
    const ConversationStateStore::SaveBatch&, std::size_t,
    const NativePortableKvLimitsV1&, std::uint64_t)>
      exportCommittedSaveBatchRow;
  std::vector<ExecutionEvidence> executionEvidence;
};

/** Effective resource policy frozen for one authenticated handoff event.
 * Values come from the Provider operator/Owner resolver, not from the wire's
 * parser ceiling. Zero/unconfigured values keep handoff disabled. */
struct NativeConversationHandoffPolicyV1
{
  bool enabled = false;
  std::size_t maxConcurrentEvents = 0;
  std::size_t maxRolesPerEvent = 0;
  std::size_t maxConcurrentRoleSlots = 0;
  std::size_t maxPacketCountPerRole = 0;
  std::uint64_t maxPacketSetWireBytesPerRole = 0;
  std::uint64_t maxAggregateBytes = 0;
  std::uint64_t maxScratchBytesPerEvent = 0;
  std::uint64_t scratchQuotaBytes = 0;
  std::uint64_t maxSaveDurationMs = 0;
  std::filesystem::path scratchRoot;
  std::string effectiveDigest;
};

/** Authority and packet-store inputs for one authenticated handoff event.
 * Resolve these for each Core targeted invocation; never retain another
 * requester's grant resolver or store in Provider host-wide state. */
struct NativeConversationHandoffOwnerContextV1
{
  // Bind an injected network store to this operation's existing absolute
  // deadline before any publisher starts. This grants no storage authority.
  std::function<void(std::chrono::steady_clock::time_point)> bindPublicationDeadline;
  ndn::security::Certificate ownerEncryptionCertificate;
  NativeDurableStorageOwner durableStorageOwner;
  std::shared_ptr<ndn_service_framework::EncryptedLargeDataRangeStore> originalPacketStore;
  ConversationSaveBatchLimitsV1 saveBatchLimits;
  NativePortableKvCheckpointLimitsV1 portableLimits;
  NativeProtectedKvCheckpointLimitsV1 protectedCheckpointLimits;
  NativeConversationHandoffPolicyV1 policy;
  // Per-role route reported with the exact Repo grant challenge. Resolve this
  // only for the authenticated Owner; the Owner compares every route with its
  // own pinned trust policy before issuing any Repo grant.
  std::map<std::string, NativeDurableStorageGrantRepoTargetV1> repoTargetByRole;
};

// Implementations must verify the returned certificate's signature and
// validity against the Provider's configured trust profile.
using NativeConversationHandoffOwnerContextResolverV1 =
  std::function<NativeConversationHandoffOwnerContextV1(
    const ndn::Name& authenticatedOwner,
    const ndn::Name& serviceName,
    const std::string& operationId)>;

NativeConversationHandoffOwnerContextV1
resolveNativeConversationHandoffOwnerContextV1(
  const NativeConversationHandoffOwnerContextResolverV1& resolver,
  const ndn::Name& authenticatedOwner,
  const ndn::Name& serviceName,
  const std::string& operationId);

NativeProviderCollaborationRuntime
makeNativeProviderCollaborationRuntime(NativeProviderHandlerConfig config);

void bindNativeProviderDurableKvPublisher(
  NativeProviderCollaborationRuntime& runtime,
  std::shared_ptr<ndn_service_framework::ServiceProvider> producer);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_HANDLER_HPP
