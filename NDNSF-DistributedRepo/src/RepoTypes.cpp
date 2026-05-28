#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>

namespace ndnsf_distributed_repo {

namespace {

std::string
jsonQuote(const std::string& value)
{
  std::ostringstream os;
  os << '"';
  for (char ch : value) {
    switch (ch) {
    case '\\':
      os << "\\\\";
      break;
    case '"':
      os << "\\\"";
      break;
    case '\n':
      os << "\\n";
      break;
    case '\r':
      os << "\\r";
      break;
    case '\t':
      os << "\\t";
      break;
    default:
      os << ch;
      break;
    }
  }
  os << '"';
  return os.str();
}

double
scoreCandidate(const StorageCapability& candidate)
{
  double score = 0.0;
  score += static_cast<double>(candidate.freeBytes) / (1024.0 * 1024.0);
  score += 1000.0 * candidate.availabilityScore;
  score -= 1000.0 * candidate.recentLoad;
  return score;
}

} // namespace

std::string
RepoObjectManifest::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"objectName\":" << jsonQuote(objectName) << ",";
  os << "\"objectType\":" << jsonQuote(objectType) << ",";
  os << "\"sha256\":" << jsonQuote(sha256) << ",";
  os << "\"size\":" << size << ",";
  os << "\"segmentCount\":" << segmentCount << ",";
  os << "\"replicationFactor\":" << replicationFactor << ",";
  os << "\"policyEpoch\":" << jsonQuote(policyEpoch) << ",";
  os << "\"replicaNodes\":[";
  for (size_t i = 0; i < replicaNodes.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << jsonQuote(replicaNodes[i]);
  }
  os << "]}";
  return os.str();
}

std::string
StorageCapability::toJson() const
{
  std::ostringstream os;
  os << "{";
  os << "\"repoNode\":" << jsonQuote(repoNode) << ",";
  os << "\"freeBytes\":" << freeBytes << ",";
  os << "\"usedBytes\":" << usedBytes << ",";
  os << "\"recentLoad\":" << recentLoad << ",";
  os << "\"availabilityScore\":" << availabilityScore << ",";
  os << "\"failureDomain\":" << jsonQuote(failureDomain) << ",";
  os << "\"storageClasses\":[";
  for (size_t i = 0; i < storageClasses.size(); ++i) {
    if (i != 0) {
      os << ",";
    }
    os << jsonQuote(storageClasses[i]);
  }
  os << "]}";
  return os.str();
}

std::string
sha256Hex(const std::vector<uint8_t>& payload)
{
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(payload.data(), payload.size(), digest);

  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (uint8_t byte : digest) {
    os << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return os.str();
}

std::vector<StorageCapability>
selectReplicas(const std::vector<StorageCapability>& candidates,
               const PlacementPolicy& policy,
               uint64_t objectSize)
{
  std::vector<StorageCapability> filtered;
  for (const auto& candidate : candidates) {
    if (!candidate.repoNode.empty() && candidate.freeBytes >= objectSize) {
      filtered.push_back(candidate);
    }
  }

  std::sort(filtered.begin(), filtered.end(),
            [] (const StorageCapability& lhs, const StorageCapability& rhs) {
              const double lhsScore = scoreCandidate(lhs);
              const double rhsScore = scoreCandidate(rhs);
              if (lhsScore == rhsScore) {
                return lhs.repoNode < rhs.repoNode;
              }
              return lhsScore > rhsScore;
            });

  std::vector<StorageCapability> selected;
  std::set<std::string> selectedFailureDomains;
  for (const auto& candidate : filtered) {
    if (selected.size() >= policy.replicationFactor) {
      break;
    }
    if (policy.avoidSameFailureDomain && !candidate.failureDomain.empty() &&
        selectedFailureDomains.count(candidate.failureDomain) != 0) {
      continue;
    }
    selected.push_back(candidate);
    if (!candidate.failureDomain.empty()) {
      selectedFailureDomains.insert(candidate.failureDomain);
    }
  }

  if (selected.size() < policy.replicationFactor) {
    for (const auto& candidate : filtered) {
      if (selected.size() >= policy.replicationFactor) {
        break;
      }
      const auto alreadySelected = std::any_of(
        selected.begin(), selected.end(),
        [&] (const StorageCapability& item) {
          return item.repoNode == candidate.repoNode;
        });
      if (!alreadySelected) {
        selected.push_back(candidate);
      }
    }
  }

  return selected;
}

void
InMemoryRepoStore::put(const RepoObjectManifest& manifest,
                       std::vector<uint8_t> payload)
{
  if (manifest.objectName.empty()) {
    throw std::invalid_argument("repo object name must not be empty");
  }
  if (sha256Hex(payload) != manifest.sha256) {
    throw std::invalid_argument("repo object payload does not match manifest sha256");
  }
  m_objects[manifest.objectName] = StoredObject{manifest, std::move(payload)};
}

const StoredObject&
InMemoryRepoStore::get(const std::string& objectName) const
{
  auto it = m_objects.find(objectName);
  if (it == m_objects.end()) {
    throw std::out_of_range("repo object not found: " + objectName);
  }
  return it->second;
}

bool
InMemoryRepoStore::has(const std::string& objectName) const
{
  return m_objects.count(objectName) != 0;
}

bool
InMemoryRepoStore::erase(const std::string& objectName)
{
  return m_objects.erase(objectName) != 0;
}

size_t
InMemoryRepoStore::size() const
{
  return m_objects.size();
}

std::vector<RepoObjectManifest>
InMemoryRepoStore::listManifests() const
{
  std::vector<RepoObjectManifest> manifests;
  manifests.reserve(m_objects.size());
  for (const auto& item : m_objects) {
    manifests.push_back(item.second.manifest);
  }
  return manifests;
}

} // namespace ndnsf_distributed_repo
