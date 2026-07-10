#ifndef NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

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
  uint64_t generation = 0;
  int64_t parentGeneration = -1;
  std::string writeConsistency = "ALL";
  uint32_t requiredWriteAcks = 0;
  std::vector<std::string> confirmedReplicaNodes;
  std::string operationId;
  std::string lifecycleState = "COMMITTED";

  std::string toJson() const;
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
  std::string objectName;
  uint64_t generation = 0;
  std::string digest;
  uint64_t persistedBytes = 0;
  std::string state = "COMMITTED";
  uint64_t completedAtMs = 0;

  std::string toJson() const;
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

  virtual RepoCacheStatus cacheStatus() const;
};

class InMemoryRepoStore : public RepoStoreBackend
{
public:
  void put(const RepoObjectManifest& manifest, std::vector<uint8_t> payload) override;

  void putManifest(const RepoObjectManifest& manifest) override;

  StoredObject get(const std::string& objectName) const override;

  bool has(const std::string& objectName) const override;

  bool erase(const std::string& objectName) override;

  size_t size() const override;

  std::vector<RepoObjectManifest> listManifests() const override;

  uint64_t usedBytes() const override;

  RepoCacheStatus cacheStatus() const override;

private:
  std::map<std::string, StoredObject> m_objects;
};

std::shared_ptr<RepoStoreBackend>
makeMemoryRepoStore();

std::shared_ptr<RepoStoreBackend>
makeSqliteRepoStore(const std::string& databasePath);

std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(const std::string& databasePath, uint64_t memoryCacheBytes);

std::shared_ptr<RepoStoreBackend>
makeTieredRepoStore(std::shared_ptr<RepoStoreBackend> authoritativeStore,
                    uint64_t memoryCacheBytes,
                    std::string authoritativeBackend = "custom");

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP
