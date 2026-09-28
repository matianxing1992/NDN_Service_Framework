#ifndef NDNSF_DISTRIBUTED_REPO_ARTIFACT_REPOSITORY_API_HPP
#define NDNSF_DISTRIBUTED_REPO_ARTIFACT_REPOSITORY_API_HPP

#include "ndnsf-distributed-repo/ArtifactTypes.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <ndn-cxx/name.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
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
};

struct PersistentRepoClientConfig
{
  ArtifactLimits limits;
  ndn::Name servicePrefix{"/NDNSF/DistributedRepo/Object/v1"};
  std::vector<ndn::Name> providerScope;
  int ackCollectionTimeMs = 200;
  int requestTimeoutMs = 5000;
};

struct RepoCandidate
{
  std::string repoNode;
  ndn::Name providerIdentity;
  ndn::Name servicePrefix;
  ArtifactCapability capability;
  uint64_t observedAtMs = 0;
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

  void validate(const ArtifactLimits& limits = {}) const;
};

/**
 * Operation-level native Repo API. In-App instances own one durable local
 * root and have no network service, identity, or endpoint configuration.
 * Persistent calls block while awaiting ServiceUser callbacks. The caller's
 * Face event loop must already run on a different thread; calls must not run
 * on that Face thread, and this API never starts or drives the loop.
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

  ArtifactPublishResult
  publishFile(const std::string& sourcePath,
              const ArtifactReference& reference);

  ArtifactPublishResult
  publishFile(const std::string& sourcePath,
              const ArtifactReference& reference,
              const SelectedRepoSet& selected, int timeoutMs);

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

  void
  fetchFile(const ArtifactHandle& handle,
            const std::string& destinationPath, int timeoutMs) const;

  std::optional<ArtifactLocalCommitRecord>
  lookupLocalCommit(const ArtifactReference& reference) const;

private:
  struct Impl;

  explicit ArtifactRepositoryApi(std::unique_ptr<Impl> impl);

private:
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_ARTIFACT_REPOSITORY_API_HPP
