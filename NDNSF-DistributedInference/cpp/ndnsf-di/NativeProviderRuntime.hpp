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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

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
};

/** Provider-local retained state for explicit multi-turn conversations. */
class ConversationStateStore
{
public:
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
                 std::uint64_t nowMs);

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
                        std::uint64_t nowMs);

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
    std::uint64_t prefetchGeneration = 0;
    std::uint64_t lastAccess = 0;
    std::uint64_t lastUsedAtMs = 0;
    std::uint64_t expiresAtMs = 0;
  };

  static std::string
  keyFor(const ConversationStateBinding& binding);

  static bool
  compatible(const ConversationStateBinding& left,
             const ConversationStateBinding& right);

  bool
  evictGpuUntilFits(std::size_t incomingBytes,
                    const std::string& replacingKey);

  bool
  evictHostUntilFits(std::size_t incomingBytes,
                     const std::string& replacingKey);

  void
  removeEntry(const std::string& key,
              bool countCleanup);

private:
  const std::size_t m_gpuMaxBytes;
  const std::size_t m_gpuMaxEntries;
  const std::size_t m_hostMaxBytes;
  const std::size_t m_hostMaxEntries;
  const std::uint64_t m_retentionMs;
  mutable std::mutex m_mutex;
  std::map<std::string, Entry> m_entries;
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

  bool
  hasRunner(const std::string& role) const;

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

  bool
  promoteDecodeStateToConversation(const std::string& sessionId,
                                   const RoleSpec& role,
                                   ConversationStateBinding binding,
                                   std::uint64_t nowMs);

  bool
  stageDecodeStatePromotion(const std::string& sessionId,
                            const RoleSpec& role,
                            ConversationStateBinding binding,
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
    std::function<void()> executionGuard);

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
  };

  static std::string
  conversationPromotionKey(const ConversationStateBinding& binding);

  mutable std::mutex m_mutex;
  std::map<std::string, std::shared_ptr<NativeModelRunner>> m_runners;
  std::map<std::string, NativeModelRunnerSpec> m_runnerSpecs;
  KvStateStore m_decodeStateStore;
  ConversationStateStore m_conversationStateStore;
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
