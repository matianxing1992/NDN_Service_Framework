#ifndef NDNSF_DISTRIBUTED_REPO_FILESYSTEM_REPO_STORE_BACKEND_HPP
#define NDNSF_DISTRIBUTED_REPO_FILESYSTEM_REPO_STORE_BACKEND_HPP

#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ndnsf_distributed_repo {

/** Startup policy is selected by the owning deployment, before any recovery runs. */
enum class FilesystemRecoveryPolicy { RECLAIM_ORPHANS, KEEP_UNCONFIRMED };

/**
 * RepoStoreBackend authority backed by the streaming artifact payload store.
 *
 * Manifest rows are small, atomically replaced sidecars.  Payload bytes are
 * staged and verified through FilesystemArtifactPayloadStore and become
 * visible only after the digest-checked rename and manifest publication.
 * Vector put/get remains available for objects at or below the compatibility
 * threshold; large objects must use putRange/commitRanges/getRange.
 */
class FilesystemRepoStoreBackend final : public RepoStoreBackend
{
public:
  explicit FilesystemRepoStoreBackend(
    std::string rootPath,
    uint64_t maxRangeBytes = 16 * 1024 * 1024,
    uint64_t vectorCompatibilityThreshold = 1 * 1024 * 1024,
    std::string ownerId = "filesystem-repo");
  FilesystemRepoStoreBackend(std::string rootPath, uint64_t maxRangeBytes,
    uint64_t vectorCompatibilityThreshold, std::string ownerId,
    FilesystemRecoveryPolicy recoveryPolicy);
  bool supportsUserStorageRecovery() const noexcept override;

  void put(const RepoObjectManifest& manifest,
           std::vector<uint8_t> payload) override;
  void putManifest(const RepoObjectManifest& manifest) override;
  StoredObject get(const std::string& objectName) const override;
  bool has(const std::string& objectName) const override;
  bool erase(const std::string& objectName) override;
  size_t size() const override;
  std::vector<RepoObjectManifest> listManifests() const override;
  /** Internal locator integration: copy matching metadata under the existing lock. */
  std::vector<RepoObjectManifest>
  findPacketManifests(const std::string& canonical, const std::string& alternate,
                      bool prefixMatch) const;
  uint64_t usedBytes() const override;
  bool hasCommittedPayload(const RepoObjectManifest& manifest) const override;

  void putRange(const RepoObjectManifest& manifest,
                RepoByteRange range,
                const std::vector<uint8_t>& bytes) override;
  void commitRanges(const RepoObjectManifest& manifest) override;
  std::vector<uint8_t> getRange(const std::string& objectName,
                                RepoByteRange range) const override;
  RepoObjectManifest getManifest(const std::string& objectName) const override;
  /** Internal read integration: pin current parsed metadata without copying its index. */
  std::shared_ptr<const RepoObjectManifest>
  getManifestSnapshot(const std::string& objectName) const;
  bool supportsConcurrentCommittedReads() const noexcept override { return true; }
  RepoObjectMetadataProbe
  probeObjectMetadata(const std::string& objectName) const override;
  /** Pin current parsed metadata while measuring the authoritative physical files. */
  RepoObjectMetadataSnapshot
  probeObjectMetadataSnapshot(const std::string& objectName) const;
  std::optional<RepoObjectManifest>
  findRetainedDataReceipt(const std::string& dataName,
                          const std::string& wireSha256) const override;
  void attachImportedDataReceipt(const RepoObjectManifest& expected,
                                 const RepoObjectManifest& updated) override;
  bool supportsRange() const noexcept override;
  uint64_t fullCopyFallbackCount() const noexcept override;
  void abortRanges(const std::string& objectName) override;
  bool abortUserStorageRangesIfOwned(const RepoObjectManifest& expected) override;
  bool abortFailedRangesIfOwned(const RepoObjectManifest& expected) override;
  bool canRemoveUserStorageObject(const RepoUserStorageObject& expected) const override;
  bool confirmUserStorageRemoval(const RepoUserStorageObject& expected) override;
  bool supportsManifestLookup() const noexcept override;

  /** Remove uncommitted staging files and committed payloads with no manifest. */
  uint64_t recoverOrphans();

  const std::string& rootPath() const noexcept;
  uint64_t vectorCompatibilityThreshold() const noexcept;

private:
  RepoRangeReadFunction
  bindCurrentRangeReader(std::shared_ptr<const RepoObjectManifest> expected) const override;

  struct Reservation
  {
    std::string canonicalIdentity;
    std::string digest;
    uint64_t size = 0;
    uint64_t generation = 0;
  };

  struct CachedManifest
  {
    std::string encoded;
    std::shared_ptr<const RepoObjectManifest> parsed;
    std::array<uint64_t, 7> identity{};
  };

  ArtifactReference artifactReference(const RepoObjectManifest& manifest) const;
  std::filesystem::path manifestPath(const std::string& objectName) const;
  std::filesystem::path receiptDirectory(const std::string& dataName,
                                         const std::string& wireSha256) const;
  RepoObjectManifest readManifest(const std::string& objectName) const;
  std::shared_ptr<const RepoObjectManifest>
  readManifestUnlocked(const std::string& objectName) const;
  std::shared_ptr<const RepoObjectManifest>
  loadManifestUnlocked(const std::filesystem::path& path) const;
  /** Pin an immutable parse while the complete current filesystem identity matches. */
  std::shared_ptr<const RepoObjectManifest>
  cachedManifestUnlocked(const std::filesystem::path& path) const;
  std::shared_ptr<const RepoObjectManifest>
  parseManifestCachedUnlocked(const std::string& key, std::string encoded,
                              const std::filesystem::path* source = nullptr) const;
  void writeManifest(const RepoObjectManifest& manifest) const;
  bool isDigestReferencedUnlocked(const std::string& digest,
                                  const std::string& exceptObjectName) const;
  static bool isMetadataOnly(const RepoObjectManifest& manifest) noexcept;
  /** Reserve an already generation-normalized manifest with its exact canonical identity. */
  void reserveDiskUnlocked(const RepoObjectManifest& manifest, const std::string& identity);
  void releaseReservationUnlocked(const std::string& objectName);
  bool canRemoveUserStorageObjectUnlocked(const RepoUserStorageObject& expected) const;
  bool abortUserStorageRangesIfOwnedUnlocked(const RepoObjectManifest& expected);
  bool abortFailedRangesIfOwnedUnlocked(const RepoObjectManifest& expected) noexcept;
  bool hasVerifiedCommittedDigestReferenceUnlocked(const RepoObjectManifest& expected) const;
  RepoObjectManifest readCheckedManifestUnlocked(const std::filesystem::path& path) const;

private:
  std::string m_rootPath;
  uint64_t m_maxRangeBytes;
  uint64_t m_vectorCompatibilityThreshold;
  FilesystemRecoveryPolicy m_recoveryPolicy;
  BackendOwnershipLease m_ownership;
  mutable FilesystemArtifactPayloadStore m_payloadStore;
  mutable std::mutex m_mutex;
  mutable std::unordered_map<std::string, Reservation> m_reservations;
  mutable uint64_t m_reservedBytes = 0;
  mutable std::atomic<uint64_t> m_fullCopyFallbacks{0};
  // Reuse only while the complete current filesystem identity remains unchanged.
  mutable std::unordered_map<std::string, CachedManifest> m_catalogParseCache;
  mutable std::mutex m_catalogParseMutex;
  mutable size_t m_catalogEncodedBytes = 0;
};

std::shared_ptr<RepoStoreBackend>
makeFilesystemRepoStore(const std::string& rootPath,
                        uint64_t maxRangeBytes = 16 * 1024 * 1024,
                        uint64_t vectorCompatibilityThreshold = 1 * 1024 * 1024,
                        std::string ownerId = "filesystem-repo");

std::shared_ptr<RepoStoreBackend>
makeFilesystemRepoStore(const std::string& rootPath, uint64_t maxRangeBytes,
                        uint64_t vectorCompatibilityThreshold, std::string ownerId,
                        FilesystemRecoveryPolicy recoveryPolicy);

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_FILESYSTEM_REPO_STORE_BACKEND_HPP
