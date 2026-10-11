#ifndef NDNSF_DISTRIBUTED_REPO_ARTIFACT_REPOSITORY_API_HPP
#define NDNSF_DISTRIBUTED_REPO_ARTIFACT_REPOSITORY_API_HPP

#include "ndnsf-distributed-repo/ArtifactTypes.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <ndn-cxx/name.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndn_service_framework {
class ServiceUser;
}

namespace ndnsf_distributed_repo {

struct ArtifactReplicaCommit;

struct InAppRepoConfig
{
  std::filesystem::path durableRoot;
  std::string ownerId;
  ArtifactLimits limits;
  uint64_t availabilityFreshnessMs = 5000;
};

struct PersistentRepoClientConfig
{
  ArtifactLimits limits;
  ndn::Name servicePrefix{"/NDNSF/DistributedRepo/Object/v1"};
  std::vector<ndn::Name> providerScope;
  int ackCollectionTimeMs = 200;
  int requestTimeoutMs = 5000;
  int operationWaitTimeoutMs = 5000;
  int fetchFailureCooldownMs = 3000;
  size_t maxFetchHealthEntries = 1024;
};

struct RepoCandidate
{
  std::string repoNode;
  ndn::Name providerIdentity;
  ndn::Name servicePrefix;
  ArtifactCapability capability;
  uint64_t observedAtMs = 0;
  std::string ackWireDigest;
  std::string signerKeyLocator;
};

struct SelectedRepoTarget
{
  std::string repoNode;
  ndn::Name providerIdentity;
  ndn::Name servicePrefix;
};

struct RepoCandidateSet
{
  std::vector<RepoCandidate> candidates;
  uint64_t observedAtMs = 0;
  ndn::Name requestId;
};

struct SelectedRepoSet
{
  std::vector<SelectedRepoTarget> targets;
  std::string seedRepoId;
  RepoWriteConsistency consistency = RepoWriteConsistency::All;

  void validate(const ndn::Name& expectedServicePrefix,
                const ArtifactLimits& limits = {}) const;
};

struct ArtifactHandle
{
  uint32_t schemaVersion = 1;
  ArtifactReference reference;
  std::vector<std::string> commitReceiptIds;
  std::vector<SelectedRepoTarget> selectedTargets;
  std::vector<ArtifactReplicaCommit> replicaCommits;
  std::string seedRepoId;
  std::string writeConsistency;
  std::string lifecycleMode;

  void validate(const ArtifactLimits& limits = {}) const;
  std::string toJson() const;
  static ArtifactHandle fromJson(const std::string& wire,
                                 const ArtifactLimits& limits = {});
};

struct ArtifactReplicaCommit
{
  std::string repoNode;
  std::string providerIdentity;
  std::string receiptId;
  std::string operationId;
  ArtifactReference reference;
  uint64_t generation = 0;
  std::string contentDigest;
  uint64_t persistedBytes = 0;
  uint64_t completedAtMs = 0;
  uint64_t locationVersion = 0;
  std::string routeState;
  std::string state;
  std::string failureReason;
  std::string forwardingHint;
};

struct ArtifactCommitReceipt
{
  std::string receiptId;
  std::string operationId;
  ArtifactReference reference;
  // In-App uses its first immutable local commit generation (1). This is not
  // the payload transfer/staging generation.
  uint64_t generation = 0;
  std::string lifecycleMode;
  std::string placementState;
  uint32_t requestedReplicas = 0;
  uint32_t requiredAcks = 0;
  uint32_t achievedReplicas = 0;
  std::vector<ArtifactReplicaCommit> replicaCommits;
  std::optional<ArtifactLocalCommitRecord> localCommit;
  std::string writeConsistency;
  std::string state;
  uint64_t completedAtMs = 0;
  uint32_t schemaVersion = 1;

  void validate(const ArtifactLimits& limits = {}) const;
  std::string toJson() const;
  static ArtifactCommitReceipt fromJson(const std::string& wire,
                                        const ArtifactLimits& limits = {});
};

std::string makeArtifactCommitReceiptId(const ArtifactCommitReceipt& receipt);

struct ArtifactReplicaResult
{
  std::string repoNode;
  std::string providerIdentity;
  std::string state;
  std::string receiptId;
  std::string errorCode;
};

struct ArtifactPublishResult
{
  ArtifactReference reference;
  std::string operationId;
  uint32_t requestedReplicas = 0;
  uint32_t achievedReplicas = 0;
  std::vector<ArtifactReplicaResult> replicas;
  bool deduplicated = false;
  bool resumed = false;
  double totalDurationMs = 0;
  std::map<std::string, double> phaseDurationsMs;
  std::optional<ArtifactHandle> handle;
  std::optional<ArtifactCommitReceipt> receipt;
  std::optional<PublishOperationHandle> operationHandle;
  std::optional<PublishOperationStatus> operationStatus;

  void validate(const ArtifactLimits& limits = {}) const;
};

struct ArtifactFetchAttempt
{
  std::string repoNode;
  std::string providerIdentity;
  bool wasCoolingDownAtSelection = false;
  uint64_t locationVersion = 0;
  uint64_t requestTimeoutBudgetMs = 0;
  uint64_t elapsedMs = 0;
  uint64_t bytesTransferred = 0;
  uint64_t bytesVerified = 0;
  std::string outcome;
  std::string failureClass;
};

struct ArtifactFetchResult
{
  ArtifactReference reference;
  std::string repoNode;
  std::string providerIdentity;
  uint64_t bytesTransferred = 0;
  uint64_t bytesVerified = 0;
  uint64_t totalElapsedMs = 0;
  std::vector<ArtifactFetchAttempt> attempts;
};

class ArtifactFetchError final : public std::runtime_error
{
public:
  ArtifactFetchError(std::string message, ArtifactFetchResult result);
  const ArtifactFetchResult& result() const noexcept;

private:
  ArtifactFetchResult m_result;
};

/**
 * Operation-level native Repo API. In-App instances own one durable local
 * root and have no network service, identity, or endpoint configuration.
 * Persistent instances borrow ServiceUser and its Face. Persistent calls
 * block while awaiting ServiceUser callbacks. The caller's Face event loop
 * must already run on a different thread; calls must not run on that Face
 * thread, and this API never starts or drives the loop. Keep ServiceUser and
 * Face alive while this API exists. A request timeout bounds the caller's
 * wait but does not stop the Face loop; stop and join that loop before
 * destroying ServiceUser, then destroy the API before ServiceUser.
 */
class ArtifactRepositoryApi
{
public:
  static ArtifactRepositoryApi
  makeInApp(InAppRepoConfig config);

  static ArtifactRepositoryApi
  makePersistent(ndn_service_framework::ServiceUser& serviceUser,
                 PersistentRepoClientConfig config = {});

  ~ArtifactRepositoryApi();
  ArtifactRepositoryApi(ArtifactRepositoryApi&&) noexcept;
  ArtifactRepositoryApi& operator=(ArtifactRepositoryApi&&) noexcept;

  ArtifactRepositoryApi(const ArtifactRepositoryApi&) = delete;
  ArtifactRepositoryApi& operator=(const ArtifactRepositoryApi&) = delete;

  /** Store file bytes under a canonical Producer-owned logical reference.
   * This API does not publish a manifest graph or original signed Data packets.
   * Use raw-file-v1 with an empty rootManifestName when no graph exists.
   * It must not assert a nonexistent independently published manifest name.
   * Persistent transfer uses the existing Core invocation naming contract.
   */
  ArtifactPublishResult
  publishFile(const std::string& sourcePath,
              const ArtifactReference& reference);

  ArtifactPublishResult
  publishFileUntil(const std::string& sourcePath,
                   const ArtifactReference& reference,
                   std::chrono::steady_clock::time_point deadline);

  ArtifactPublishResult
  publishFile(const std::string& sourcePath,
              const ArtifactReference& reference,
              const SelectedRepoSet& selected, int timeoutMs);

  PublishOperationHandle
  beginPublishFile(const std::string& sourcePath,
                   const ArtifactReference& reference,
                   const SelectedRepoSet& selected,
                   const std::string& idempotencyKey,
                   int timeoutMs);

  PublishOperationHandle
  beginArchive(const ArtifactRepositoryApi& inAppSource,
               const ArtifactHandle& handle,
               const SelectedRepoSet& selected,
               const std::string& idempotencyKey,
               int timeoutMs);

  PublishOperationStatus
  getPublishStatus(const PublishOperationHandle& handle,
                   int timeoutMs = 0) const;

  RepoCandidateSet
  discoverRepoCandidates(const ArtifactCapabilityRequirements& requirements,
                         int timeoutMs) const;

  ArtifactPublishResult
  archive(const ArtifactRepositoryApi& inAppSource,
          const ArtifactHandle& handle,
          const SelectedRepoSet& selected, int timeoutMs);

  void
  fetchFile(const ArtifactReference& reference,
            const std::string& destinationPath) const;

  ArtifactFetchResult
  fetchFile(const ArtifactHandle& handle,
            const std::string& destinationPath, int timeoutMs) const;

  std::optional<ArtifactLocalCommitRecord>
  lookupLocalCommit(const ArtifactReference& reference) const;

  ArtifactCommitReceipt
  getReceipt(const std::string& receiptId,
             std::optional<ArtifactHandle> handle = std::nullopt,
             int timeoutMs = 0) const;

  AvailabilityReport
  checkAvailability(const ArtifactHandle& handle,
                    const std::string& probeKind = "metadata",
                    int timeoutMs = 0,
                    uint64_t maxAgeMs = 0) const;

  std::optional<ArtifactLocalCommitRecord>
  lookupLocalCommitUntil(
    const ArtifactReference& reference,
    std::chrono::steady_clock::time_point deadline) const;

  void
  fetchFileUntil(const ArtifactReference& reference,
                 const std::string& destinationPath,
                 std::chrono::steady_clock::time_point deadline) const;

private:
  struct Impl;

  PublishOperationHandle
  beginPublishFileImpl(const std::string& sourcePath,
                       const ArtifactReference& reference,
                       const SelectedRepoSet& selected,
                       const std::string& idempotencyKey,
                       std::chrono::steady_clock::time_point deadline,
                       PublishOperationStatus* acceptedStatus);

  PublishOperationStatus
  getPublishStatusUntil(const PublishOperationHandle& handle,
                        std::chrono::steady_clock::time_point deadline,
                        int requestTimeoutMs) const;

  explicit ArtifactRepositoryApi(std::unique_ptr<Impl> impl);

private:
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_ARTIFACT_REPOSITORY_API_HPP
