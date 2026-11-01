#ifndef NDNSF_DISTRIBUTED_REPO_FILESYSTEM_ARTIFACT_STORE_HPP
#define NDNSF_DISTRIBUTED_REPO_FILESYSTEM_ARTIFACT_STORE_HPP

#include "ndnsf-distributed-repo/RepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace ndnsf_distributed_repo {

enum class RepoUserStorageWriteState { Reserved, Committed, Cancelled };

struct RepoUserStorageWrite
{
  std::string operationId;
  RepoUserStorageGrant grant;
  RepoUserStorageObject object;
  RepoUserStorageWriteState state = RepoUserStorageWriteState::Reserved;
  uint64_t reservedBytes = 0;
  uint64_t createdAtMs = 0;
  uint64_t updatedAtMs = 0;
};

struct RepoUserStorageCleanupPeers
{
  RepoUserStorageClaim cleanup;
  std::vector<RepoUserStorageClaim> retainedClaims;
  std::vector<RepoUserStorageWrite> reservedWrites;
  std::vector<RepoUserStorageWrite> cancelledWrites;
  bool hasQuarantinedContent = false;
};

struct RepoUserStorageInventoryEntry
{
  RepoUserStorageObject object;
  bool legacyMetadata = false;
};

struct RepoUserStorageReconciliation
{
  bool ready = false;
  uint64_t quarantinedObjectCount = 0;
  uint64_t pendingWriteCount = 0;
  uint64_t pendingCleanupCount = 0;
};

struct ArtifactBackendMigrationDiagnostics
{
  uint64_t runtimeSchemaGeneration = 16;
  uint64_t databaseSchemaGeneration = 0;
  uint64_t previousSchemaGeneration = 0;
  uint64_t maxWriteSchemaGeneration = 16;
  bool writesEnabled = false;
  bool destructiveChanges = false;
  std::string action;
  std::string reason;
};

struct PersistentArtifactCommitResult
{
  RepoArtifactCommitReceipt receipt;
  ArtifactLocationRecord currentLocation;
  bool acceptedSeedOperation = false;
};

struct CommittedPayloadMetadata
{
  bool exists = false;
  bool regularFile = false;
  uint64_t sizeBytes = 0;
};

struct ArtifactPublishOperationRecord
{
  std::string operationId;
  std::string submittingPrincipal;
  std::string idempotencyKeyDigest;
  std::string requestDigest;
  PublishOperationIntent intent;
  std::string state;
  uint64_t createdAtMs = 0;
  uint64_t updatedAtMs = 0;
  uint64_t acceptedAtMs = 0;
  uint64_t terminalAtMs = 0;
  PublishOperationStatus status;
  std::string aggregateReceiptJson;
};

/**
 * Streaming content-addressed payload storage for artifact-manifest-v2.
 *
 * Incoming bytes are written to generation-scoped sparse staging files.
 * Verified progress is stored as a compact, merged range map. Finalization
 * verifies the complete content digest, persists a finalization intent, and
 * atomically renames the payload into its digest-derived committed path.
 */
class FilesystemArtifactPayloadStore final : public PayloadStore
{
public:
  explicit FilesystemArtifactPayloadStore(
    std::string rootPath, uint64_t maxRangeBytes = 16 * 1024 * 1024,
    bool createDirectories = true);

  void begin(const ArtifactReference& artifact, uint64_t generation) override;
  void writeRange(const ArtifactReference& artifact, uint64_t generation,
                  ArtifactByteRange range,
                  const std::vector<uint8_t>& bytes) override;
  std::vector<uint8_t> readRange(const ArtifactReference& artifact,
                                 uint64_t generation,
                                 ArtifactByteRange range) const override;
  void markVerified(const ArtifactReference& artifact, uint64_t generation,
                    ArtifactByteRange range) override;
  std::vector<ArtifactByteRange>
  verifiedRanges(const ArtifactReference& artifact, uint64_t generation) const override;
  void flush(const ArtifactReference& artifact, uint64_t generation) override;
  void finalize(const ArtifactReference& artifact, uint64_t generation) override;
  bool isCommitted(const ArtifactReference& artifact,
                   uint64_t generation) const override;
  CommittedPayloadMetadata
  inspectCommitted(const ArtifactReference& artifact) const;
  void abort(const ArtifactReference& artifact, uint64_t generation) override;

  const std::string& rootPath() const noexcept;
  std::string committedPath(const ArtifactReference& artifact) const;
  std::string stagingPath(const ArtifactReference& artifact,
                          uint64_t generation) const;

private:
  std::string m_rootPath;
  uint64_t m_maxRangeBytes;
  bool m_createDirectories;
  mutable std::mutex m_mutex;
};

/** SQLite metadata journal with one durable row per lifecycle event, never per
 * artifact Data packet. */
class SqliteArtifactMetadataStore final : public MetadataStore
{
public:
  // Set immutableReadOnlyProfile only when the caller serializes ownership of
  // this database for the store lifetime; SQLite then skips WAL shared-memory
  // locks while inspecting or reading a compatible future schema.
  explicit SqliteArtifactMetadataStore(
    std::string databasePath, bool artifactWritesEnabled = true,
    uint64_t maxWriteSchemaGeneration = 16,
    bool immutableReadOnlyProfile = false);
  ~SqliteArtifactMetadataStore() override;

  SqliteArtifactMetadataStore(const SqliteArtifactMetadataStore&) = delete;
  SqliteArtifactMetadataStore&
  operator=(const SqliteArtifactMetadataStore&) = delete;

  uint64_t schemaGeneration() const override;
  bool artifactWritesEnabled() const noexcept;
  ArtifactBackendMigrationDiagnostics migrationDiagnostics() const;
  void appendLifecycleEvent(const ArtifactLifecycleEvent& event) override;
  std::vector<ArtifactLifecycleEvent>
  lifecycleEvents(const std::string& operationId) const override;
  ArtifactLifecycleState
  currentLifecycleState(const std::string& operationId) const override;

  ArtifactLocalCommitRecord
  recordLocalCommit(const ArtifactReference& reference,
                    uint64_t committedAtMs);
  std::optional<ArtifactLocalCommitRecord>
  lookupLocalCommit(const ArtifactReference& reference) const;
  std::optional<ArtifactLocalCommitRecord>
  lookupLocalCommit(uint64_t localCommitId) const;

  std::optional<std::string>
  lookupAggregateReceipt(const std::string& receiptId) const;

  void recordPersistentCommit(const ArtifactLocationRecord& location);
  PersistentArtifactCommitResult
  commitPersistentArtifact(const RepoArtifactCommitReceipt& receipt,
                           uint64_t expiresAtMs,
                           const std::string& requesterIdentity = {});
  std::optional<ArtifactLocationRecord>
  lookupPersistentLocation(const ArtifactReference& reference,
                           const std::string& repoNode,
                           const std::string& providerIdentity,
                           uint64_t nowMs) const;
  bool hasPersistentCommitReceipt(const ArtifactReference& reference,
                                  const std::string& repoNode,
                                  const std::string& providerIdentity,
                                  const std::string& operationId,
                                  const std::string& receiptId,
                                  uint64_t generation) const;
  bool hasPersistentCommitReceiptForObject(
    const std::string& objectName, const std::string& repoNode,
    const std::string& providerIdentity, const std::string& sha256,
    uint64_t sizeBytes, const std::string& policyEpoch,
    uint64_t generation) const;

  std::string reservePublishOperation(const std::string& operationId,
                                      const std::string& submittingPrincipal,
                                      const PublishOperationIntent& intent,
                                      uint64_t nowMs);
  std::optional<ArtifactPublishOperationRecord>
  lookupPublishOperation(const std::string& operationId,
                         const std::string& requesterIdentity,
                         const std::optional<RepoArtifactTarget>& authorizedTarget =
                           std::nullopt) const;
  std::vector<ArtifactPublishOperationRecord>
  recoverablePublishOperations() const;
  void updatePublishOperationTarget(const std::string& operationId,
                                    const PublishOperationTargetStatus& targetStatus,
                                    uint64_t updatedAtMs);
  void finalizePublishOperation(const std::string& operationId,
                                const PublishOperationStatus& status,
                                const std::string& aggregateReceiptJson,
                                uint64_t terminalAtMs);

  /** Register immutable storage authority for one exact object.
   * @param authenticatedOwnerIdentity Identity supplied by trusted Core wiring,
   *        never by the request payload. The writer may be an authorized delegate.
   * @return Durable grant; no bytes have been admitted or stored.
   * Calls serialize on the metadata mutex. Invalid bindings and read-only
   * profiles throw; caller-owned identity strings are copied before return.
   */
  RepoUserStorageGrant grantUserStorage(
    const std::string& authenticatedOwnerIdentity, const std::string& repoNode,
    const std::string& grantId, const std::string& writerIdentity,
    const std::string& objectName, uint64_t expiresAtMs, uint64_t nowMs);

  /** Point-in-time check of an existing exact grant, without reserving bytes.
   * Trusted adapters supply the authenticated writer and measured object name.
   * Actual write admission must recheck authorization and quota before mutation.
   */
  RepoUserStorageGrant validateUserStorageGrant(
    const std::string& authenticatedWriterIdentity, const std::string& repoNode,
    const std::string& ownerIdentity, const std::string& grantId,
    const std::string& objectName, uint64_t ownerEpoch, uint64_t nowMs) const;

  /** Trusted orchestration lookup; no admission or physical cleanup is implied. */
  std::optional<RepoUserStorageWrite> lookupUserStorageWrite(
    const std::string& repoNode, const std::string& ownerIdentity,
    const std::string& operationId) const;

  /** Atomically admit measured bytes using an authenticated writer and grant.
   * @param object Immutable payload identity/measurement and initially measured
   *        manifest bytes from the trusted storage adapter. Final manifest size
   *        may be settled before commit. This method does not inspect files.
   * @return Durable reservation or exact terminal retry; QUOTA_EXCEEDED throws.
   * Admission requires successful inventory reconciliation on this connection.
   */
  RepoUserStorageWrite reserveUserStorage(
    const std::string& authenticatedWriterIdentity, const std::string& repoNode,
    const std::string& ownerIdentity, const std::string& grantId,
    const std::string& operationId, const RepoUserStorageObject& object,
    uint64_t nowMs);

  /** Adjust only a Reserved operation to independently measured final manifest
   * bytes, before publication. Trusted recovery may settle before node Ready;
   * this neither creates an operation nor grants writer authorization.
   * Payload identity stays immutable; quota/error leaves the old row unchanged.
   */
  RepoUserStorageWrite settleUserStorageReservation(
    const std::string& repoNode, const std::string& ownerIdentity,
    const std::string& operationId, uint64_t finalManifestBytes, uint64_t nowMs);

   /** Commit only while the exact grant remains live, after trusted orchestration
    * verifies durable publication. This is not an unauthenticated network entry
    * point. The transition is transactionally serialized and idempotent while
    * the grant remains live; conflicting transitions throw without releasing
    * another claim.
   */
  RepoUserStorageWrite commitUserStorage(
    const std::string& repoNode, const std::string& ownerIdentity,
    const std::string& operationId, uint64_t nowMs);

   /** Finish startup accounting for an operation whose payload was already
    * durably committed and verified before a process interruption. This trusted
    * recovery path may settle an expired grant, but still enforces its ownership
    * fence and operation identity. Never use it for incoming writes.
    */
   RepoUserStorageWrite commitRecoveredUserStorage(
     const std::string& repoNode, const std::string& ownerIdentity,
     const std::string& operationId, uint64_t nowMs);
  RepoUserStorageWrite cancelUserStorage(
    const std::string& repoNode, const std::string& ownerIdentity,
    const std::string& operationId, uint64_t nowMs);

  /** Query only the authenticated owner's account/objects at this node.
   * @param afterObjectName Exclusive canonical-name cursor; empty starts a page.
   * @param limit Page size, 1..1000. Deleted claims are excluded.
   * @return A consistent metadata snapshot; neither method grants file access.
   */
  RepoUserStorageUsage userStorageUsage(
    const std::string& authenticatedOwnerIdentity,
    const std::string& repoNode) const;
  std::vector<RepoUserStorageClaim> listUserStorage(
    const std::string& authenticatedOwnerIdentity, const std::string& repoNode,
    const std::string& afterObjectName, uint32_t limit = 100) const;

  /** Observe only the latest live committed claim at the exact owner epoch.
   * This metadata snapshot grants neither a lease nor physical read access.
   */
  std::optional<RepoUserStorageClaim> lookupCommittedUserStorageClaim(
    const std::string& authenticatedOwnerIdentity, const std::string& repoNode,
    const std::string& objectName, uint64_t ownerEpoch) const;

  /** Fence an authenticated owner's exact epoch before physical cleanup.
   * Charge remains until trusted orchestration confirms removal below.
   * An uncommitted reservation transfers its charge into CleanupPrepared;
   * this does not prove publication or abort physical staging.
   * @return Durable cleanup claim, including the result of an exact retry.
   */
  RepoUserStorageClaim prepareUserStorageCleanup(
    const std::string& authenticatedOwnerIdentity, const std::string& repoNode,
    const std::string& cleanupId, const std::string& objectName,
    uint64_t ownerEpoch, uint64_t nowMs);
  /** Confirm physical owner/index removal and staging abort before quota release.
   * This trusted local operation retains the tombstone and is idempotent.
   */
  RepoUserStorageClaim confirmUserStorageCleanup(
    const std::string& repoNode, const std::string& ownerIdentity,
    const std::string& cleanupId, uint64_t nowMs);

  /** Snapshot matching live claims, reservations, exact cancelled source writes
   * and quarantine for prepared cleanup.
   * Trusted caller excludes physical writers across this lookup and cleanup;
   * this read neither fences future writes nor releases quota.
   */
  RepoUserStorageCleanupPeers userStorageCleanupPeers(
    const std::string& repoNode, const std::string& ownerIdentity,
    const std::string& cleanupId) const;

  /** Trusted consistent holder snapshot before reusing a physical object.
   * Includes prepared claims until actual owner cleanup releases their charge.
   * No network endpoint or storage authority is granted by this lookup. */
  std::vector<RepoUserStorageClaim> liveUserStorageClaimsForObject(
    const std::string& repoNode, const std::string& objectName) const;

  /** Startup-only trusted inventory/recovery APIs. They do not authenticate
   * caller-supplied inventory or delete payload files. The orchestrator must
   * exclude physical writers and settle ambiguous publication before admission.
   */
  std::vector<RepoUserStorageWrite> pendingUserStorageWrites(
    const std::string& repoNode) const;
  std::vector<RepoUserStorageClaim> pendingUserStorageCleanups(
    const std::string& repoNode) const;
  RepoUserStorageReconciliation reconcileUserStorageInventory(
    const std::string& repoNode,
    const std::vector<RepoUserStorageInventoryEntry>& inventory, uint64_t nowMs);
  RepoUserStorageReconciliation userStorageReconciliation(
    const std::string& repoNode) const;

  const std::string& databasePath() const noexcept;

private:
  RepoUserStorageWrite commitUserStorageImpl(
    const std::string& repoNode, const std::string& ownerIdentity,
    const std::string& operationId, uint64_t nowMs, bool isRecovery);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

std::unique_ptr<RepositoryStoreFacade>
makeFilesystemArtifactRepositoryStore(const std::string& rootPath,
                                      const std::string& ownerId);

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_FILESYSTEM_ARTIFACT_STORE_HPP
