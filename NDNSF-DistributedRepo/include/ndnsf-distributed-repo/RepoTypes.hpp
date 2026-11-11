#ifndef NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP

#include "ndnsf-distributed-repo/ArtifactTypes.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

inline constexpr uint64_t REPO_USER_STORAGE_LIMIT_BYTES = 100000000000ULL;
/** Per-object admission/read bound; independent of the Owner's total quota. */
inline constexpr uint64_t REPO_USER_STORAGE_OBJECT_MAX_BYTES = 1000000000ULL;

struct RepoUserStorageObject
{
  std::string objectName;
  std::string wireSha256;
  uint64_t generation = 0;
  uint64_t payloadWireBytes = 0;
  uint64_t manifestBytes = 0;
};

/** Physical APIs accept either normalized Name spelling; Owner journal/wire require ALTERNATE. */
void validateRepoUserStorageObject(const RepoUserStorageObject& object);

enum class RepoUserStorageClaimState { COMMITTED, CLEANUP_PREPARED, DELETED };

struct RepoUserStorageClaim
{
  std::string repoNode;
  std::string ownerIdentity;
  std::string grantId;
  uint64_t ownerEpoch = 0;
  RepoUserStorageObject object;
  RepoUserStorageClaimState state = RepoUserStorageClaimState::COMMITTED;
  std::string cleanupId;
  uint64_t committedAtMs = 0;
  uint64_t deletedAtMs = 0;
};

struct RepoUserStorageGrant
{
  std::string grantId;
  std::string repoNode;
  std::string ownerIdentity;
  std::string writerIdentity;
  std::string objectName;
  uint64_t ownerEpoch = 0;
  uint64_t expiresAtMs = 0;
  uint64_t createdAtMs = 0;
};

struct RepoUserStorageUsage
{
  std::string repoNode;
  std::string ownerIdentity;
  uint64_t limitBytes = REPO_USER_STORAGE_LIMIT_BYTES;
  uint64_t committedBytes = 0;
  uint64_t reservedBytes = 0;
};

struct RepoUserStorageGrantRequest;

struct RepoUserStorageGrantReply
{
  uint32_t schemaVersion = 1;
  RepoUserStorageGrant grant;
  std::string toJson() const;
  static RepoUserStorageGrantReply fromJson(const std::string& wire);
  void validateExpected(const RepoArtifactTarget& target, const std::string& owner,
                        const RepoUserStorageGrantRequest& request) const;
};

struct RepoUserStorageUsageReply
{
  uint32_t schemaVersion = 1;
  RepoUserStorageUsage usage;
  std::string toJson() const;
  static RepoUserStorageUsageReply fromJson(const std::string& wire);
  void validateExpected(const RepoArtifactTarget& target, const std::string& owner) const;
};

struct RepoUserStorageGrantRequest
{
  uint32_t schemaVersion = 1;
  std::string grantId;
  std::string writerIdentity;
  std::string objectName;
  uint64_t expiresAtMs = 0;
  std::string toJson() const;
  static RepoUserStorageGrantRequest fromJson(const std::string& wire);
};

struct RepoUserStorageUsageRequest
{
  uint32_t schemaVersion = 1;
  std::string toJson() const;
  static RepoUserStorageUsageRequest fromJson(const std::string& wire);
};

struct RepoUserStorageListRequest
{
  uint32_t schemaVersion = 1;
  std::string afterObjectName;
  uint32_t limit = 100;
  std::string toJson() const;
  static RepoUserStorageListRequest fromJson(const std::string& wire);
};

struct RepoUserStorageDeleteRequest
{
  uint32_t schemaVersion = 1;
  std::string cleanupId;
  std::string objectName;
  uint64_t ownerEpoch = 0;
  std::string toJson() const;
  static RepoUserStorageDeleteRequest fromJson(const std::string& wire);
};

struct RepoUserStorageDeleteReply
{
  uint32_t schemaVersion = 1;
  std::string repoNode;
  std::string ownerIdentity;
  std::string cleanupId;
  std::string objectName;
  uint64_t ownerEpoch = 0;
  std::string state;
  uint64_t releasedBytes = 0;
  std::string toJson() const;
  static RepoUserStorageDeleteReply fromJson(const std::string& wire);
  void validateExpected(const RepoArtifactTarget& target, const std::string& owner,
                        const RepoUserStorageDeleteRequest& request) const;
};

struct RepoUserStorageListReply
{
  uint32_t schemaVersion = 1;
  std::string repoNode;
  std::string ownerIdentity;
  std::vector<RepoUserStorageClaim> objects;
  std::string nextAfterObjectName;
  std::string toJson() const;
  static RepoUserStorageListReply fromJson(const std::string& wire);
  void validateExpected(const RepoArtifactTarget& target, const std::string& owner,
                        const RepoUserStorageListRequest& request) const;
};
namespace reason {
inline constexpr const char* OperationConflict = "repo-operation-conflict";
inline constexpr const char* GenerationConflict = "repo-generation-conflict";
inline constexpr const char* WriteIncomplete = "repo-write-incomplete";
inline constexpr const char* Overloaded = "repo-overloaded";
inline constexpr const char* CapacityReserved = "repo-capacity-reserved";
inline constexpr const char* IntegrityFailure = "repo-integrity-failure";
inline constexpr const char* RepairUnavailable = "repo-repair-unavailable";
} // namespace reason

enum class RepoDeploymentMode
{
  Remote,
  Embedded,
  Both,
};

enum class RepoWriteConsistency
{
  One,
  Quorum,
  All,
};

std::string
toString(RepoWriteConsistency consistency);

RepoWriteConsistency
parseRepoWriteConsistency(const std::string& value);

uint32_t
requiredWriteAcks(uint32_t replicationFactor, RepoWriteConsistency consistency);

std::string
normalizeRepoOperationState(const std::string& value);

struct RepoObjectManifest
{
  std::string objectName;
  std::string objectType = "artifact";
  std::string sha256;
  uint64_t size = 0;
  uint32_t segmentCount = 1;
  uint32_t replicationFactor = 1;
  std::vector<std::string> replicaNodes;
  // Ordered original Data names for packet-backed objects. Packet wire bytes
  // are stored under these exact names, never under Repo-generated aliases.
  std::vector<std::string> packetNames;
  std::string policyEpoch;
  // Protected durable-publication identity. These are opaque, non-secret
  // values owned by Core; Repo persists and returns them without interpreting
  // keys or authorization.
  std::string publicationIdentity;
  std::string protectionEpoch;
  std::string keyReferenceId;
  std::string keyReferenceVersion;
  std::string ciphertextManifestDigest;
  std::string servingLocator;
  std::string contentDigest;
  uint64_t plaintextSize = 0;
  uint64_t generation = 0;
  int64_t parentGeneration = -1;
  std::string writeConsistency = "ALL";
  uint32_t requiredWriteAcks = 0;
  std::vector<std::string> confirmedReplicaNodes;
  std::string operationId;
  std::string lifecycleState = "COMMITTED";
  // Original Data commits only. Persisted with the Data manifest, so a
  // query never manufactures a receipt from unbound content metadata.
  std::string dataReceiptId;
  std::string dataReceiptRepoNode;
  std::string dataReceiptProviderIdentity;
  uint64_t dataReceiptCommittedAtMs = 0;

  // Private physical packet-set index; original Data wire remains unchanged.
  std::vector<uint64_t> packetOffsets;
  uint64_t envelopeBytes = 0;
  std::string dataPrefix;

  /** Compare all persisted metadata with the same default handling as toJson(). */
  bool hasSameMetadata(const RepoObjectManifest& other) const;

  std::string toJson() const;
};

/** Bounded metadata-only view of one manifest and its committed payload. */
struct RepoObjectMetadataProbe
{
  bool manifestPresent = false;
  RepoObjectManifest manifest;
  bool payloadMetadataAvailable = false;
  bool payloadPresent = false;
  bool payloadRepresentationValid = false;
  uint64_t payloadSize = 0;
  bool manifestMeasurementAvailable = false;
  uint64_t manifestWireBytes = 0;
};

/** Current physical measurements with pinned immutable metadata; no admission decision. */
struct RepoObjectMetadataSnapshot
{
  std::shared_ptr<const RepoObjectManifest> manifest;
  bool payloadMetadataAvailable = false;
  bool payloadPresent = false;
  bool payloadRepresentationValid = false;
  uint64_t payloadSize = 0;
  bool manifestMeasurementAvailable = false;
  uint64_t manifestWireBytes = 0;
};

struct RepoWriteIntent
{
  std::string operationId;
  std::string objectName;
  uint64_t generation = 0;
  int64_t expectedGeneration = -1;
  std::string digest;
  uint32_t replicationFactor = 1;
  uint32_t requiredAcks = 1;
  std::string consistency = "ALL";
  std::vector<std::string> selectedReplicas;
  std::string state = "RECEIVED";
  uint64_t createdAtMs = 0;
  uint64_t updatedAtMs = 0;

  std::string toJson() const;
};

struct RepoWriteReceipt
{
  std::string operationId;
  std::string repoNode;
  std::string providerIdentity;
  std::string objectName;
  uint64_t generation = 0;
  std::string digest;
  uint64_t persistedBytes = 0;
  std::string state = "COMMITTED";
  uint64_t completedAtMs = 0;
  std::optional<ArtifactReference> artifactReference;

  std::string toJson() const;
  static RepoWriteReceipt dataPacketFromJson(const std::string& wire);
};

struct RepoDataPacketQuery
{
  uint32_t schemaVersion = 1;
  std::string dataName;
  std::string expectedWireSha256;

  std::string toJson() const;
  static RepoDataPacketQuery fromJson(const std::string& wire);
};

struct RepoDataPacketLocation
{
  uint32_t schemaVersion = 1;
  std::string receiptId;
  std::string repoNode;
  std::string providerIdentity;
  std::string dataName;
  std::string wireSha256;
  std::string state = "UNKNOWN";
  std::string forwardingHint;
  uint64_t observedAtMs = 0;
  uint64_t expiresAtMs = 0;

  std::string toJson() const;
  static RepoDataPacketLocation fromJson(const std::string& wire);
};

struct ArtifactStoreRequest
{
  uint32_t schemaVersion = 2;
  std::string operationId;
  uint64_t generation = 0;
  ArtifactReference reference;
  // Set only when an authenticated seed Provider stores a selected replica.
  // The local seed's foreground write instead requires its submitter journal.
  std::optional<RepoArtifactTarget> operationOwner;
  std::vector<uint8_t> bytes;

  std::vector<uint8_t> toWire(const ArtifactLimits& limits = {}) const;
  static ArtifactStoreRequest
  fromWire(const std::vector<uint8_t>& wire, const ArtifactLimits& limits = {});
};

struct ArtifactRangeWriteRequest
{
  uint32_t schemaVersion = 2;
  std::string operationId;
  uint64_t generation = 0;
  ArtifactReference reference;
  std::optional<RepoArtifactTarget> operationOwner;
  uint64_t offsetBytes = 0;
  uint64_t lengthBytes = 0;
  std::vector<uint8_t> bytes;

  std::vector<uint8_t> toWire(const ArtifactLimits& limits = {}) const;
  static ArtifactRangeWriteRequest
  fromWire(const std::vector<uint8_t>& wire, const ArtifactLimits& limits = {});
};

struct ArtifactRangeCommitRequest
{
  uint32_t schemaVersion = 2;
  std::string operationId;
  uint64_t generation = 0;
  ArtifactReference reference;
  std::optional<RepoArtifactTarget> operationOwner;

  std::vector<uint8_t> toWire(const ArtifactLimits& limits = {}) const;
  static ArtifactRangeCommitRequest
  fromWire(const std::vector<uint8_t>& wire, const ArtifactLimits& limits = {});
};

struct ArtifactRangeAbortRequest
{
  uint32_t schemaVersion = 1;
  std::string operationId;
  uint64_t generation = 0;
  ArtifactReference reference;

  std::vector<uint8_t> toWire(const ArtifactLimits& limits = {}) const;
  static ArtifactRangeAbortRequest
  fromWire(const std::vector<uint8_t>& wire, const ArtifactLimits& limits = {});
};

struct RepoCapacityReservation
{
  std::string reservationId;
  std::string operationId;
  uint64_t reservedBytes = 0;
  std::string state = "ACTIVE";
  uint64_t expiresAtMs = 0;

  std::string toJson() const;
};

struct RepoDataReference
{
  std::string objectName;
  std::string dataPrefix;
  uint64_t firstSegment = 0;
  uint64_t finalSegment = 0;
  bool hasFinalSegment = false;
  std::string forwardingHint;
  std::string expectedSha256;
  uint64_t expectedSize = 0;
  bool storeWirePackets = true;
  std::string objectType = "ndn-segmented-data";

  // Original packet names and publication metadata; never a storage authorization.
  std::optional<RepoObjectManifest> packetSetMetadata;

  void validateExactPacketSet() const;

  std::string toJson() const;
};

struct RepoOperationStatus
{
  std::string operationId;
  std::string operation;
  std::string state = "RECEIVED";
  std::string objectName;
  std::string message;
  uint64_t completedSegments = 0;
  uint64_t totalSegments = 0;
  uint64_t createdAtMs = 0;
  uint64_t updatedAtMs = 0;
  uint64_t expiresAtMs = 0;

  std::string toJson() const;
};

/**
 * Canonical, per-operation evidence for Spec 164 performance gates.
 *
 * Byte counters are deliberately non-overlapping concepts: logical payload is
 * reconstructed application content, wire bytes include NDN packet encoding,
 * retransmitted bytes are the repeated subset of wire traffic, and storage
 * bytes describe payload-store I/O.  committedReplicaCount counts distinct
 * retained valid receipts, never attempted writes.
 */
struct RepoOperationMetrics
{
  static constexpr size_t MAX_OPERATION_ID_BYTES = 256;

  std::string operationId;
  uint64_t startedAtMs = 0;
  uint64_t completedAtMs = 0;
  std::map<std::string, double> phaseTimingsMs;

  uint64_t logicalPayloadBytes = 0;
  uint64_t dataWireBytes = 0;
  uint64_t interestWireBytes = 0;
  uint64_t wireBytes = 0;
  uint64_t retransmittedBytes = 0;
  uint64_t payloadStoreBytesRead = 0;
  uint64_t payloadStoreBytesWritten = 0;
  uint64_t metadataStoreBytesRead = 0;
  uint64_t metadataStoreBytesWritten = 0;
  uint64_t storageBytesRead = 0;
  uint64_t storageBytesWritten = 0;

  uint64_t asymmetricVerifications = 0;
  uint64_t digestVerifications = 0;
  double asymmetricVerificationMs = 0.0;
  double digestVerificationMs = 0.0;

  uint64_t controlOperations = 0;
  uint64_t metadataOperations = 0;
  uint64_t metadataRecordCount = 0;

  uint32_t requestedReplicaCount = 0;
  uint32_t selectedReplicaCount = 0;
  uint32_t committedReplicaCount = 0;
  uint32_t rejectedReplicaReceiptCount = 0;

  static bool isCanonicalPhase(const std::string& phase);
  void validate() const;
  std::string toJson() const;
};

struct StorageCapability
{
  std::string repoNode;
  uint64_t freeBytes = 0;
  uint64_t usedBytes = 0;
  double recentLoad = 0.0;
  double availabilityScore = 1.0;
  std::string failureDomain;
  std::vector<std::string> storageClasses;
  std::string repoMode = "persistent";
  bool acceptsBackupReplica = true;

  std::string toJson() const;
};

struct RepoCatalogEntry
{
  RepoObjectManifest manifest;
  std::string sourceRepo;
  std::string repoMode = "persistent";
  std::string state = "AVAILABLE";
  uint64_t catalogEpoch = 0;

  std::string toJson() const;
};

struct RepoCatalogStatus
{
  std::string repoNode;
  std::string repoMode = "persistent";
  uint64_t catalogEpoch = 0;
  uint64_t objectCount = 0;
  bool acceptsBackupReplica = true;
  // True when the process-local delta journal may not cover durable history,
  // including objects deleted before restart. Reconcile from catalogSnapshot().
  bool reconciliationRequired = false;

  std::string toJson() const;
};

struct RepoCatalogDelta
{
  std::string repoNode;
  std::string repoMode = "persistent";
  uint64_t sinceEpoch = 0;
  uint64_t catalogEpoch = 0;
  std::vector<RepoCatalogEntry> entries;

  std::string toJson() const;
};

struct RepoCacheStatus
{
  std::string storageBackend = "unknown";
  std::string authoritativeBackend = "unknown";
  std::string cachePolicy = "disabled";
  uint64_t budgetBytes = 0;
  uint64_t usedBytes = 0;
  uint64_t entryCount = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t admissions = 0;
  uint64_t evictions = 0;
  uint64_t invalidations = 0;
  uint64_t oversizedBypasses = 0;
  uint64_t backingReads = 0;
  uint64_t backingWrites = 0;
  // The authority is durable but cache epoch bookkeeping needs a later
  // reconciliation (for example after an allocation failure).
  bool reconciliationRequired = false;

  std::string toJson() const;
};

struct PlacementPolicy
{
  uint32_t replicationFactor = 1;
  bool avoidSameFailureDomain = true;
  bool preferLowLoad = true;
  bool preferHighAvailability = true;
};

struct StoredObject
{
  RepoObjectManifest manifest;
  std::vector<uint8_t> payload;
};

/**
 * Bounded byte range used by large-object repository backends.  A backend
 * may reject a range larger than its configured I/O window; callers must not
 * assume that a range read materializes the complete object.
 */
struct RepoByteRange
{
  uint64_t offsetBytes = 0;
  uint64_t lengthBytes = 0;
};

using RepoRangeReadFunction = std::function<std::vector<uint8_t>(RepoByteRange)>;

RepoDeploymentMode
parseRepoDeploymentMode(const std::string& value);

std::string
toString(RepoDeploymentMode mode);

bool
enablesRemote(RepoDeploymentMode mode);

bool
enablesEmbedded(RepoDeploymentMode mode);

bool
isInAppRepo(const StorageCapability& capability);

bool
isPersistentRepo(const StorageCapability& capability);

std::string
sha256Hex(const std::vector<uint8_t>& payload);

std::vector<StorageCapability>
selectReplicas(const std::vector<StorageCapability>& candidates,
               const PlacementPolicy& policy,
               uint64_t objectSize);

class RepoStoreBackend
{
public:
  virtual ~RepoStoreBackend() = default;

  // Store opaque APP bytes. Repo backends validate storage shape/capacity only;
  // APP trust, signature, and hash verification happen after fetch.
  virtual void put(const RepoObjectManifest& manifest, std::vector<uint8_t> payload) = 0;

  // Store metadata for a logical parent object whose payload is held in child
  // objects such as <object>/seg/<index>.
  virtual void putManifest(const RepoObjectManifest& manifest) = 0;

  virtual StoredObject get(const std::string& objectName) const = 0;

  virtual bool has(const std::string& objectName) const = 0;

  virtual bool erase(const std::string& objectName) = 0;

  virtual size_t size() const = 0;

  virtual std::vector<RepoObjectManifest> listManifests() const = 0;

  virtual uint64_t usedBytes() const = 0;

  // Conservatively reports whether a commit may reuse payload bytes already
  // present for this content. Content-addressed stores also detect orphaned
  // committed payloads that are not referenced by a manifest.
  virtual bool hasCommittedPayload(const RepoObjectManifest& manifest) const;

  virtual RepoCacheStatus cacheStatus() const;

  /** Committed reads and metadata probes do not require Core's write fence. */
  virtual bool supportsConcurrentCommittedReads() const noexcept { return false; }

  // Large-object path.  Legacy vector backends keep the compatibility
  // defaults below; concrete file-backed authorities override these methods
  // so Core/Node/Client can transfer bounded ranges without full assembly.
  virtual void putRange(const RepoObjectManifest& manifest,
                        RepoByteRange range,
                        const std::vector<uint8_t>& bytes);

  virtual void commitRanges(const RepoObjectManifest& manifest);

  virtual std::vector<uint8_t>
  getRange(const std::string& objectName, RepoByteRange range) const;

  virtual RepoObjectManifest
  getManifest(const std::string& objectName) const;

  /** Must not materialize or read payload bytes. Unsupported stores return an
   * unknown probe so callers do not mistake has()/cache state for durability. */
  virtual RepoObjectMetadataProbe
  probeObjectMetadata(const std::string& objectName) const;

  /** Latest prior Data commit for an exact name and wire digest, retained
   * after deletion. Durable backends override this; volatile stores may not.
   */
  virtual std::optional<RepoObjectManifest>
  findRetainedDataReceipt(const std::string& dataName,
                          const std::string& wireSha256) const;

  /** Attach an explicit import receipt to an unchanged legacy Data manifest.
   * The backend must compare the current manifest with expected and publish
   * updated atomically without rewriting the original payload. */
  virtual void attachImportedDataReceipt(const RepoObjectManifest& expected,
                                         const RepoObjectManifest& updated);

  virtual bool supportsRange() const noexcept;

  // Number of times a caller requested the legacy full-vector path for an
  // object that is configured for bounded range transfer.
  virtual uint64_t fullCopyFallbackCount() const noexcept;

  // Cache lease hooks.  The default is a no-op for non-caching authorities;
  // tiered stores use the count to keep active entries out of eviction.
  virtual void pin(const std::string& objectName) const;
  virtual void unpin(const std::string& objectName) const;

  virtual void abortRanges(const std::string& objectName);

  /** Trusted caller holds Owner/writer fences; success confirms exact staging absent. */
  virtual bool abortUserStorageRangesIfOwned(const RepoObjectManifest& expected);
  /** Failed transaction cleanup; refuse committed or uncertain finalized residue. */
  virtual bool abortFailedRangesIfOwned(const RepoObjectManifest& expected);

  /** Physical preflight only; caller excludes writers and retains Owner authorization. */
  virtual bool canRemoveUserStorageObject(const RepoUserStorageObject& expected) const;
  /** Confirm durable erase after trusted Owner cleanup; unsupported stores refuse. */
  virtual bool confirmUserStorageRemoval(const RepoUserStorageObject& expected);

  // True only when getManifest can be served without materializing payload.
  virtual bool supportsManifestLookup() const noexcept;
  /** True only when startup preserves unconfirmed Owner bytes for fenced recovery. */
  virtual bool supportsUserStorageRecovery() const noexcept;

protected:
  friend class RepoCore;
  /** RepoCore retains this backend and serializes readers with its writers. */
  virtual RepoRangeReadFunction
  bindCurrentRangeReader(std::shared_ptr<const RepoObjectManifest> expected) const;
};

std::shared_ptr<RepoStoreBackend>
makeSqliteRepoStore(const std::string& databasePath);

std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(const std::string& databasePath, uint64_t memoryCacheBytes,
                    uint64_t largeObjectThreshold = 1 * 1024 * 1024);

std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(std::shared_ptr<RepoStoreBackend> authoritativeStore,
                    uint64_t memoryCacheBytes,
                    std::string authoritativeBackend = "custom",
                    uint64_t largeObjectThreshold = 1 * 1024 * 1024);

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP
