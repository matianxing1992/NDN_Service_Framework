#ifndef NDNSF_DISTRIBUTED_REPO_LEGACY_REPO_IMPORTER_HPP
#define NDNSF_DISTRIBUTED_REPO_LEGACY_REPO_IMPORTER_HPP

#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

struct LegacyImportClaim
{
  std::string table;
  std::string sourceKey;
  std::string rowSha256;
};

struct LegacyImportItem
{
  std::string sourceKind;
  std::string sourceKey;
  std::string sourceManifestSha256;
  std::string sourcePayloadSha256;
  std::string nativeKey;
  uint64_t sizeBytes = 0;
  uint64_t sourceGeneration = 0;
  uint64_t nativeGeneration = 0;
  std::vector<std::string> packetNames;
  std::vector<std::string> packetSha256;
  std::string state = "PREPARING";
  std::vector<LegacyImportClaim> legacyClaims;
};

struct LegacyImportManifest
{
  uint32_t formatVersion = 1;
  std::string importId;
  std::string sourceSchemaId;
  std::string sourceSchemaFingerprint;
  std::string sourceSnapshotDigest;
  std::string sourcePayloadInventoryDigest;
  std::string destinationId;
  std::string destinationInventoryDigest;
  std::string state = "PREPARING";
  uint64_t itemCount = 0;
  uint64_t byteCount = 0;
  std::string failureCode;
  std::vector<LegacyImportItem> items;
  std::vector<LegacyImportClaim> legacyClaims;
  std::string startedAt;
  std::string completedAt;

  std::string toJson() const;
};

struct LegacyRepoImportConfig
{
  std::filesystem::path sourceDatabase;
  // Root containing payloads/sha256/<prefix>/<digest>.
  std::filesystem::path sourcePayloadRoot;
  std::filesystem::path destinationRoot;
  std::string importId;
  // The importer enforces hard ceilings of 16 MiB per native range and 64 MiB
  // per signed Data wire; callers may lower these limits.
  uint64_t maxRangeBytes = 4 * 1024 * 1024;
  uint64_t maxDataWireBytes = 16 * 1024 * 1024;
  // Called after an item's durable staging checkpoint. Throwing leaves an
  // INCOMPLETE manifest that the same import can verify and resume.
  std::function<void(size_t, const LegacyImportItem&)> onItemCommitted;
};

class LegacyRepoImportError : public std::runtime_error
{
public:
  explicit LegacyRepoImportError(const std::string& what)
    : std::runtime_error(what)
  {
  }
};

class LegacyRepoImporter
{
public:
  static LegacyImportManifest
  import(const LegacyRepoImportConfig& config);
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_LEGACY_REPO_IMPORTER_HPP
