#ifndef NDNSF_DISTRIBUTED_REPO_PERSISTENT_REPO_SERVER_CONFIG_HPP
#define NDNSF_DISTRIBUTED_REPO_PERSISTENT_REPO_SERVER_CONFIG_HPP

#include <ndn-cxx/name.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ndnsf_distributed_repo {

enum class RepoServerMode
{
  Persistent,
};

RepoServerMode
parseRepoServerMode(const std::string& value);

/** Validated startup configuration for the standalone C++ Persistent Repo. */
struct PersistentRepoServerConfig
{
  RepoServerMode mode = RepoServerMode::Persistent;
  std::string repoNode;
  ndn::Name servicePrefix{"/NDNSF/DistributedRepo/Object/v1"};
  ndn::Name groupPrefix;
  ndn::Name providerIdentity;
  ndn::Name authorityIdentity;
  std::filesystem::path databasePath;
  std::filesystem::path trustSchemaPath;
  uint64_t memoryCacheBytes = 64ULL * 1024 * 1024;
  uint64_t largeObjectThresholdBytes = 1024ULL * 1024;
  int ackCollectionTimeMs = 250;
  int requestTimeoutMs = 5000;
  uint64_t availabilityFreshnessMs = 5000;
  std::string failureDomain;

  static PersistentRepoServerConfig
  fromArguments(int argc, char* argv[]);

  void
  validate() const;
};

} // namespace ndnsf_distributed_repo

#endif // NDNSF_DISTRIBUTED_REPO_PERSISTENT_REPO_SERVER_CONFIG_HPP
