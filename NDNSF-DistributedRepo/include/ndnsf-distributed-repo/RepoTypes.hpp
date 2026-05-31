#ifndef NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

enum class RepoDeploymentMode
{
  Remote,
  Embedded,
  Both,
};

struct RepoObjectManifest
{
  std::string objectName;
  std::string objectType = "artifact";
  std::string sha256;
  uint64_t size = 0;
  uint32_t segmentCount = 1;
  uint32_t replicationFactor = 1;
  std::vector<std::string> replicaNodes;
  std::string policyEpoch;

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

private:
  std::map<std::string, StoredObject> m_objects;
};

std::shared_ptr<RepoStoreBackend>
makeMemoryRepoStore();

std::shared_ptr<RepoStoreBackend>
makeSqliteRepoStore(const std::string& databasePath);

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP
