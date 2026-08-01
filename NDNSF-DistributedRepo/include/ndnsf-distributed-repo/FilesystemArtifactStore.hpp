#ifndef NDNSF_DISTRIBUTED_REPO_FILESYSTEM_ARTIFACT_STORE_HPP
#define NDNSF_DISTRIBUTED_REPO_FILESYSTEM_ARTIFACT_STORE_HPP

#include "ndnsf-distributed-repo/RepoStoreBackend.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace ndnsf_distributed_repo {

struct ArtifactBackendMigrationDiagnostics
{
  uint64_t runtimeSchemaGeneration = 12;
  uint64_t databaseSchemaGeneration = 0;
  uint64_t previousSchemaGeneration = 0;
  uint64_t maxWriteSchemaGeneration = 12;
  bool writesEnabled = false;
  bool destructiveChanges = false;
  std::string action;
  std::string reason;
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
    std::string rootPath, uint64_t maxRangeBytes = 16 * 1024 * 1024);

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
  void abort(const ArtifactReference& artifact, uint64_t generation) override;

  const std::string& rootPath() const noexcept;
  std::string committedPath(const ArtifactReference& artifact) const;
  std::string stagingPath(const ArtifactReference& artifact,
                          uint64_t generation) const;

private:
  std::string m_rootPath;
  uint64_t m_maxRangeBytes;
  mutable std::mutex m_mutex;
};

/** SQLite metadata journal with one durable row per lifecycle event, never per
 * artifact Data packet. */
class SqliteArtifactMetadataStore final : public MetadataStore
{
public:
  explicit SqliteArtifactMetadataStore(
    std::string databasePath, bool artifactWritesEnabled = true,
    uint64_t maxWriteSchemaGeneration = 12);
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

  const std::string& databasePath() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

std::unique_ptr<RepositoryStoreFacade>
makeFilesystemArtifactRepositoryStore(const std::string& rootPath,
                                      const std::string& ownerId);

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_FILESYSTEM_ARTIFACT_STORE_HPP
