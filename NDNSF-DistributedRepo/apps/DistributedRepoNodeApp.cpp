#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/LocalServiceRegistry.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/logger.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

NDN_LOG_INIT(ndnsf_distributed_repo.DistributedRepoNodeApp);

using ndnsf_distributed_repo::RepoClient;
using ndnsf_distributed_repo::RepoDeploymentMode;
using ndnsf_distributed_repo::RepoNode;
using ndnsf_distributed_repo::StorageCapability;

class FileLock
{
public:
  explicit FileLock(const std::string& path)
  {
    m_fd = open(path.c_str(), O_CREAT | O_RDWR, 0666);
    if (m_fd < 0 || flock(m_fd, LOCK_EX) != 0) {
      throw std::runtime_error("failed to lock " + path);
    }
  }

  ~FileLock()
  {
    if (m_fd >= 0) {
      flock(m_fd, LOCK_UN);
      close(m_fd);
    }
  }

private:
  int m_fd = -1;
};

std::string
trim(std::string text)
{
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::map<std::string, std::string>
loadKeyValueConfig(const std::string& path)
{
  std::map<std::string, std::string> values;
  if (path.empty()) {
    return values;
  }

  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open repo config: " + path);
  }

  std::string line;
  while (std::getline(input, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.resize(comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    std::string key;
    std::string value;
    const auto equal = line.find('=');
    if (equal != std::string::npos) {
      key = trim(line.substr(0, equal));
      value = trim(line.substr(equal + 1));
    }
    else {
      std::istringstream is(line);
      is >> key;
      std::getline(is, value);
      value = trim(value);
    }
    if (!key.empty() && !value.empty()) {
      values[key] = value;
    }
  }
  return values;
}

std::string
getOption(int argc, char** argv, const std::string& option, const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return argv[i + 1];
    }
  }
  return fallback;
}

bool
hasFlag(int argc, char** argv, const std::string& option)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == option) {
      return true;
    }
  }
  return false;
}

std::string
configValue(const std::map<std::string, std::string>& config,
            const std::string& key,
            const std::string& fallback)
{
  const auto it = config.find(key);
  return it == config.end() ? fallback : it->second;
}

std::string
optionOrConfig(int argc, char** argv,
               const std::map<std::string, std::string>& config,
               const std::string& option,
               const std::string& key,
               const std::string& fallback)
{
  return getOption(argc, argv, option, configValue(config, key, fallback));
}

bool
parseBool(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return !(value == "false" || value == "0" || value == "no" || value == "off");
}

std::vector<std::string>
splitCsv(const std::string& text)
{
  std::vector<std::string> values;
  std::istringstream input(text);
  std::string item;
  while (std::getline(input, item, ',')) {
    item = trim(item);
    if (!item.empty()) {
      values.push_back(item);
    }
  }
  return values;
}

ndn::security::Certificate
getOrCreateIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib()
      .getIdentity(identity)
      .getDefaultKey()
      .getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey()
      .getDefaultCertificate();
  }
}

struct RepoAppConfig
{
  ndn::Name servicePrefix = ndn::Name(RepoClient::DEFAULT_SERVICE_NAME);
  ndn::Name groupPrefix = ndn::Name("/example/repo/group");
  ndn::Name controllerPrefix = ndn::Name("/example/repo/controller");
  ndn::Name identity = ndn::Name("/example/repo/repo/A");
  std::string trustSchema = "examples/trust-schema.conf";
  StorageCapability capability{
    "/example/repo/repo/A",
    4'000'000'000ULL,
    0,
    0.0,
    1.0,
    "default",
    {"object"}
  };
  RepoDeploymentMode deploymentMode = RepoDeploymentMode::Remote;
  bool serveCertificates = true;
  std::string storageBackend = "tiered";
  std::string storagePath = "/tmp/ndnsf-distributed-repo/repo-node-A.sqlite3";
  uint64_t memoryCacheBytes = 64 * 1024 * 1024;
};

RepoAppConfig
loadRepoAppConfig(int argc, char** argv)
{
  const auto configPath = getOption(
    argc, argv, "--config", "NDNSF-DistributedRepo/configs/repo-node.conf");
  const auto config = loadKeyValueConfig(configPath);

  RepoAppConfig appConfig;
  appConfig.servicePrefix = ndn::Name(optionOrConfig(
    argc, argv, config, "--service-prefix", "service-prefix",
    appConfig.servicePrefix.toUri()));
  appConfig.groupPrefix = ndn::Name(optionOrConfig(
    argc, argv, config, "--group-prefix", "group-prefix",
    appConfig.groupPrefix.toUri()));
  appConfig.controllerPrefix = ndn::Name(optionOrConfig(
    argc, argv, config, "--controller-prefix", "controller-prefix",
    appConfig.controllerPrefix.toUri()));
  appConfig.identity = ndn::Name(optionOrConfig(
    argc, argv, config, "--identity", "identity", appConfig.identity.toUri()));
  appConfig.trustSchema = optionOrConfig(
    argc, argv, config, "--trust-schema", "trust-schema", appConfig.trustSchema);

  appConfig.capability.repoNode = optionOrConfig(
    argc, argv, config, "--repo-node", "repo-node", appConfig.identity.toUri());
  appConfig.capability.freeBytes = std::stoull(optionOrConfig(
    argc, argv, config, "--free-bytes", "free-bytes",
    std::to_string(appConfig.capability.freeBytes)));
  appConfig.capability.usedBytes = std::stoull(optionOrConfig(
    argc, argv, config, "--used-bytes", "used-bytes",
    std::to_string(appConfig.capability.usedBytes)));
  appConfig.capability.recentLoad = std::stod(optionOrConfig(
    argc, argv, config, "--recent-load", "recent-load",
    std::to_string(appConfig.capability.recentLoad)));
  appConfig.capability.availabilityScore = std::stod(optionOrConfig(
    argc, argv, config, "--availability-score", "availability-score",
    std::to_string(appConfig.capability.availabilityScore)));
  appConfig.capability.failureDomain = optionOrConfig(
    argc, argv, config, "--failure-domain", "failure-domain",
    appConfig.capability.failureDomain);
  appConfig.capability.storageClasses = splitCsv(optionOrConfig(
    argc, argv, config, "--storage-classes", "storage-classes", "object"));
  if (appConfig.capability.storageClasses.empty()) {
    appConfig.capability.storageClasses.push_back("object");
  }

  appConfig.deploymentMode = ndnsf_distributed_repo::parseRepoDeploymentMode(
    optionOrConfig(argc, argv, config, "--deployment-mode", "deployment-mode",
                   ndnsf_distributed_repo::toString(appConfig.deploymentMode)));
  const bool deploymentIsInApp = appConfig.deploymentMode == RepoDeploymentMode::Embedded;
  appConfig.capability.repoMode = optionOrConfig(
    argc, argv, config, "--repo-mode", "repo-mode",
    deploymentIsInApp ? "in-app" : "persistent");
  appConfig.capability.acceptsBackupReplica = parseBool(optionOrConfig(
    argc, argv, config, "--accepts-backup-replica", "accepts-backup-replica",
    deploymentIsInApp ? "false" : "true"));
  appConfig.serveCertificates = parseBool(optionOrConfig(
    argc, argv, config, "--serve-certificates", "serve-certificates",
    appConfig.serveCertificates ? "true" : "false"));
  appConfig.storageBackend = optionOrConfig(
    argc, argv, config, "--storage-backend", "storage-backend",
    appConfig.storageBackend);
  appConfig.storagePath = optionOrConfig(
    argc, argv, config, "--storage-path", "storage-path",
    appConfig.storagePath);
  appConfig.memoryCacheBytes = std::stoull(optionOrConfig(
    argc, argv, config, "--memory-cache-bytes", "memory-cache-bytes",
    std::to_string(appConfig.memoryCacheBytes)));
  return appConfig;
}

std::shared_ptr<ndnsf_distributed_repo::RepoStoreBackend>
makeConfiguredStore(const RepoAppConfig& config)
{
  std::string backend = config.storageBackend;
  std::transform(backend.begin(), backend.end(), backend.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (backend == "memory" || backend == "in-memory") {
    throw std::invalid_argument(
      "memory-only repo storage is unsupported; use tiered SQLite storage "
      "with memory-cache-bytes=0 to disable the hot cache");
  }
  if (backend == "sqlite" || backend == "sqlite3") {
    if (config.storagePath.empty()) {
      throw std::invalid_argument("sqlite repo storage requires storage-path");
    }
    const auto parent = std::filesystem::path(config.storagePath).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    return ndnsf_distributed_repo::makeSqliteRepoStore(config.storagePath);
  }
  if (backend.empty() || backend == "tiered" || backend == "sqlite-memory" ||
      backend == "sqlite+lru") {
    if (config.storagePath.empty()) {
      throw std::invalid_argument("tiered repo storage requires storage-path");
    }
    const auto parent = std::filesystem::path(config.storagePath).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    return ndnsf_distributed_repo::makeTieredRepoStore(
      config.storagePath, config.memoryCacheBytes);
  }
  throw std::invalid_argument("unknown repo storage backend: " + config.storageBackend);
}

void
runLocalSmoke(const RepoAppConfig& config, RepoNode& node)
{
  ndn_service_framework::LocalServiceRegistry registry;
  node.registerDeploymentServices(nullptr, &registry, RepoDeploymentMode::Embedded);

  const std::vector<uint8_t> payload = {'r', 'e', 'p', 'o', '-', 's', 'm', 'o', 'k', 'e'};
  ndnsf_distributed_repo::StoreOptions options;
  options.objectType = "smoke";
  options.replicationFactor = 1;
  options.replicaNodes = {config.capability.repoNode};
  auto manifest = RepoClient::localPut(
    registry, config.servicePrefix,
    config.identity.toUri() + "/NDNSF-DISTRIBUTED-REPO/OBJECT/smoke",
    payload, options);
  auto fetched = RepoClient::localGet(registry, config.servicePrefix, manifest.objectName);
  auto inventory = RepoClient::localList(registry, config.servicePrefix);
  auto cacheStatus = RepoClient::localCacheStatus(registry, config.servicePrefix);
  const bool removed = RepoClient::localRemove(registry, config.servicePrefix, manifest.objectName);

  if (fetched != payload || inventory.empty() || !removed ||
      cacheStatus.authoritativeBackend != "sqlite") {
    throw std::runtime_error("local repo smoke failed");
  }
  std::cout << "DISTRIBUTED_REPO_NODE_APP_LOCAL_SMOKE_OK "
            << manifest.toJson() << " cache=" << cacheStatus.toJson() << std::endl;
}

void
printConfigSummary(const RepoAppConfig& config)
{
  std::cout << "DISTRIBUTED_REPO_NODE_CONFIG"
            << " identity=" << config.identity
            << " service_prefix=" << config.servicePrefix
            << " group_prefix=" << config.groupPrefix
            << " controller=" << config.controllerPrefix
            << " repo_node=" << config.capability.repoNode
            << " repo_mode=" << config.capability.repoMode
            << " accepts_backup_replica="
            << (config.capability.acceptsBackupReplica ? "true" : "false")
            << " free_bytes=" << config.capability.freeBytes
            << " deployment_mode=" << ndnsf_distributed_repo::toString(config.deploymentMode)
            << " storage_backend=" << config.storageBackend
            << " storage_path=" << config.storagePath
            << " memory_cache_bytes=" << config.memoryCacheBytes
            << " trust_schema=" << config.trustSchema
            << " serve_certificates=" << (config.serveCertificates ? "true" : "false")
            << std::endl;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    const bool dryRun = hasFlag(argc, argv, "--dry-run");
    const bool localSmoke = hasFlag(argc, argv, "--local-smoke");
    auto config = loadRepoAppConfig(argc, argv);
    RepoNode node(config.servicePrefix, config.capability, makeConfiguredStore(config));
    printConfigSummary(config);

    if (localSmoke) {
      runLocalSmoke(config, node);
    }
    if (dryRun) {
      return 0;
    }

    ndn::Face face;
    FileLock keyChainLock("/tmp/ndnsf-distributed-repo-keychain-" +
                          std::to_string(getuid()) + ".lock");
    ndn::KeyChain keyChain;
    auto repoCert = getOrCreateIdentity(keyChain, config.identity);
    auto controllerCert = getOrCreateIdentity(keyChain, config.controllerPrefix);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(config.identity));

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (config.serveCertificates) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, repoCert.getName());
    }

    ndn_service_framework::ServiceProvider provider(
      face, config.groupPrefix, repoCert, controllerCert, config.trustSchema);
    ndn_service_framework::LocalServiceRegistry localRegistry;
    node.registerDeploymentServices(&provider, &localRegistry, config.deploymentMode);
    provider.init();
    provider.fetchPermissionsFromController(config.controllerPrefix);

    NDN_LOG_INFO("DistributedRepoNodeApp ready identity=" << config.identity
                 << " servicePrefix=" << config.servicePrefix
                 << " mode=" << ndnsf_distributed_repo::toString(config.deploymentMode));
    std::cout << "DISTRIBUTED_REPO_NODE_APP_READY identity=" << config.identity
              << " service_prefix=" << config.servicePrefix
              << " mode=" << ndnsf_distributed_repo::toString(config.deploymentMode)
              << std::endl;
    face.processEvents();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "DistributedRepoNodeApp error: " << e.what() << std::endl;
    return 1;
  }
}
