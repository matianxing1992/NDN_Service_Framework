#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace ndnsf_distributed_repo::tests {

namespace {

class TempRoot
{
public:
  TempRoot()
  {
    static std::atomic<uint64_t> sequence{0};
    m_path = std::filesystem::temp_directory_path() /
      ("spec192-persistent-service-" + std::to_string(getpid()) + "-" +
       std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(m_path);
  }

  ~TempRoot()
  {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  const std::filesystem::path&
  path() const
  {
    return m_path;
  }

private:
  std::filesystem::path m_path;
};

ndn::security::Certificate
makeRsaIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  auto key = keyChain.createIdentity(identity, ndn::RsaKeyParams(2048));
  return key.getDefaultKey().getDefaultCertificate();
}

std::vector<std::string>
repoOperations()
{
  return {
    "STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES", "INSERT",
    "STORE_MANIFEST", "FETCH", "FETCH_RANGE", "MANIFEST", "INVENTORY",
    "CAPABILITY", "CACHE_STATUS", "STATUS", "CATALOG_STATUS",
    "CATALOG_SNAPSHOT", "CATALOG_DELTA", "CATALOG_LOOKUP", "DELETE",
  };
}

std::vector<char*>
mutableArgv(std::vector<std::string>& arguments)
{
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (auto& argument : arguments) {
    argv.push_back(argument.data());
  }
  return argv;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedRepoPersistentService)

BOOST_AUTO_TEST_CASE(PersistentModeRejectsNonPersistentAndUnknownValues)
{
  BOOST_CHECK(parseRepoServerMode("persistent") == RepoServerMode::Persistent);
  BOOST_CHECK(parseRepoServerMode("Remote") == RepoServerMode::Persistent);
  for (const auto* mode : {"", "in-app", "embedded", "local", "both", "unknown"}) {
    BOOST_CHECK_THROW(parseRepoServerMode(mode), std::invalid_argument);
  }

  TempRoot temp;
  std::vector<std::string> invalidArguments = {
    "DistributedRepoServer", "--repo-mode", "embedded",
    "--repo-node", "/repo/test",
    "--database", (temp.path() / "runtime-must-not-exist" / "repo.sqlite3").string(),
    "--group-prefix", "/repo/group",
    "--provider-identity", "/repo/provider",
    "--authority-identity", "/repo/authority",
    "--trust-schema", std::filesystem::absolute("examples/trust-any.conf").string(),
  };
  auto argv = mutableArgv(invalidArguments);
  BOOST_CHECK_THROW(PersistentRepoServerConfig::fromArguments(
                      static_cast<int>(argv.size()), argv.data()),
                    std::invalid_argument);
  BOOST_CHECK(!std::filesystem::exists(temp.path() / "runtime-must-not-exist"));

  std::vector<std::string> unknownOptionArguments = {
    "DistributedRepoServer", "--repo-mode", "persistent",
    "--repo-node", "/repo/test",
    "--database", (temp.path() / "unknown-option-must-not-exist" / "repo.sqlite3").string(),
    "--group-prefix", "/repo/group",
    "--provider-identity", "/repo/provider",
    "--authority-identity", "/repo/authority",
    "--trust-schema", std::filesystem::absolute("examples/trust-any.conf").string(),
    "--unexpected", "value",
  };
  argv = mutableArgv(unknownOptionArguments);
  BOOST_CHECK_THROW(PersistentRepoServerConfig::fromArguments(
                      static_cast<int>(argv.size()), argv.data()),
                    std::invalid_argument);
  BOOST_CHECK(!std::filesystem::exists(temp.path() / "unknown-option-must-not-exist"));

  std::vector<std::string> missingOptionArguments = {
    "DistributedRepoServer", "--repo-mode", "persistent",
    "--repo-node", "/repo/test",
    "--database", (temp.path() / "missing-option-must-not-exist" / "repo.sqlite3").string(),
    "--group-prefix", "/repo/group",
    "--provider-identity", "/repo/provider",
    "--authority-identity", "/repo/authority",
  };
  argv = mutableArgv(missingOptionArguments);
  BOOST_CHECK_THROW(PersistentRepoServerConfig::fromArguments(
                      static_cast<int>(argv.size()), argv.data()),
                    std::invalid_argument);
  BOOST_CHECK(!std::filesystem::exists(temp.path() / "missing-option-must-not-exist"));

  const auto malformedSchema = temp.path() / "malformed-trust-schema.conf";
  {
    std::ofstream output(malformedSchema);
    output << "this is not a valid validator configuration\n";
  }
  std::vector<std::string> malformedSchemaArguments = {
    "DistributedRepoServer", "--repo-mode", "persistent",
    "--repo-node", "/repo/test",
    "--database", (temp.path() / "malformed-schema-must-not-exist" / "repo.sqlite3").string(),
    "--group-prefix", "/repo/group",
    "--provider-identity", "/repo/provider",
    "--authority-identity", "/repo/authority",
    "--trust-schema", malformedSchema.string(),
  };
  argv = mutableArgv(malformedSchemaArguments);
  BOOST_CHECK_THROW(PersistentRepoServerConfig::fromArguments(
                      static_cast<int>(argv.size()), argv.data()),
                    std::exception);
  BOOST_CHECK(!std::filesystem::exists(temp.path() / "malformed-schema-must-not-exist"));

  PersistentRepoServerConfig config;
  config.mode = static_cast<RepoServerMode>(255);
  config.repoNode = "/repo/test";
  config.groupPrefix = ndn::Name("/repo/group");
  config.providerIdentity = ndn::Name("/repo/provider");
  config.authorityIdentity = ndn::Name("/repo/authority");
  config.databasePath = temp.path() / "must-not-be-created" / "repo.sqlite3";
  config.trustSchemaPath = std::filesystem::absolute("examples/trust-any.conf");
  BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
  BOOST_CHECK(!std::filesystem::exists(config.databasePath.parent_path()));
}

BOOST_AUTO_TEST_CASE(PersistentConfigRequiresExplicitDurableRuntimeInputs)
{
  TempRoot temp;
  PersistentRepoServerConfig config;
  config.repoNode = "/repo/test";
  config.groupPrefix = ndn::Name("/repo/group");
  config.providerIdentity = ndn::Name("/repo/provider");
  config.authorityIdentity = ndn::Name("/repo/authority");
  config.databasePath = temp.path() / "repo.sqlite3";
  config.trustSchemaPath = std::filesystem::absolute("examples/trust-any.conf");
  config.validate();

  config.databasePath = "relative/repo.sqlite3";
  BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
  config.databasePath = temp.path() / "repo.sqlite3";
  config.trustSchemaPath = "examples/trust-any.conf";
  BOOST_CHECK_THROW(config.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NetworkRegistrationCoversOperationsAndRetainsRepoLifetime)
{
  TempRoot temp;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec192-repo-service",
                                   "tpm-memory:spec192-repo-service");
  const auto providerCert =
    makeRsaIdentity(keyChain, ndn::Name("/spec192/provider"));
  const auto authorityCert =
    makeRsaIdentity(keyChain, ndn::Name("/spec192/authority"));

  std::weak_ptr<RepoNode> retainedRepo;
  {
    ndn_service_framework::ServiceProvider provider(
      ndn_service_framework::ServiceProvider::LocalMockTag{},
      face,
      ndn::Name("/spec192/group"),
      providerCert,
      authorityCert,
      std::filesystem::absolute("examples/trust-any.conf").string());

    StorageCapability capability;
    capability.repoNode = "/repo/test";
    capability.repoMode = "persistent";
    capability.freeBytes = 1024 * 1024;
    auto repo = std::make_shared<RepoNode>(
      ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
      capability,
      makeTieredRepoStore((temp.path() / "repo.sqlite3").string(), 4096));
    retainedRepo = repo;
    repo->registerServices(provider);
    repo.reset();

    const auto capabilityName =
      makeRepoServiceName(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), "CAPABILITY");
    provider.setAckStrategyHandler(
      capabilityName,
      [](const ndn_service_framework::RequestMessage&) {
        ndn_service_framework::ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "replacement-ack";
        return decision;
      });
    BOOST_CHECK(!retainedRepo.expired());
    for (const auto& operation : repoOperations()) {
      const auto serviceName =
        makeRepoServiceName(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), operation);
      ndn_service_framework::RequestMessage request;
      const auto response = provider.dispatchRequest(
        ndn::Name("/spec192/user"),
        provider.getName(),
        serviceName,
        ndn::Name("/spec192/request").append(operation),
        request);
      BOOST_CHECK_MESSAGE(
        response.getErrorInfo() != "No handler registered for " + serviceName.toUri(),
        "production Repo handler was not registered for " << operation);
    }

    const auto capabilityResponse = provider.dispatchRequest(
      ndn::Name("/spec192/user"), provider.getName(),
      capabilityName,
      ndn::Name("/spec192/capability"), ndn_service_framework::RequestMessage{});
    BOOST_CHECK(capabilityResponse.getStatus());
    BOOST_CHECK_EQUAL(
      makeRepoServiceName(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), "STATUS").toUri(),
      "/NDNSF/DistributedRepo/Object/v1/STATUS");
  }

  BOOST_CHECK(retainedRepo.expired());
}

BOOST_AUTO_TEST_CASE(NetworkRegistrationRejectsStackOwnedRepoBeforePublishingHandlers)
{
  TempRoot temp;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec192-repo-stack",
                                   "tpm-memory:spec192-repo-stack");
  const auto providerCert =
    makeRsaIdentity(keyChain, ndn::Name("/spec192/provider-stack"));
  const auto authorityCert =
    makeRsaIdentity(keyChain, ndn::Name("/spec192/authority-stack"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{},
    face,
    ndn::Name("/spec192/group"),
    providerCert,
    authorityCert,
    std::filesystem::absolute("examples/trust-any.conf").string());

  StorageCapability capability;
  capability.repoNode = "/repo/stack";
  capability.repoMode = "persistent";
  RepoNode repo(ndn::Name("/NDNSF/DistributedRepo"), capability,
                makeTieredRepoStore((temp.path() / "stack.sqlite3").string(), 4096));
  BOOST_CHECK_THROW(repo.registerServices(provider), std::logic_error);

  const auto missing = provider.dispatchRequest(
    ndn::Name("/spec192/user"), provider.getName(),
    makeRepoServiceName(ndn::Name("/NDNSF/DistributedRepo"), "CAPABILITY"),
    ndn::Name("/spec192/stack"), ndn_service_framework::RequestMessage{});
  BOOST_CHECK_EQUAL(missing.getErrorInfo(),
                    "No handler registered for /NDNSF/DistributedRepo/CAPABILITY");
}

BOOST_AUTO_TEST_CASE(ArtifactAndCoreLogicalRepoIdsMustMatch)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/core-identity";
  capability.freeBytes = 4096;
  BOOST_CHECK_EXCEPTION(
    RepoNode(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), capability,
             makeTieredRepoStore((temp.path() / "identity.sqlite3").string(), 4096),
             "/repo/artifact-identity", "/provider/identity"),
    std::invalid_argument,
    [] (const std::invalid_argument& error) {
      return std::string(error.what()).find(
        "repo-artifact-identity-mismatch:") == 0;
    });
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::tests
