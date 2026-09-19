#ifndef NDNSF_DISTRIBUTED_REPO_FILESYSTEM_REPO_STORE_BACKEND_HPP
#define NDNSF_DISTRIBUTED_REPO_FILESYSTEM_REPO_STORE_BACKEND_HPP

#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ndnsf_distributed_repo {

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

  void put(const RepoObjectManifest& manifest,
           std::vector<uint8_t> payload) override;
  void putManifest(const RepoObjectManifest& manifest) override;
  StoredObject get(const std::string& objectName) const override;
  bool has(const std::string& objectName) const override;
  bool erase(const std::string& objectName) override;
  size_t size() const override;
  std::vector<RepoObjectManifest> listManifests() const override;
  uint64_t usedBytes() const override;

  void putRange(const RepoObjectManifest& manifest,
                RepoByteRange range,
                const std::vector<uint8_t>& bytes) override;
  void commitRanges(const RepoObjectManifest& manifest) override;
  std::vector<uint8_t> getRange(const std::string& objectName,
                                RepoByteRange range) const override;
  RepoObjectManifest getManifest(const std::string& objectName) const override;
  bool supportsRange() const noexcept override;
  uint64_t fullCopyFallbackCount() const noexcept override;
  void abortRanges(const std::string& objectName) override;
  bool supportsManifestLookup() const noexcept override;

  /** Remove uncommitted staging files and committed payloads with no manifest. */
  uint64_t recoverOrphans();

  const std::string& rootPath() const noexcept;
  uint64_t vectorCompatibilityThreshold() const noexcept;

private:
  struct Reservation
  {
    std::string canonicalIdentity;
    std::string digest;
    uint64_t size = 0;
    uint64_t generation = 0;
  };

  ArtifactReference artifactReference(const RepoObjectManifest& manifest) const;
  std::filesystem::path manifestPath(const std::string& objectName) const;
  RepoObjectManifest readManifest(const std::string& objectName) const;
  RepoObjectManifest readManifestUnlocked(const std::string& objectName) const;
  void writeManifest(const RepoObjectManifest& manifest) const;
  bool isDigestReferencedUnlocked(const std::string& digest,
                                  const std::string& exceptObjectName) const;
  static bool isMetadataOnly(const RepoObjectManifest& manifest) noexcept;
  void reserveDiskUnlocked(const RepoObjectManifest& manifest);
  void releaseReservationUnlocked(const std::string& objectName);

private:
  std::string m_rootPath;
  uint64_t m_maxRangeBytes;
  uint64_t m_vectorCompatibilityThreshold;
  BackendOwnershipLease m_ownership;
  mutable FilesystemArtifactPayloadStore m_payloadStore;
  mutable std::mutex m_mutex;
  mutable std::unordered_map<std::string, Reservation> m_reservations;
  mutable uint64_t m_reservedBytes = 0;
  mutable std::atomic<uint64_t> m_fullCopyFallbacks{0};
};

std::shared_ptr<RepoStoreBackend>
makeFilesystemRepoStore(const std::string& rootPath,
                        uint64_t maxRangeBytes = 16 * 1024 * 1024,
                        uint64_t vectorCompatibilityThreshold = 1 * 1024 * 1024,
                        std::string ownerId = "filesystem-repo");

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_FILESYSTEM_REPO_STORE_BACKEND_HPP
