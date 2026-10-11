#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"

#include <ndn-cxx/security/certificate-fetcher-offline.hpp>
#include <ndn-cxx/security/validator-config.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>

namespace ndnsf_distributed_repo {

namespace {

uint64_t
parseUnsigned(const std::string& value, const std::string& option)
{
  uint64_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::invalid_argument("invalid unsigned value for " + option);
  }
  return result;
}

int
parsePositiveInt(const std::string& value, const std::string& option)
{
  const auto parsed = parseUnsigned(value, option);
  if (parsed == 0 || parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("invalid positive integer for " + option);
  }
  return static_cast<int>(parsed);
}

std::string
required(const std::map<std::string, std::string>& options,
         const std::string& name)
{
  const auto it = options.find(name);
  if (it == options.end() || it->second.empty()) {
    throw std::invalid_argument("missing required option " + name);
  }
  return it->second;
}

} // namespace

RepoServerMode
parseRepoServerMode(const std::string& value)
{
  std::string normalized = value;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [] (unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  // "remote" is the existing Repo deployment spelling for the same network
  // service path; keep it as an alias while this executable's canonical mode
  // remains Persistent.
  if (normalized == "persistent" || normalized == "remote") {
    return RepoServerMode::Persistent;
  }
  throw std::invalid_argument("Persistent Repo server rejects mode: " + value);
}

PersistentRepoServerConfig
PersistentRepoServerConfig::fromArguments(int argc, char* argv[])
{
  std::map<std::string, std::string> options;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option.size() < 3 || option.compare(0, 2, "--") != 0) {
      throw std::invalid_argument("unexpected argument: " + option);
    }
    if (i + 1 >= argc || std::string(argv[i + 1]).compare(0, 2, "--") == 0) {
      throw std::invalid_argument("missing value for " + option);
    }
    if (!options.emplace(option, argv[++i]).second) {
      throw std::invalid_argument("duplicate option " + option);
    }
  }

  PersistentRepoServerConfig config;
  if (const auto it = options.find("--repo-mode"); it != options.end()) {
    config.mode = parseRepoServerMode(it->second);
  }
  config.repoNode = required(options, "--repo-node");
  config.databasePath = required(options, "--database");
  config.groupPrefix = ndn::Name(required(options, "--group-prefix"));
  config.providerIdentity = ndn::Name(required(options, "--provider-identity"));
  config.authorityIdentity = ndn::Name(required(options, "--authority-identity"));
  config.trustSchemaPath = required(options, "--trust-schema");

  if (const auto it = options.find("--service-prefix"); it != options.end()) {
    config.servicePrefix = ndn::Name(it->second);
    options.erase(it);
  }
  if (const auto it = options.find("--cache-bytes"); it != options.end()) {
    config.memoryCacheBytes = parseUnsigned(it->second, it->first);
    options.erase(it);
  }
  if (const auto it = options.find("--large-object-threshold-bytes");
      it != options.end()) {
    config.largeObjectThresholdBytes = parseUnsigned(it->second, it->first);
    options.erase(it);
  }
  if (const auto it = options.find("--failure-domain"); it != options.end()) {
    config.failureDomain = it->second;
    options.erase(it);
  }
  if (const auto it = options.find("--ack-collection-ms"); it != options.end()) {
    config.ackCollectionTimeMs = parsePositiveInt(it->second, it->first);
    options.erase(it);
  }
  if (const auto it = options.find("--request-timeout-ms"); it != options.end()) {
    config.requestTimeoutMs = parsePositiveInt(it->second, it->first);
    options.erase(it);
  }
  if (const auto it = options.find("--availability-freshness-ms"); it != options.end()) {
    config.availabilityFreshnessMs = parseUnsigned(it->second, it->first);
    options.erase(it);
  }

  for (const auto& [option, value] : options) {
    (void)value;
    if (option != "--repo-mode" && option != "--repo-node" &&
        option != "--database" && option != "--group-prefix" &&
        option != "--provider-identity" && option != "--authority-identity" &&
        option != "--trust-schema" && option != "--ack-collection-ms" &&
        option != "--request-timeout-ms" &&
        option != "--availability-freshness-ms") {
      throw std::invalid_argument("unknown option " + option);
    }
  }

  config.validate();
  return config;
}

void
PersistentRepoServerConfig::validate() const
{
  if (mode != RepoServerMode::Persistent) {
    throw std::invalid_argument("Persistent Repo server requires Persistent mode");
  }
  if (repoNode.empty() || ndn::Name(repoNode).empty()) {
    throw std::invalid_argument("repo-node must be a non-empty NDN name");
  }
  if (servicePrefix.empty() || groupPrefix.empty() || providerIdentity.empty() ||
      authorityIdentity.empty()) {
    throw std::invalid_argument("service, group, Provider, and authority names are required");
  }
  if (databasePath.empty() || !databasePath.is_absolute() ||
      databasePath.filename().empty()) {
    throw std::invalid_argument("database must be an absolute file path");
  }
  if (trustSchemaPath.empty() || !trustSchemaPath.is_absolute()) {
    throw std::invalid_argument("trust-schema must be an absolute file path");
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(trustSchemaPath, error) || error) {
    throw std::invalid_argument("trust-schema must be a regular file");
  }
  // Run the same ndn-cxx ValidatorConfig parser used by ServiceProvider before
  // the server creates its Face, filesystem Repo store, artifact SQLite store,
  // or Provider.
  // The offline fetcher is sufficient because loading parses local policy and
  // trust-anchor declarations but must not perform network certificate fetches.
  ndn::ValidatorConfig validator(
    std::make_unique<ndn::security::CertificateFetcherOffline>());
  validator.load(trustSchemaPath.string());
  if (memoryCacheBytes == 0 || largeObjectThresholdBytes == 0 ||
      availabilityFreshnessMs == 0) {
    throw std::invalid_argument("cache and large-object threshold must be positive");
  }
  if (ackCollectionTimeMs <= 0 || requestTimeoutMs <= ackCollectionTimeMs) {
    throw std::invalid_argument(
      "ACK collection time must be positive and less than the request timeout");
  }
}

} // namespace ndnsf_distributed_repo
