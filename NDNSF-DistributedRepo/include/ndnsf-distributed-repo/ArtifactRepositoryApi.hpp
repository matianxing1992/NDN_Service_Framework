#ifndef NDNSF_DISTRIBUTED_REPO_ARTIFACT_REPOSITORY_API_HPP
#define NDNSF_DISTRIBUTED_REPO_ARTIFACT_REPOSITORY_API_HPP

#include "ndnsf-distributed-repo/artifact-client-types.hpp"

namespace ndnsf_distributed_repo {

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

  /** Read one committed byte range of an In-App artifact. The bytes are the exact
   *  stored content for that range; this is not a whole-file verification, so callers
   *  must not report a verified digest from it. */
  std::vector<uint8_t>
  readCommittedFileRange(const ArtifactReference& reference, uint64_t offsetBytes,
                uint64_t lengthBytes,
                std::chrono::steady_clock::time_point deadline =
                  std::chrono::steady_clock::time_point::max()) const;

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
