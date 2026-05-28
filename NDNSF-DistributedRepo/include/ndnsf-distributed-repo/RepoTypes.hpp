#ifndef NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP
#define NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ndnsf_distributed_repo {

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

std::string
sha256Hex(const std::vector<uint8_t>& payload);

std::vector<StorageCapability>
selectReplicas(const std::vector<StorageCapability>& candidates,
               const PlacementPolicy& policy,
               uint64_t objectSize);

class InMemoryRepoStore
{
public:
  void put(const RepoObjectManifest& manifest, std::vector<uint8_t> payload);

  const StoredObject& get(const std::string& objectName) const;

  bool has(const std::string& objectName) const;

  bool erase(const std::string& objectName);

  size_t size() const;

  std::vector<RepoObjectManifest> listManifests() const;

private:
  std::map<std::string, StoredObject> m_objects;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_REPO_TYPES_HPP
