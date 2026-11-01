#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_RUNTIME_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_RUNTIME_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/DecodeStateIdentity.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationStateBinding.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ndnsf::di {

struct KvStateBinding
{
  std::string sessionId;
  std::string stage;
  std::uint64_t contextEpoch = 0;
  std::string modelDigest;
  std::string planDigest;
  std::string providerName;
  std::string providerBootId;
  std::uint64_t securityEpoch = 0;
  std::string requestId;
  std::uint64_t attemptEpoch = 0;
  std::string generationId;
  std::string stateSchemaDigest;
  // Complete adapter-certified identity. The older scalar members remain the
  // compact index/compatibility envelope while T013 migration is in progress;
  // when this value is present, equality of the compact key never authorizes
  // reuse without equality of the complete identity.
  std::optional<DecodeStateIdentityV1> exactIdentity;

  void validate() const;
  bool operator==(const KvStateBinding& other) const;
};

class KvStateStore
{
public:
  explicit KvStateStore(std::size_t maxBytes, std::size_t maxEntries = 128);

  void setProviderBootId(std::string providerBootId);
  bool put(KvStateBinding binding, TensorBundle state);
  std::optional<TensorBundle> lookup(const KvStateBinding& binding);
  // A deferred decode transition is intentionally invisible to ordinary
  // lookup, but conversation promotion must be able to consume that exact
  // request-local candidate after execution has succeeded.
  std::optional<TensorBundle> lookupCandidate(const KvStateBinding& binding);
  std::optional<TensorBundle> beginTransition(const KvStateBinding& predecessor);
  bool stageCandidate(std::optional<KvStateBinding> predecessor,
                      KvStateBinding candidate,
                      TensorBundle state);
  bool commitCandidate(const KvStateBinding& candidate);
  bool rollbackTransition(const KvStateBinding& binding);
  bool erase(const std::string& sessionId, const std::string& stage);
  void clear();

  std::size_t size() const;
  std::size_t usedBytes() const;
  std::size_t pinnedCount() const;
  std::size_t candidateCount() const;
  std::uint64_t evictionCount() const;
  std::uint64_t cleanupCount() const;

private:
  struct Entry
  {
    KvStateBinding binding;
    TensorBundle state;
    bool hasCommitted = true;
    bool pinned = false;
    std::optional<std::pair<KvStateBinding, TensorBundle>> candidate;
    std::uint64_t lastAccess = 0;
  };

  static std::string keyFor(const KvStateBinding& binding);
  static bool sameLineage(const KvStateBinding& left,
                          const KvStateBinding& right);
  static std::size_t entryBytes(const Entry& entry);
  static void wipeEntry(Entry& entry) noexcept;
  bool evictUntilFits(std::size_t incomingBytes,
                      const std::string& replacingKey);

private:
  const std::size_t m_maxBytes;
  const std::size_t m_maxEntries;
  mutable std::mutex m_mutex;
  std::map<std::string, Entry> m_entries;
  std::string m_providerBootId;
  std::size_t m_usedBytes = 0;
  std::uint64_t m_accessSequence = 0;
  std::uint64_t m_evictions = 0;
  std::uint64_t m_cleanups = 0;
};

struct ProviderDecodeStateSnapshot
{
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t commits = 0;
  std::uint64_t commitFailures = 0;
  std::uint64_t rollbacks = 0;
  std::uint64_t recomputes = 0;
  std::uint64_t evictions = 0;
  std::uint64_t cleanups = 0;
  std::size_t entries = 0;
  std::size_t bytes = 0;
  std::size_t pinnedEntries = 0;
  std::size_t candidates = 0;
};

enum class ConversationStateResidency
{
  GPU_RESIDENT,
  HOST_RESIDENT,
  EVICTED,
};

enum class ConversationStateLifecycle
{
  IDLE,
  PREFETCHING,
  PINNED,
  COMMITTING,
};

struct ConversationStateSnapshot
{
  std::size_t entries = 0;
  std::size_t committingEntries = 0;
  std::size_t gpuEntries = 0;
  std::size_t hostEntries = 0;
  std::size_t gpuBytes = 0;
  std::size_t hostBytes = 0;
  std::size_t pinnedEntries = 0;
  std::size_t prefetchingEntries = 0;
  std::uint64_t requestLocalReleases = 0;
  std::uint64_t stagedPromotions = 0;
  std::uint64_t promotions = 0;
  std::uint64_t promotionRollbacks = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
  std::uint64_t prefetchedEntries = 0;
  std::uint64_t prefetchBytes = 0;
  std::uint64_t prefetchLatencyMs = 0;
  std::uint64_t cleanups = 0;
  std::size_t saveEligibleEntries = 0;
  std::uint64_t saveBatchRowExportAttempts = 0;
};

/** Authenticated Provider context retained with one committed role receipt.
 * This is eligibility metadata only; it does not itself authorize export or
 * storage publication. Only the native Handler supplies the requester value.
 */
struct ConversationSaveMetadataV1
{
  std::string authenticatedRequester;
  ProviderConversationStateReceiptV1 receipt;
};

struct ConversationAdapterSnapshotLeaseV1
{
  ConversationStateBinding binding;
  NativeConversationStateHandleV1 state;
  std::shared_ptr<NativeModelRunner> runner;
private:
  std::shared_ptr<void> lifetime;
  friend class ConversationStateStore;
};

struct ConversationSaveBatchLimitsV1
{
  std::size_t maxEntries = 0;
  std::size_t maxMetadataBytes = 0;
  std::uint64_t maxTotalBytes = std::numeric_limits<std::uint64_t>::max();
};

enum class ConversationSaveBatchStatusV1
{
  Ready,
  Busy,
  IneligibleOrUnavailable,
  BoundsExceeded,
  Empty,
};

struct ConversationSaveBatchRowV1
{
  ConversationStateBinding binding;
  ConversationSaveMetadataV1 metadata;
  std::size_t serializedMetadataBytes = 0;
};

struct ConversationStateDrainSnapshotV1
{
  bool closing = false;
  bool complete = true;
  std::uint64_t activeSnapshots = 0;
  std::uint64_t activeUses = 0;
  std::uint64_t activePrefetches = 0;
  std::uint64_t activeTransitions = 0;
  std::uint64_t retainedGpuBytes = 0;
  std::uint64_t retainedHostBytes = 0;
  std::uint64_t cleanupFailures = 0;
  std::uint64_t unverifiedOccupancyEntries = 0;
  std::uint64_t unsafeCleanupEntries = 0;
  std::string reason;
};

struct ConversationStateUseLeaseV1
{
  ConversationStateBinding binding;
  std::optional<NativeConversationStateHandleV1> state;
  std::shared_ptr<NativeModelRunner> runner;
  std::optional<TensorBundle> bundle;
private:
  std::shared_ptr<void> lifetime;
  friend class ConversationStateStore;
  friend class NativeProviderRuntime;
};

enum class ImportedStateReleaseResult
{
  Released,
  AlreadyGone,
  Busy,
  CleanupUnconfirmed,
};

/** Provider-local retained state for explicit multi-turn conversations. */
class ConversationStateStore : public std::enable_shared_from_this<ConversationStateStore>
{
  struct ImportReservationToken;
  struct ExecutionAdmissionToken;
  struct SaveFenceToken;
public:
  // Local creation proof only; no serialization and no automatic parent erasure.
  class ImportOwnership
  {
  public:
    ImportOwnership(const ImportOwnership&) noexcept = default;
    ImportOwnership(ImportOwnership&&) noexcept = default;
    ImportOwnership& operator=(const ImportOwnership&) noexcept = default;
    ImportOwnership& operator=(ImportOwnership&&) noexcept = default;
  private:
    explicit ImportOwnership(std::shared_ptr<ImportReservationToken> token) noexcept
      : m_token(std::move(token)) {}
    std::shared_ptr<ImportReservationToken> m_token;
    friend class ConversationStateStore;
    friend class NativeProviderRuntime;
  };

  /**
   * Shared RAII admission for one authenticated conversation execution.
   * @return True while this handle still owns an admission registration.
   * Thread safety: copies share one token; the final destruction releases it.
   */
  class ExecutionAdmission
  {
  public:
    ExecutionAdmission() noexcept = default;
    ExecutionAdmission(const ExecutionAdmission&) noexcept = default;
    ExecutionAdmission(ExecutionAdmission&&) noexcept = default;
    ExecutionAdmission& operator=(const ExecutionAdmission&) noexcept = default;
    ExecutionAdmission& operator=(ExecutionAdmission&&) noexcept = default;
    bool valid() const noexcept { return static_cast<bool>(m_token); }
  private:
    explicit ExecutionAdmission(std::shared_ptr<ExecutionAdmissionToken> token) noexcept
      : m_token(std::move(token)) {}
    std::shared_ptr<ExecutionAdmissionToken> m_token;
    friend class ConversationStateStore;
    friend class NativeProviderRuntime;
  };

  /**
   * Shared RAII fence for a user-conversation, service, or Provider withdrawal.
   * @return True while this handle still owns a save-fence registration.
   * Thread safety: copies share one token; the final destruction releases it.
   */
  class SaveFence
  {
  public:
    SaveFence() noexcept = default;
    SaveFence(const SaveFence&) noexcept = default;
    SaveFence(SaveFence&&) noexcept = default;
    SaveFence& operator=(const SaveFence&) noexcept = default;
    SaveFence& operator=(SaveFence&&) noexcept = default;
    bool valid() const noexcept { return static_cast<bool>(m_token); }
  private:
    explicit SaveFence(std::shared_ptr<SaveFenceToken> token) noexcept
      : m_token(std::move(token)) {}
    std::shared_ptr<SaveFenceToken> m_token;
    friend class ConversationStateStore;
    friend class NativeProviderRuntime;
  };

  /**
   * Move-only ownership of one all-or-nothing committed snapshot set.
   * Rows are immutable observations; the Store privately retains the exact
   * snapshot leases and fence which authorize exporting them.
   */
  class SaveBatch
  {
  public:
    SaveBatch(const SaveBatch&) = delete;
    SaveBatch& operator=(const SaveBatch&) = delete;
    SaveBatch(SaveBatch&&) noexcept = default;
    SaveBatch& operator=(SaveBatch&&) noexcept = default;
    std::size_t size() const noexcept { return m_rows.size(); }
    const std::vector<ConversationSaveBatchRowV1>& rows() const noexcept
    {
      return m_rows;
    }
  private:
    SaveBatch() = default;
    std::weak_ptr<ConversationStateStore> m_owner;
    SaveFence m_fence;
    std::uint64_t m_generation = 0;
    std::vector<ConversationSaveBatchRowV1> m_rows;
    std::vector<ConversationAdapterSnapshotLeaseV1> m_snapshots;
    friend class ConversationStateStore;
    friend class NativeProviderRuntime;
  };

  struct SaveBatchResult
  {
    ConversationSaveBatchStatusV1 status =
      ConversationSaveBatchStatusV1::IneligibleOrUnavailable;
    std::optional<SaveBatch> batch;
    std::size_t metadataBytes = 0;
    std::uint64_t totalBytes = 0;
    std::vector<ConversationSaveBatchRowV1> affectedRows;
  };

  ~ConversationStateStore();
  explicit ConversationStateStore(
    std::size_t gpuMaxBytes,
    std::size_t gpuMaxEntries,
    std::size_t hostMaxBytes = 512ULL * 1024ULL * 1024ULL,
    std::size_t hostMaxEntries = 128,
    std::uint64_t retentionMs = 300'000);

  bool
  setProviderBinding(std::string providerIdentity,
                     std::string providerBootId,
                     std::uint64_t cacheEpoch);

  /**
   * Admit one authenticated request for a conversation not currently fenced.
   * @param authenticatedRequester Authenticated Core requester identity URI.
   * @param serviceName Authenticated service URI.
   * @param conversationId Conversation from the authenticated turn binding.
   * @return A shared admission handle, or empty if a matching save fence is active.
   * @throws std::invalid_argument If a name or conversation identifier is invalid.
   * Thread safety: linearized with save-fence installation under the Store mutex.
   */
  std::optional<ExecutionAdmission> tryAcquireConversationExecution(
    const std::string& authenticatedRequester,
    const std::string& serviceName,
    const std::string& conversationId);

  /** Begin a fence affecting only one authenticated owner's conversation. */
  std::optional<SaveFence> beginConversationSaveFence(
    const std::string& authenticatedRequester,
    const std::string& serviceName,
    const std::string& conversationId);

  /** Begin a trusted local fence affecting every conversation of one service. */
  std::optional<SaveFence> beginServiceWithdrawalSaveFence(
    const std::string& serviceName, const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration);

  /** Begin a trusted local fence affecting this Provider's entire Store. */
  std::optional<SaveFence> beginProviderWithdrawalSaveFence(
    const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration);

  /** Borrow one service scope from a live Provider withdrawal fence.
   * The child retains its parent and uses its own operation/generation binding. */
  std::optional<SaveFence> borrowServiceWithdrawalSaveFence(
    const SaveFence& parent, const std::string& serviceName,
    const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration);

  bool matchesServiceWithdrawalSaveFence(
    const SaveFence& fence, const std::string& serviceName,
    const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration) const noexcept;

  /** Borrow one conversation scope from a live local withdrawal fence.
   * Possession of the parent fence is required; the borrowed fence retains it
   * until its save batch and all snapshot leases have settled. */
  std::optional<SaveFence> borrowConversationWithdrawalSaveFence(
    const SaveFence& parent, const std::string& authenticatedRequester,
    const std::string& serviceName, const std::string& conversationId,
    const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration);

  /** Observe whether all earlier matching admissions have settled. */
  bool isSaveFenceQuiescent(const SaveFence& fence) const noexcept;

  /** Atomically pin the latest committed epoch per conversation in a quiescent fence scope. */
  SaveBatchResult tryAcquireCommittedSaveBatch(
    const SaveFence& fence, ConversationSaveBatchLimitsV1 limits,
    std::uint64_t nowMs);

  bool
  promote(std::string originRequestId,
          std::string role,
          ConversationStateBinding binding,
          TensorBundle state,
          std::uint64_t nowMs);

  /** Stage a request-local state under its successor conversation identity.
   * The entry remains COMMITTING and is invisible to lookup/resolve until an
   * authenticated aggregate checkpoint commits every selected role. */
  bool
  stagePromotion(std::string originRequestId,
                 std::string role,
                 ConversationStateBinding binding,
                 TensorBundle state,
                 std::uint64_t nowMs,
                 std::optional<ConversationSaveMetadataV1> saveMetadata = std::nullopt,
                 const ExecutionAdmission* admission = nullptr);

  /**
   * Stage an adapter-owned state transaction.  Unlike the legacy overload,
   * this path stores only an opaque reference and byte accounting; the
   * runner remains the sole owner of the actual device/host buffers.
   */
  bool
  stageAdapterPromotion(std::string originRequestId,
                        std::string role,
                        ConversationStateBinding binding,
                        NativeConversationStateHandleV1 state,
                        std::shared_ptr<NativeModelRunner> runner,
                        std::uint64_t nowMs,
                        std::optional<ConversationSaveMetadataV1> saveMetadata = std::nullopt,
                        const ExecutionAdmission* admission = nullptr);

  /** Admit an imported candidate without evicting or replacing existing state.
   * A successful admission transfers handle ownership to the store. */
  bool
  stageImportedAdapterState(std::string originRequestId,
                            std::string role,
                            ConversationStateBinding binding,
                            NativeConversationStateHandleV1 state,
                            std::shared_ptr<NativeModelRunner> runner,
                            std::uint64_t nowMs,
                            const std::string& authenticatedRequester,
                            const ExecutionAdmission* admission = nullptr);

  bool
  commitStagedPromotion(const ConversationStateBinding& binding);

  /** Commit a staged candidate and bind it to the aggregate User checkpoint
   * carried by the authenticated COMMIT control. */
  bool
  commitStagedPromotion(const ConversationStateBinding& binding,
                        const std::string& checkpointDigest);

  bool
  rollbackStagedPromotion(const ConversationStateBinding& binding);

  std::optional<TensorBundle>
  lookup(const ConversationStateBinding& binding,
         std::uint64_t nowMs);

  std::optional<NativeConversationStateHandleV1>
  adapterHandle(const ConversationStateBinding& binding,
                std::uint64_t nowMs) const;

  std::shared_ptr<NativeModelRunner>
  adapterRunner(const ConversationStateBinding& binding,
                std::uint64_t nowMs) const;

  /** Resolve a Selection's compact commitment to the Provider-owned exact
   * binding. The Selection cannot inject a DecodeStateIdentityV1. */
  std::optional<ConversationStateBinding>
  resolve(const ConversationStateReferenceV1& reference,
          std::uint64_t nowMs);

  /** Resolve only an uncommitted COMMITTING entry for the authenticated
   * Provider commit/rollback control path. */
  std::optional<ConversationStateBinding>
  resolveStaged(const ConversationStateReferenceV1& reference,
                std::uint64_t nowMs);

  void
  noteRequestLocalRelease();

  bool
  pin(const ConversationStateBinding& binding,
      std::uint64_t nowMs);

  bool
  unpin(const ConversationStateBinding& binding);

  bool
  pauseToHost(const ConversationStateBinding& binding,
              std::uint64_t nowMs);

  std::future<bool>
  prefetchToGpu(const ConversationStateBinding& binding,
                std::uint64_t nowMs);

  /** Cancel an in-flight host-to-device prefetch without releasing the entry. */
  bool
  cancelPrefetch(const ConversationStateBinding& binding);

  bool
  release(const ConversationStateBinding& binding);

  /** Remove expired committed or staged entries and return the count removed. */
  std::size_t
  cleanupExpired(std::uint64_t nowMs);

  void
  clear();

  std::optional<ConversationAdapterSnapshotLeaseV1>
  acquireAdapterSnapshot(const ConversationStateBinding& binding,
                         std::uint64_t nowMs);

  std::optional<ConversationStateUseLeaseV1>
  acquireConversationUse(const ConversationStateBinding& binding,
                         std::uint64_t nowMs);

  ConversationStateDrainSnapshotV1
  drainSnapshot() const;

  bool
  retryDeferredCleanup();

  ConversationStateSnapshot
  snapshot() const;

private:
  struct Entry
  {
    ConversationStateBinding binding;
    TensorBundle state;
    std::optional<NativeConversationStateHandleV1> adapterState;
    std::shared_ptr<NativeModelRunner> adapterRunner;
    ConversationStateResidency residency = ConversationStateResidency::GPU_RESIDENT;
    ConversationStateLifecycle lifecycle = ConversationStateLifecycle::IDLE;
    std::size_t logicalBytes = 0;
    std::size_t allocatedBytes = 0;
    std::uint32_t pinCount = 0;
    bool importedCandidate = false;
    std::weak_ptr<ImportReservationToken> importAdmission;
    bool importReserved = false;
    bool importOccupancyUnverified = false;
    bool adapterCleanupUnsafe = false;
    bool snapshotOwned = false;
    std::uint64_t snapshotTokenId = 0;
    bool useOwned = false;
    bool cleanupFailed = false;
    bool operationActive = false;
    bool hostReserved = false;
    bool evictionRequested = false;
    bool rollbackRequested = false;
    std::optional<ConversationSaveMetadataV1> saveMetadata;
    bool saveEligible = false;
    std::uint64_t executionAdmissionId = 0;
    std::uint64_t activePrefetches = 0;
    bool prefetchGpuReserved = false;
    std::uint64_t prefetchGeneration = 0;
    std::uint64_t lastAccess = 0;
    std::uint64_t lastUsedAtMs = 0;
    std::uint64_t expiresAtMs = 0;
  };

  static std::string
  keyFor(const ConversationStateBinding& binding);

  // Callback-free side-index reconciliation; caller may hold Runtime mutex.
  bool retainsStagedBinding(const ConversationStateBinding& binding) const;
  friend class NativeProviderRuntime;

  bool commitStagedPromotionImpl(const ConversationStateBinding& binding,
    const std::string& checkpointDigest,
    const std::shared_ptr<ImportReservationToken>& requiredAdmission);
  bool finalizeSaveEligibility(const ConversationStateBinding& binding);
  ImportedStateReleaseResult releaseImportedState(
    const std::shared_ptr<ImportReservationToken>& token);
  std::optional<ConversationStateUseLeaseV1> claimConversationUseForExecution(
    ConversationStateUseLeaseV1 lease, const ConversationStateBinding& expected,
    std::uint64_t nowMs);
  std::shared_ptr<ImportReservationToken>
  reserveImportedState(ConversationStateBinding binding,
                       std::shared_ptr<NativeModelRunner> runner,
                       std::size_t bytes,
                       std::uint64_t nowMs,
                       const std::string& authenticatedRequester,
                       const ExecutionAdmission* admission);
  bool completeImportedState(const std::shared_ptr<ImportReservationToken>& token,
                             NativeConversationStateHandleV1 state,
                             const NativePortableKvLimitsV1& limits);
  void cancelImportedReservation(ImportReservationToken& token) noexcept;

  struct SnapshotToken;
  struct UseToken;
  struct PrefetchToken;
  enum class SaveScopeKind { Conversation, Service, Provider };
  struct AdmissionScope
  {
    std::string requester;
    std::string service;
    std::string conversation;
    std::uint64_t generation = 0;
  };
  struct SaveScope
  {
    SaveScopeKind kind = SaveScopeKind::Conversation;
    std::string requester;
    std::string service;
    std::string conversation;
    std::uint64_t generation = 0;
    std::string withdrawalOperationId;
    std::uint64_t registrationGeneration = 0;
  };
  bool isEntryFencedLocked(const ConversationStateBinding& binding,
                           const std::string& requester) const;
  bool saveFenceMatchesLocked(const SaveFence& fence) const noexcept;
  std::optional<ConversationAdapterSnapshotLeaseV1> snapshotForSaveBatch(
    const SaveBatch& batch, std::size_t index, std::uint64_t nowMs) const;
  bool executionAdmissionMatchesLocked(
    const ExecutionAdmission* admission,
    const ConversationStateBinding& binding,
    const std::string& authenticatedRequester,
    const std::optional<ConversationSaveMetadataV1>& saveMetadata) const;
  void releaseExecutionAdmission(std::uint64_t id) noexcept;
  void releaseSaveFence(std::uint64_t id) noexcept;
  static bool saveScopeBlocksAdmission(const SaveScope& fence,
                                       const AdmissionScope& admission) noexcept;
  static bool saveScopesOverlap(const SaveScope& left,
                                const SaveScope& right) noexcept;
  void releaseAdapterSnapshot(const ConversationStateBinding& binding,
                              std::uint64_t tokenId) noexcept;
  void releaseConversationUse(const ConversationStateBinding& binding) noexcept;
  void drainDeferredState(bool retryFailures = false) noexcept;

  static bool
  compatible(const ConversationStateBinding& left,
             const ConversationStateBinding& right);

  bool
  evictGpuUntilFits(std::size_t incomingBytes,
                    const std::string& replacingKey,
                    std::unique_lock<std::mutex>& lock);

  bool
  evictHostUntilFits(std::size_t incomingBytes,
                     const std::string& replacingKey,
                     std::unique_lock<std::mutex>& lock);

  bool
  removeEntry(const std::string& key,
              bool countCleanup,
              std::unique_lock<std::mutex>& lock,
              bool rollback = false);

private:
  const std::size_t m_gpuMaxBytes;
  const std::size_t m_gpuMaxEntries;
  const std::size_t m_hostMaxBytes;
  const std::size_t m_hostMaxEntries;
  const std::uint64_t m_retentionMs;
  mutable std::mutex m_mutex;
  std::map<std::string, Entry> m_entries;
  std::map<std::uint64_t, AdmissionScope> m_executionAdmissions;
  std::map<std::uint64_t, SaveScope> m_saveFences;
  std::uint64_t m_nextExecutionAdmissionId = 1;
  std::uint64_t m_nextSaveFenceId = 1;
  std::uint64_t m_nextSnapshotTokenId = 1;
  bool m_closing = false;
  bool m_drainRunning = false;
  bool m_preserveCommittedOnImportFailure = false;
  std::uint64_t m_activeTransitions = 0;
  std::uint64_t m_admissionGeneration = 0;
  struct PendingProviderBinding {
    std::string identity;
    std::string bootId;
    std::uint64_t epoch = 0;
  };
  std::optional<PendingProviderBinding> m_pendingProviderBinding;
  std::string m_providerIdentity;
  std::string m_providerBootId;
  std::uint64_t m_cacheEpoch = 0;
  std::size_t m_gpuBytes = 0;
  std::size_t m_hostBytes = 0;
  std::uint64_t m_accessSequence = 0;
  std::uint64_t m_requestLocalReleases = 0;
  std::uint64_t m_stagedPromotions = 0;
  std::uint64_t m_promotions = 0;
  std::uint64_t m_promotionRollbacks = 0;
  std::uint64_t m_hits = 0;
  std::uint64_t m_misses = 0;
  std::uint64_t m_evictions = 0;
  std::uint64_t m_prefetchedEntries = 0;
  std::uint64_t m_prefetchBytes = 0;
  std::uint64_t m_prefetchLatencyMs = 0;
  std::uint64_t m_cleanups = 0;
};

using ConversationStateImportOwnershipV1 = ConversationStateStore::ImportOwnership;

class NativeProviderRuntime
{
public:
  explicit NativeProviderRuntime(
    std::size_t workerCount = std::thread::hardware_concurrency(),
    std::size_t readyQueueCapacity = 1024,
    std::size_t decodeStateMaxBytes = 512ULL * 1024ULL * 1024ULL,
    std::size_t decodeStateMaxEntries = 128,
    std::size_t conversationGpuMaxBytes = 512ULL * 1024ULL * 1024ULL,
    std::size_t conversationGpuMaxEntries = 128,
    std::size_t conversationHostMaxBytes = 512ULL * 1024ULL * 1024ULL,
    std::size_t conversationHostMaxEntries = 128,
    std::uint64_t conversationRetentionMs = 300'000);

  void
  registerRunner(std::string role, std::shared_ptr<NativeModelRunner> runner);

  void
  registerRunner(NativeModelRunnerSpec spec,
                 std::shared_ptr<NativeModelRunner> runner);

  void
  registerRunner(std::string role, RoleRunner runner);

  /** Trusted host binding for lazy runners; does not register or retain a runner. */
  void
  bindConversationStateProvider(std::string providerIdentity,
                                std::string providerBootId,
                                std::uint64_t cacheEpoch);

  bool
  hasRunner(const std::string& role) const;

  /** Trusted local access; retains the registered instance after lookup unlocks. */
  std::shared_ptr<NativeModelRunner>
  registeredRunnerForRole(const std::string& role) const;

  std::future<ProviderRoleResult>
  executeRoleAsync(std::string sessionId,
                   RoleSpec role,
                   std::shared_ptr<DependencyIo> io,
                   std::map<std::string, TensorBundle> initialInputsByScope = {},
                   RoleExecutionContext::StreamEventSink eventSink = {},
                   std::function<void()> executionGuard = {});

  std::future<ProviderRoleResult>
  executePreparedRoleAsync(
    std::string sessionId,
    RoleSpec role,
    std::shared_ptr<DependencyIo> io,
    ProviderRoleWorker::NativeRunnerPreparation prepareRunner,
    std::map<std::string, TensorBundle> initialInputsByScope = {},
    RoleExecutionContext::StreamEventSink eventSink = {},
    std::function<void()> executionGuard = {});

  std::optional<ConversationStateUseLeaseV1> acquireConversationUse(
    const ConversationStateBinding& binding, std::uint64_t nowMs);

  std::future<ProviderRoleResult> executeRoleWithConversationUseAsync(
    std::string sessionId, RoleSpec role, std::shared_ptr<DependencyIo> io,
    ConversationStateUseLeaseV1 use,
    std::map<std::string, TensorBundle> initialInputsByScope = {},
    RoleExecutionContext::StreamEventSink eventSink = {},
    std::function<void()> executionGuard = {});

  std::future<std::shared_ptr<NativeModelRunner>>
  prepareRunnerAsync(
    ProviderRoleWorker::NativeRunnerPreparation prepareRunner,
    std::function<void()> executionGuard = {});

  ProviderRoleWorkerSnapshot
  snapshot() const;

  ProviderDecodeStateSnapshot
  decodeStateSnapshot() const;

  ConversationStateSnapshot
  conversationStateSnapshot() const;

  std::optional<ConversationStateStore::ExecutionAdmission>
  tryAcquireConversationExecution(const std::string& authenticatedRequester,
                                 const std::string& serviceName,
                                 const std::string& conversationId);

  std::optional<ConversationStateStore::SaveFence>
  beginConversationSaveFence(const std::string& authenticatedRequester,
                             const std::string& serviceName,
                             const std::string& conversationId);

  std::optional<ConversationStateStore::SaveFence>
  beginServiceWithdrawalSaveFence(const std::string& serviceName,
    const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration);

  std::optional<ConversationStateStore::SaveFence>
  beginProviderWithdrawalSaveFence(const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration);

  std::optional<ConversationStateStore::SaveFence>
  borrowServiceWithdrawalSaveFence(
    const ConversationStateStore::SaveFence& parent,
    const std::string& serviceName,
    const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration);

  bool matchesServiceWithdrawalSaveFence(
    const ConversationStateStore::SaveFence& fence,
    const std::string& serviceName,
    const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration) const noexcept;

  std::optional<ConversationStateStore::SaveFence>
  borrowConversationWithdrawalSaveFence(
    const ConversationStateStore::SaveFence& parent,
    const std::string& authenticatedRequester,
    const std::string& serviceName, const std::string& conversationId,
    const std::string& withdrawalOperationId,
    std::uint64_t registrationGeneration);

  bool isSaveFenceQuiescent(
    const ConversationStateStore::SaveFence& fence) const noexcept;

  ConversationStateStore::SaveBatchResult tryAcquireCommittedSaveBatch(
    const ConversationStateStore::SaveFence& fence,
    ConversationSaveBatchLimitsV1 limits, std::uint64_t nowMs);

  bool
  promoteDecodeStateToConversation(const std::string& sessionId,
                                   const RoleSpec& role,
                                   ConversationStateBinding binding,
                                   std::uint64_t nowMs);

  bool
  stageDecodeStatePromotion(const std::string& sessionId,
                            const RoleSpec& role,
                            ConversationStateBinding binding,
                            std::uint64_t nowMs,
                            std::optional<ConversationSaveMetadataV1> saveMetadata = std::nullopt,
                            const ConversationStateStore::ExecutionAdmission* admission = nullptr);

  /** Trusted local admission; false/throw leaves the handle caller-owned. */
  bool
  stageImportedConversationState(ConversationStateBinding binding,
                                 NativeConversationStateHandleV1 state,
                                 std::shared_ptr<NativeModelRunner> runner,
                                 std::uint64_t nowMs);

  /** Own the whole import attempt; reserve capacity before backend allocation. */
  bool
  importConversationState(const NativePortableKvSnapshotV1& snapshot,
                          ConversationStateBinding binding,
                          std::shared_ptr<NativeModelRunner> runner,
                          const NativePortableKvLimitsV1& limits,
                          std::uint64_t nowMs);

  std::optional<ConversationStateImportOwnershipV1> importConversationStateOwned(
    const NativePortableKvSnapshotV1& snapshot, ConversationStateBinding binding,
    std::shared_ptr<NativeModelRunner> runner, const NativePortableKvLimitsV1& limits,
    std::uint64_t nowMs,
    const std::string& authenticatedRequester = {},
    const ConversationStateStore::ExecutionAdmission* admission = nullptr);
  bool commitImportedConversationState(const ConversationStateImportOwnershipV1& ownership,
    const std::string& checkpointDigest);
  ImportedStateReleaseResult releaseImportedConversationState(
    const ConversationStateImportOwnershipV1& ownership);

  ConversationStateDrainSnapshotV1 conversationStateDrainSnapshot() const;
  bool retryConversationStateCleanup();

  /** Export an exact committed state while retaining its Store/runner owner. */
  NativePortableKvSnapshotV1
  exportConversationState(const ConversationStateBinding& binding,
                          const NativePortableKvLimitsV1& limits,
                          std::uint64_t nowMs);

  /** Export one row from an already pinned save batch without reacquiring it. */
  NativePortableKvSnapshotV1
  exportConversationState(const ConversationStateStore::SaveBatch& batch,
                          std::size_t index,
                          const NativePortableKvLimitsV1& limits,
                          std::uint64_t nowMs);

  bool
  commitStagedDecodeStatePromotion(const ConversationStateBinding& binding);

  bool
  commitStagedDecodeStatePromotion(const ConversationStateBinding& binding,
                                   const std::string& checkpointDigest);

  bool
  rollbackStagedDecodeStatePromotion(
    const ConversationStateBinding& binding);

  std::optional<TensorBundle>
  lookupConversationState(const ConversationStateBinding& binding,
                          std::uint64_t nowMs);

  std::optional<ConversationStateBinding>
  resolveConversationState(const ConversationStateReferenceV1& reference,
                           std::uint64_t nowMs);

  std::optional<ConversationStateBinding>
  resolveStagedConversationState(const ConversationStateReferenceV1& reference,
                                 std::uint64_t nowMs);

  bool
  pauseConversationStateToHost(const ConversationStateBinding& binding,
                               std::uint64_t nowMs);

  std::future<bool>
  prefetchConversationStateToGpu(const ConversationStateBinding& binding,
                                 std::uint64_t nowMs);

  bool
  cancelConversationStatePrefetch(const ConversationStateBinding& binding);

  bool
  pinConversationState(const ConversationStateBinding& binding,
                       std::uint64_t nowMs);

  bool
  unpinConversationState(const ConversationStateBinding& binding);

  bool
  releaseConversationState(const ConversationStateBinding& binding);

  bool
  commitDecodeStateTransition(const std::string& sessionId,
                              const RoleSpec& role);

  bool
  rollbackDecodeStateTransition(const std::string& sessionId,
                                const RoleSpec& role);

  bool
  releaseDecodeState(const std::string& sessionId,
                     const std::string& role);

private:
  std::future<ProviderRoleResult>
  executeRoleAsyncImpl(
    std::string sessionId,
    RoleSpec role,
    std::shared_ptr<DependencyIo> io,
    std::shared_ptr<NativeModelRunner> runner,
    ProviderRoleWorker::NativeRunnerPreparation prepareRunner,
    std::map<std::string, TensorBundle> initialInputsByScope,
    RoleExecutionContext::StreamEventSink eventSink,
    std::function<void()> executionGuard,
    std::optional<ConversationStateUseLeaseV1> conversationUse = std::nullopt);

  std::shared_ptr<NativeModelRunner>
  findRunner(const std::string& role) const;

  std::optional<NativeModelRunnerSpec>
  findRunnerSpec(const std::string& role) const;

private:
  struct StagedConversationPromotion
  {
    std::string sessionId;
    RoleSpec role;
    ConversationStateBinding binding;
    bool imported = false;
    std::weak_ptr<ConversationStateStore::ImportReservationToken> importAdmission;
    std::optional<ConversationStateStore::ExecutionAdmission> executionAdmission;
  };

  static std::string
  conversationPromotionKey(const ConversationStateBinding& binding);

  static std::string
  sessionRunnerKey(const std::string& sessionId,
                   const std::string& role,
                   std::uint64_t attemptEpoch);

  mutable std::mutex m_mutex;
  std::map<std::string, std::shared_ptr<NativeModelRunner>> m_runners;
  // A post-Selection runner is request-owned until its state is either
  // promoted into ConversationStateStore or released.  It must not be
  // replaced by the process-wide role runner during that handoff.
  std::map<std::string, std::shared_ptr<NativeModelRunner>> m_sessionRunners;
  std::map<std::string, NativeModelRunnerSpec> m_runnerSpecs;
  KvStateStore m_decodeStateStore;
  std::shared_ptr<ConversationStateStore> m_conversationStateStore;
  std::map<std::string, StagedConversationPromotion>
    m_stagedConversationPromotions;
  std::atomic<std::uint64_t> m_decodeStateHits{0};
  std::atomic<std::uint64_t> m_decodeStateMisses{0};
  std::atomic<std::uint64_t> m_decodeStateCommits{0};
  std::atomic<std::uint64_t> m_decodeStateCommitFailures{0};
  std::atomic<std::uint64_t> m_decodeStateRollbacks{0};
  std::atomic<std::uint64_t> m_decodeStateRecomputes{0};
  ProviderRoleWorker m_worker;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_RUNTIME_HPP
