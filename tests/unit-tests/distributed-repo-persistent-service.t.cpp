#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"
#include "ndnsf-distributed-repo/ArtifactTypes.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackendTestAccess.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"
#include "ndnsf-distributed-repo/detail/StrictJson.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <openssl/sha.h>
#include <sqlite3.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

namespace ndn_service_framework {

struct ServiceProviderTestAccess
{
  static void setLocalMockMode(ServiceProvider& provider, bool enabled)
  {
    provider.m_isLocalMock = enabled;
  }

  static void configureFaceBackedProducerValidator(ServiceProvider& provider,
                                                   const std::string& trustSchemaPath,
                                                   const ndn::Name& groupPrefix,
                                                   ndn::Face& face)
  {
    provider.validator = std::make_shared<MessageValidator>(trustSchemaPath, groupPrefix, &face);
    provider.m_isLocalMock = false;
  }

  static std::function<std::size_t()> observeProducerValidationWaiters(
      ServiceProvider& provider)
  {
    const auto control = provider.m_producerPacketValidationControl;
    return [control] {
      std::lock_guard<std::mutex> lock(control->mutex);
      return control->outstanding;
    };
  }
};

} // namespace ndn_service_framework

namespace ndnsf_distributed_repo::tests {

namespace {

int rejectDirectoryFsync(int fd)
{
  struct stat metadata{};
  if (::fstat(fd, &metadata) == 0 && S_ISDIR(metadata.st_mode)) {
    errno = EIO;
    return -1;
  }
  return ::fsync(fd);
}

struct DirectoryFsyncFailureScope
{
  detail::FilesystemRepoStoreIoHooks previous =
    detail::installFilesystemRepoStoreIoHooks({rejectDirectoryFsync, nullptr, nullptr});
  ~DirectoryFsyncFailureScope() { detail::installFilesystemRepoStoreIoHooks(previous); }
};

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

std::string
sha256(const std::vector<uint8_t>& bytes)
{
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(bytes.data(), bytes.size(), digest);
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(SHA256_DIGEST_LENGTH * 2);
  for (const auto value : digest) {
    result.push_back(HEX[value >> 4]);
    result.push_back(HEX[value & 0x0f]);
  }
  return result;
}

ndnsf_distributed_repo::ArtifactReference
makeReference(const std::string& name, const std::vector<uint8_t>& bytes)
{
  ndnsf_distributed_repo::ArtifactReference reference;
  reference.logicalName = "/spec192/publisher-restart/model/" + name;
  reference.contentDigest = sha256(bytes);
  reference.sizeBytes = bytes.size();
  reference.rootManifestName = "/spec192/publisher-restart/manifest/" + name;
  reference.publisherIdentity = "/spec192/publisher-restart";
  reference.policyEpoch = "repo-policy-v1";
  return reference.canonicalized();
}

ndn_service_framework::ResponseMessage
dispatch(ndn_service_framework::ServiceProvider& provider,
         const ndn::Name& requester, const ndn::Name& service,
         const std::vector<uint8_t>& payload, const std::string& requestId)
{
  ndn_service_framework::RequestMessage request;
  ndn::Buffer buffer;
  if (!payload.empty()) buffer = ndn::Buffer(payload.data(), payload.size());
  request.setPayload(buffer, buffer.size());
  return provider.dispatchRequest(
    requester, provider.getName(), service,
    ndn::Name("/spec192/request").append(requestId), request);
}

std::string
responseString(const ndn_service_framework::ResponseMessage& response)
{
  const auto payload = response.getPayload();
  return std::string(payload.begin(), payload.end());
}

int
queryCount(const std::string& databasePath, const char* table)
{
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(databasePath.c_str(), &database,
                      SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    const std::string error = database == nullptr ? "open failed" : sqlite3_errmsg(database);
    if (database != nullptr) sqlite3_close(database);
    throw std::runtime_error("Persistent service oracle database open failed: " + error);
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> databaseOwner(database, sqlite3_close);
  sqlite3_stmt* statement = nullptr;
  const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
  if (sqlite3_prepare_v2(databaseOwner.get(), sql.c_str(), -1,
                         &statement, nullptr) != SQLITE_OK) {
    throw std::runtime_error("Persistent service count query could not be prepared");
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statementOwner(
    statement, sqlite3_finalize);
  if (sqlite3_step(statementOwner.get()) != SQLITE_ROW) {
    throw std::runtime_error("Persistent service count query returned no row");
  }
  return sqlite3_column_int(statementOwner.get(), 0);
}

std::string
queryOperationValue(const std::string& databasePath, const char* table,
                    const char* column, const std::string& operationId)
{
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(databasePath.c_str(), &database,
                      SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    const std::string error = database == nullptr ? "open failed" : sqlite3_errmsg(database);
    if (database != nullptr) sqlite3_close(database);
    throw std::runtime_error("Persistent service oracle database open failed: " + error);
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> databaseOwner(database, sqlite3_close);
  const std::string sql = std::string("SELECT ") + column + " FROM " + table +
                          " WHERE operation_id=?";
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(databaseOwner.get(), sql.c_str(), -1,
                         &statement, nullptr) != SQLITE_OK) {
    throw std::runtime_error("Persistent service operation query could not be prepared");
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statementOwner(
    statement, sqlite3_finalize);
  if (sqlite3_bind_text(statementOwner.get(), 1, operationId.c_str(), -1,
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_step(statementOwner.get()) != SQLITE_ROW) {
    throw std::runtime_error("Persistent service operation query returned no row");
  }
  const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(statementOwner.get(), 0));
  if (value == nullptr) {
    throw std::runtime_error("Persistent service operation query returned NULL");
  }
  const std::string result(value);
  return result;
}

std::vector<std::string>
repoOperations()
{
  return {
    "STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES", "INSERT",
    "STORE_MANIFEST", "FETCH", "FETCH_RANGE", "MANIFEST", "INVENTORY",
    "CAPABILITY", "CACHE_STATUS", "STATUS", "CATALOG_STATUS",
    "CATALOG_SNAPSHOT", "CATALOG_DELTA", "CATALOG_LOOKUP", "DELETE",
    "USER_STORAGE_GRANT", "USER_STORAGE_USAGE",
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

BOOST_AUTO_TEST_CASE(PersistentOperationWorkerTimeoutsAreParsedAndBounded)
{
  TempRoot temp;
  std::vector<std::string> arguments = {
    "DistributedRepoServer", "--repo-mode", "persistent",
    "--repo-node", "/repo/test",
    "--database", (temp.path() / "repo.sqlite3").string(),
    "--group-prefix", "/repo/group",
    "--provider-identity", "/repo/provider",
    "--authority-identity", "/repo/authority",
    "--trust-schema", std::filesystem::absolute("examples/trust-any.conf").string(),
    "--ack-collection-ms", "400",
    "--request-timeout-ms", "7000",
  };
  auto argv = mutableArgv(arguments);
  const auto parsed = PersistentRepoServerConfig::fromArguments(
    static_cast<int>(argv.size()), argv.data());
  BOOST_CHECK_EQUAL(parsed.ackCollectionTimeMs, 400);
  BOOST_CHECK_EQUAL(parsed.requestTimeoutMs, 7000);

  for (const auto& invalidPair : {
         std::pair<std::string, std::string>{"0", "7000"},
         std::pair<std::string, std::string>{"400", "400"},
         std::pair<std::string, std::string>{"400", "399"}}) {
    auto invalidArguments = arguments;
    invalidArguments[invalidArguments.size() - 3] = invalidPair.first;
    invalidArguments[invalidArguments.size() - 1] = invalidPair.second;
    auto invalidArgv = mutableArgv(invalidArguments);
    BOOST_CHECK_THROW(PersistentRepoServerConfig::fromArguments(
      static_cast<int>(invalidArgv.size()), invalidArgv.data()),
      std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(ActualServerOwnedPreflightCreatesAndReopensSchema16WithoutNetwork)
{
  TempRoot temp;
  const auto executable = std::filesystem::absolute(
    boost::unit_test::framework::master_test_suite().argv[0]).parent_path() /
    "spec193-object-repo-server";
  BOOST_REQUIRE(std::filesystem::is_regular_file(executable));
  const auto database = temp.path() / "owned/repo.sqlite3";
  const auto output = temp.path() / "server.log";
  std::vector<std::string> arguments = {
    executable.string(), "--repo-mode", "persistent", "--repo-node", "/repo/test",
    "--database", database.string(), "--group-prefix", "/repo/group",
    "--provider-identity", "/repo/provider", "--authority-identity", "/repo/authority",
    "--trust-schema", std::filesystem::absolute("examples/trust-any.conf").string(),
    "--local-command", "user-storage-preflight",
  };
  const auto run = [&] (std::vector<std::string> values) {
    auto argv = mutableArgv(values);
    argv.push_back(nullptr);
    const auto child = ::fork();
    BOOST_REQUIRE(child >= 0);
    if (child == 0) {
      const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0)
        ::_exit(126);
      ::close(fd);
      ::execv(executable.c_str(), argv.data());
      ::_exit(127);
    }
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    pid_t result;
    do {
      result = ::waitpid(child, &status, WNOHANG);
      if (result == child) break;
      if (result < 0 && errno != EINTR) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    if (result != child) {
      ::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
      BOOST_FAIL("actual server preflight did not exit within 15 seconds");
    }
    BOOST_REQUIRE(WIFEXITED(status));
    return WEXITSTATUS(status);
  };
  auto legacy = arguments;
  legacy.insert(legacy.end(), {"--legacy-object-protocol", "1"});
  BOOST_CHECK_EQUAL(run(legacy), 2);
  BOOST_CHECK(!std::filesystem::exists(database.parent_path()));
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    BOOST_REQUIRE_EQUAL(run(arguments), 0);
    std::ifstream log(output);
    const std::string report(std::istreambuf_iterator<char>{log}, {});
    BOOST_CHECK(report.find("USER_STORAGE_PREFLIGHT profile=owned schema=16 limit_bytes=1000000000")
                != std::string::npos);
    SqliteArtifactMetadataStore journal(database.string(), false, 16, false);
    BOOST_CHECK_EQUAL(journal.schemaGeneration(), 16);
    const auto usage = journal.userStorageUsage("/repo/provider", "/repo/test");
    BOOST_CHECK_EQUAL(usage.limitBytes, 1000000000ULL);
    BOOST_CHECK_EQUAL(usage.committedBytes, 0);
    BOOST_CHECK_EQUAL(usage.reservedBytes, 0);
  }
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
        response.getErrorInfo() != "No handler registered for " + serviceName.toUri(ndn::name::UriFormat::ALTERNATE),
        "production Repo handler was not registered for " << operation);
    }

    const auto capabilityResponse = provider.dispatchRequest(
      ndn::Name("/spec192/user"), provider.getName(),
      capabilityName,
      ndn::Name("/spec192/capability"), ndn_service_framework::RequestMessage{});
    BOOST_CHECK(capabilityResponse.getStatus());
    BOOST_CHECK_EQUAL(
      makeRepoServiceName(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), "STATUS").toUri(ndn::name::UriFormat::ALTERNATE),
      "/NDNSF/DistributedRepo/Object/v1/STATUS");
  }

  BOOST_CHECK(retainedRepo.expired());
}

BOOST_AUTO_TEST_CASE(TieredSqliteMetadataProbeUsesAuthoritativePayloadLengths)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/sqlite-metadata-probe";
  capability.repoMode = "persistent";
  capability.freeBytes = 1024 * 1024;
  const auto databasePath = (temp.path() / "repo.sqlite3").string();
  RepoNode node(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), capability,
    makeTieredRepoStore(databasePath, 4096),
    capability.repoNode);

  const std::string objectName = "/artifact/spec192/sqlite-metadata-probe";
  const std::vector<uint8_t> payload{0x51, 0x52, 0x53, 0x54, 0x55};
  node.put(objectName, payload, "artifact-v2-bytes", 1, "policy-v1");
  auto probe = node.core().probeObjectMetadata(objectName);
  BOOST_REQUIRE(probe.manifestPresent);
  BOOST_CHECK_EQUAL(probe.manifest.objectName, objectName);
  BOOST_CHECK(probe.payloadMetadataAvailable);
  BOOST_CHECK(probe.payloadPresent);
  BOOST_CHECK(probe.payloadRepresentationValid);
  BOOST_CHECK_EQUAL(probe.payloadSize, payload.size());

  const auto executeObjectUpdate = [&] (const char* sql) {
    sqlite3* rawDatabase = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_open_v2(databasePath.c_str(), &rawDatabase,
      SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(
      rawDatabase, sqlite3_close);
    sqlite3_stmt* rawStatement = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_prepare_v2(database.get(), sql, -1,
      &rawStatement, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      rawStatement, sqlite3_finalize);
    BOOST_REQUIRE_EQUAL(sqlite3_bind_text(statement.get(), 1, objectName.c_str(),
      -1, SQLITE_TRANSIENT), SQLITE_OK);
    BOOST_REQUIRE_EQUAL(sqlite3_step(statement.get()), SQLITE_DONE);
    BOOST_CHECK_EQUAL(sqlite3_changes(database.get()), 1);
  };
  const auto serializedBytes = node.core().getManifest(objectName).toJson().size();
  BOOST_CHECK(probe.manifestMeasurementAvailable);
  BOOST_CHECK_EQUAL(probe.manifestWireBytes, serializedBytes);
  executeObjectUpdate("UPDATE objects SET manifest_json=manifest_json||char(10)||'  ' WHERE object_name=?");
  probe = node.core().probeObjectMetadata(objectName);
  BOOST_CHECK(probe.manifestMeasurementAvailable);
  BOOST_CHECK_EQUAL(probe.manifestWireBytes, serializedBytes + 3);
  BOOST_CHECK_EQUAL(probe.manifest.toJson().size(), serializedBytes);
  executeObjectUpdate(
    "UPDATE objects SET payload_size=payload_size-1 WHERE object_name=?");
  probe = node.core().probeObjectMetadata(objectName);
  BOOST_CHECK(probe.payloadMetadataAvailable);
  BOOST_CHECK(probe.payloadPresent);
  BOOST_CHECK(!probe.payloadRepresentationValid);
  BOOST_CHECK_EQUAL(probe.payloadSize, payload.size());

  executeObjectUpdate(
    "UPDATE objects SET payload=CAST(payload AS TEXT),payload_size=5 WHERE object_name=?");
  probe = node.core().probeObjectMetadata(objectName);
  BOOST_CHECK(probe.payloadMetadataAvailable);
  BOOST_CHECK(probe.payloadPresent);
  BOOST_CHECK(!probe.payloadRepresentationValid);
  BOOST_CHECK_EQUAL(probe.payloadSize, payload.size());

  executeObjectUpdate(
    "UPDATE objects SET payload=CAST(substr(payload,1,2) AS BLOB),payload_size=2 WHERE object_name=?");
  probe = node.core().probeObjectMetadata(objectName);
  BOOST_CHECK(probe.payloadMetadataAvailable);
  BOOST_CHECK(probe.payloadPresent);
  BOOST_CHECK(probe.payloadRepresentationValid);
  BOOST_CHECK_EQUAL(probe.payloadSize, 2U);

  executeObjectUpdate("UPDATE objects SET manifest_json=CAST(manifest_json AS BLOB) WHERE object_name=?");
  probe = node.core().probeObjectMetadata(objectName);
  BOOST_CHECK(!probe.manifestMeasurementAvailable);
  BOOST_CHECK_EQUAL(probe.manifestWireBytes, 0U);
  BOOST_REQUIRE(node.core().remove(objectName));
  probe = node.core().probeObjectMetadata(objectName);
  BOOST_CHECK(!probe.manifestPresent);
  BOOST_CHECK(!probe.payloadMetadataAvailable);
}

BOOST_AUTO_TEST_CASE(AcceptedPublishRecoversFromSeedReceiptAfterOwnerRestart)
{
  using namespace std::chrono_literals;
  TempRoot temp;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec192-publish-restart",
                                   "tpm-memory:spec192-publish-restart");
  const auto providerName = ndn::Name("/spec192/provider-restart");
  const auto callerName = ndn::Name("/spec192/publisher-restart");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto authorityCert = makeRsaIdentity(
    keyChain, ndn::Name("/spec192/authority-restart"));
  const ndn::Name servicePrefix(RepoClient::DEFAULT_SERVICE_NAME);
  const std::string repoNode = "/logical/restart-seed";
  const auto database = (temp.path() / "artifact-v2.sqlite").string();
  const auto objectRoot = temp.path() / "objects";
  const std::vector<uint8_t> bytes{0x53, 0x70, 0x65, 0x63, 0x31, 0x39, 0x32};
  const auto reference = makeReference("restart-receipt", bytes);
  const RepoArtifactTarget seed{
    repoNode, providerName.toUri(ndn::name::UriFormat::ALTERNATE), servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE)};
  const RepoArtifactTarget other{
    "/logical/restart-replica", "/spec192/provider-replica",
    servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE)};
  const std::string key = "spec192-restart-idempotency-key";
  PublishOperationIntent intent;
  intent.idempotencyKeyDigest = digestPublishIdempotencyKey(key);
  intent.reference = reference;
  intent.generation = 1;
  intent.seed = seed;
  intent.consistency = "ONE";
  intent.targets = {seed, other};
  const auto operationId = makePublishOperationId(
    callerName.toUri(ndn::name::UriFormat::ALTERNATE), seed, intent.idempotencyKeyDigest);
  const auto storeService = makeRepoServiceName(servicePrefix, "STORE");
  const auto statusService = makeRepoServiceName(servicePrefix, "STATUS");
  const std::string storageOwner = "/user/restart-owner";
  const std::string storageGrantId = "restart-seed-grant";
  RepoUserStorageBinding storageBinding;
  ndnsf_distributed_repo::RepoArtifactCommitReceipt seedReceiptBeforeRestart;
  {
    ndn_service_framework::ServiceProvider provider(
      ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
      ndn::Name("/spec192/group-restart"), providerCert, authorityCert,
      std::filesystem::absolute("examples/trust-any.conf").string());
    StorageCapability capability;
    capability.repoNode = repoNode;
    capability.repoMode = "persistent";
    capability.freeBytes = 1024 * 1024;
    auto node = std::make_shared<RepoNode>(
      servicePrefix, capability,
      std::make_shared<FilesystemRepoStoreBackend>(
        objectRoot.string(), 4096, 1024, "spec192-restart-seed"),
      repoNode, providerName.toUri(ndn::name::UriFormat::ALTERNATE), database);
    node->registerServices(provider);

    const auto grantExpiry = static_cast<uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + 60000;
    auto grantResponse = dispatch(provider, ndn::Name(storageOwner),
      makeRepoServiceName(servicePrefix, "USER_STORAGE_GRANT"),
      toBytes(RepoUserStorageGrantRequest{1, storageGrantId,
        callerName.toUri(ndn::name::UriFormat::ALTERNATE),
        artifactStorageObjectName(reference), grantExpiry}.toJson()), "restart-owner-grant");
    BOOST_REQUIRE_MESSAGE(grantResponse.getStatus(), grantResponse.getErrorInfo());
    const auto grant = RepoUserStorageGrantReply::fromJson(responseString(grantResponse)).grant;
    storageBinding = RepoUserStorageBinding{1, storageOwner, storageGrantId, grant.ownerEpoch};
    intent.schemaVersion = 2;
    intent.userStorageBindings = {
      RepoUserStorageTargetBinding{1, seed, storageBinding,
        callerName.toUri(ndn::name::UriFormat::ALTERNATE)},
      RepoUserStorageTargetBinding{1, other,
        RepoUserStorageBinding{1, storageOwner, "restart-replica-grant", 1},
        providerName.toUri(ndn::name::UriFormat::ALTERNATE)},
    };
    intent.requestDigest = makePublishRequestDigest(
      reference, 1, seed, intent.consistency, intent.targets,
      intent.userStorageBindings);

    const auto intentJson = intent.toJson();
    std::vector<uint8_t> intentWire(intentJson.begin(), intentJson.end());
    const auto foreign = dispatch(provider, ndn::Name("/spec192/foreign-publisher"),
                                  storeService, intentWire, "foreign-publish-intent");
    BOOST_CHECK(!foreign.getStatus());
    BOOST_CHECK_NE(foreign.getErrorInfo().find("repo-publish-producer-mismatch"),
                   std::string::npos);
    BOOST_CHECK_EQUAL(queryCount(database, "artifact_publish_operations"), 0);
    auto response = dispatch(provider, callerName, storeService, intentWire,
                             "publish-intent");
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    BOOST_CHECK_EQUAL(responseString(response), operationId);

    ArtifactStoreRequest store;
    store.operationId = operationId;
    store.generation = 1;
    store.reference = reference;
    store.schemaVersion = 3;
    store.storageBinding = storageBinding;
    store.bytes = bytes;

    auto mismatchedSeedStore = store;
    mismatchedSeedStore.generation = 2;
    response = dispatch(provider, callerName, storeService,
                        mismatchedSeedStore.toWire(), "seed-store-wrong-generation");
    BOOST_CHECK(!response.getStatus());
    BOOST_CHECK(!node->core().has(artifactStorageObjectName(reference)));
    BOOST_CHECK_EQUAL(queryCount(database, "artifact_persistent_receipts"), 0);

    response = dispatch(provider, callerName, storeService, store.toWire(),
                        "seed-store");
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    seedReceiptBeforeRestart =
      RepoArtifactCommitReceipt::fromJson(responseString(response));
    BOOST_CHECK_EQUAL(seedReceiptBeforeRestart.operationId, operationId);

    const PublishOperationStatusRequest query{1, operationId};
    const auto queryJson = query.toJson();
    response = dispatch(provider, callerName, statusService,
      std::vector<uint8_t>(queryJson.begin(), queryJson.end()), "accepted-status");
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    const auto accepted = PublishOperationStatus::fromJson(responseString(response));
    BOOST_CHECK_EQUAL(accepted.state, "ACCEPTED");
    const auto seedProgress = std::find_if(
      accepted.targets.begin(), accepted.targets.end(), [&seed] (const auto& target) {
        return target.target == seed;
      });
    BOOST_REQUIRE(seedProgress != accepted.targets.end());
    BOOST_REQUIRE(seedProgress->receipt);
    BOOST_CHECK_EQUAL(seedProgress->receipt->receiptId,
                      seedReceiptBeforeRestart.receiptId);
    BOOST_CHECK(accepted.receiptId.empty());
  }

  {
    auto publisherUser = std::make_shared<ndn_service_framework::ServiceUser>(
      ndn_service_framework::ServiceUser::LocalMockTag{}, face,
      ndn::Name("/spec192/group-restart"), providerCert, authorityCert,
      std::filesystem::absolute("examples/trust-any.conf").string());
    publisherUser->useSigningKeyChainForSigningOnlyForTest(keyChain);
    ndn_service_framework::ServiceProvider provider(
      ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
      ndn::Name("/spec192/group-restart"), providerCert, authorityCert,
      std::filesystem::absolute("examples/trust-any.conf").string());
    StorageCapability capability;
    capability.repoNode = repoNode;
    capability.repoMode = "persistent";
    capability.freeBytes = 1024 * 1024;
    auto node = std::make_shared<RepoNode>(
      servicePrefix, capability,
      std::make_shared<FilesystemRepoStoreBackend>(
        objectRoot.string(), 4096, 1024, "spec192-restart-seed"),
      repoNode, providerName.toUri(ndn::name::UriFormat::ALTERNATE), database);
    node->registerServices(provider, publisherUser, 100, 1000);

    const PublishOperationStatusRequest query{1, operationId};
    const auto queryJson = query.toJson();
    const auto queryWire = std::vector<uint8_t>(queryJson.begin(), queryJson.end());
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    PublishOperationStatus recovered;
    for (;;) {
      const auto response = dispatch(provider, callerName, statusService,
                                     queryWire, "recovered-status");
      BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
      recovered = PublishOperationStatus::fromJson(responseString(response));
      if (recovered.state == "COMMITTED" || recovered.state == "PARTIAL" ||
          recovered.state == "FAILED") {
        break;
      }
      BOOST_REQUIRE(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(5ms);
    }
    BOOST_CHECK_EQUAL(recovered.state, "COMMITTED");
    BOOST_REQUIRE(!recovered.receiptId.empty());
    const auto recoveredSeed = std::find_if(
      recovered.targets.begin(), recovered.targets.end(), [&seed] (const auto& target) {
        return target.target == seed;
      });
    BOOST_REQUIRE(recoveredSeed != recovered.targets.end());
    BOOST_REQUIRE(recoveredSeed->receipt);
    BOOST_CHECK_EQUAL(recoveredSeed->receipt->receiptId,
                      seedReceiptBeforeRestart.receiptId);
    const auto skippedReplica = std::find_if(
      recovered.targets.begin(), recovered.targets.end(), [&other] (const auto& target) {
        return target.target == other;
      });
    BOOST_REQUIRE(skippedReplica != recovered.targets.end());
    BOOST_CHECK_EQUAL(skippedReplica->state, "NOT_ATTEMPTED");

    const auto aggregateJson = queryOperationValue(
      database, "artifact_publish_receipts", "receipt_json", operationId);
    const auto aggregate = ArtifactCommitReceipt::fromJson(aggregateJson);
    BOOST_CHECK_EQUAL(aggregate.operationId, operationId);
    BOOST_CHECK_EQUAL(aggregate.receiptId, recovered.receiptId);
    BOOST_CHECK_EQUAL(aggregate.state, "COMMITTED");
    BOOST_CHECK_EQUAL(queryCount(database, "artifact_persistent_receipts"), 1);
    BOOST_CHECK_EQUAL(queryCount(database, "artifact_publish_receipts"), 1);
    BOOST_CHECK_EQUAL(queryOperationValue(
      database, "artifact_publish_operations", "idempotency_key_digest", operationId),
      intent.idempotencyKeyDigest);
    const auto persistedIntent = queryOperationValue(
      database, "artifact_publish_operations", "intent_json", operationId);
    BOOST_CHECK_EQUAL(persistedIntent.find(key), std::string::npos);
    BOOST_CHECK_EQUAL(node->core().getManifest(
      artifactStorageObjectName(reference)).operationId, operationId);

    const auto unauthorized = dispatch(provider, ndn::Name("/spec192/other-user"),
      statusService, queryWire, "unauthorized-status");
    BOOST_CHECK(!unauthorized.getStatus());
    std::this_thread::sleep_for(10ms);
    const auto repeated = dispatch(provider, callerName, statusService,
      queryWire, "repeated-terminal-status");
    BOOST_REQUIRE_MESSAGE(repeated.getStatus(), repeated.getErrorInfo());
    BOOST_CHECK_EQUAL(responseString(repeated), recovered.toJson());
    BOOST_CHECK_EQUAL(queryOperationValue(
      database, "artifact_publish_receipts", "receipt_json", operationId),
      aggregateJson);
  }
}

BOOST_AUTO_TEST_CASE(PersistentHandlersRejectUnboundLegacyAndArtifactWrites)
{
  TempRoot temp;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-unbound-write",
                                   "tpm-memory:spec204-unbound-write");
  const auto providerName = ndn::Name("/spec204/unbound-write-repo");
  const auto providerCert = makeRsaIdentity(keyChain, providerName);
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/unbound-write-authority"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
    ndn::Name("/spec204/group"), providerCert, authorityCert,
    std::filesystem::absolute("examples/trust-any.conf").string());
  StorageCapability capability;
  capability.repoNode = "/repo/unbound-write";
  capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  const auto objectRoot = temp.path() / "objects";
  auto node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>(
      objectRoot.string(), 4096, 1024, "unbound-write"),
    capability.repoNode, providerName.toUri(ndn::name::UriFormat::ALTERNATE), database);
  node->registerServices(provider);

  const auto requester = ndn::Name("/spec192/publisher-restart");
  const auto seed = RepoArtifactTarget{capability.repoNode,
    providerName.toUri(ndn::name::UriFormat::ALTERNATE),
    node->servicePrefix().toUri(ndn::name::UriFormat::ALTERNATE)};
  const auto storeService = makeRepoServiceName(node->servicePrefix(), "STORE");
  const auto denied = [&] (const ndn::Name& service,
                           const std::vector<uint8_t>& payload,
                           const std::string& requestId) {
    const auto response = dispatch(provider, requester, service, payload, requestId);
    BOOST_CHECK_MESSAGE(!response.getStatus(), requestId + " unexpectedly succeeded");
    BOOST_CHECK_NE(response.getErrorInfo().find("repo-user-storage-binding-required"),
                   std::string::npos);
  };

  const std::vector<uint8_t> legacyBytes{0x6c, 0x65, 0x67, 0x61, 0x63, 0x79};
  const auto legacyReference = makeReference("unbound-legacy-intent", legacyBytes);
  PublishOperationIntent legacyIntent;
  legacyIntent.idempotencyKeyDigest = digestPublishIdempotencyKey("unbound-legacy-intent");
  legacyIntent.reference = legacyReference;
  legacyIntent.generation = 1;
  legacyIntent.seed = seed;
  legacyIntent.consistency = "ONE";
  legacyIntent.targets = {seed};
  legacyIntent.requestDigest = makePublishRequestDigest(
    legacyReference, 1, seed, "ONE", legacyIntent.targets);
  const auto legacyJson = legacyIntent.toJson();
  denied(storeService, toBytes(legacyJson), "unbound-legacy-intent");

  const auto rejectUnboundArtifactWrite = [&] (const std::string& suffix,
                                                const ndn::Name& service,
                                                const std::vector<uint8_t>& payload) {
    denied(service, payload, suffix);
  };
  const std::vector<uint8_t> bytes{0x75, 0x6e, 0x62, 0x6f, 0x75, 0x6e, 0x64};

  const auto storeReference = makeReference("unbound-store", bytes);
  ArtifactStoreRequest store;
  store.operationId = "unbound-store-operation";
  store.generation = 1;
  store.reference = storeReference;
  store.bytes = bytes;
  rejectUnboundArtifactWrite("unbound-store", storeService, store.toWire());

  const auto rangeReference = makeReference("unbound-range", bytes);
  ArtifactRangeWriteRequest range;
  range.operationId = "unbound-range-operation";
  range.generation = 1;
  range.reference = rangeReference;
  range.offsetBytes = 0;
  range.lengthBytes = bytes.size();
  range.bytes = bytes;
  rejectUnboundArtifactWrite("unbound-store-range",
    makeRepoServiceName(node->servicePrefix(), "STORE_RANGE"), range.toWire());

  ArtifactRangeCommitRequest commit;
  commit.operationId = "unbound-range-operation";
  commit.generation = 1;
  commit.reference = rangeReference;
  rejectUnboundArtifactWrite("unbound-commit-ranges",
    makeRepoServiceName(node->servicePrefix(), "COMMIT_RANGES"), commit.toWire());

  ArtifactRangeAbortRequest abort;
  abort.operationId = "unbound-range-operation";
  abort.generation = 1;
  abort.reference = rangeReference;
  rejectUnboundArtifactWrite("unbound-abort-ranges",
    makeRepoServiceName(node->servicePrefix(), "ABORT_RANGES"), abort.toWire());

  BOOST_CHECK(!node->core().has(artifactStorageObjectName(legacyReference)));
  BOOST_CHECK(!node->core().has(artifactStorageObjectName(storeReference)));
  BOOST_CHECK(!node->core().has(artifactStorageObjectName(rangeReference)));
  BOOST_CHECK(!std::filesystem::exists(objectRoot / "staging" /
    (rangeReference.contentDigest + ".1.part")));
  BOOST_CHECK_EQUAL(queryCount(database, "artifact_publish_operations"), 0);
  BOOST_CHECK_EQUAL(queryCount(database, "artifact_persistent_receipts"), 0);
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
                    "No handler registered for " +
                    makeRepoServiceName(ndn::Name("/NDNSF/DistributedRepo"), "CAPABILITY").toUri());
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

BOOST_AUTO_TEST_CASE(FilesystemVectorReadRejectsCorruptCommittedPayload)
{
  TempRoot temporary;
  FilesystemRepoStoreBackend store(
    (temporary.path() / "objects").string(), 4096, 1024,
    "spec193-o05-vector-integrity");
  const std::vector<uint8_t> source{0x10, 0x20, 0x30, 0x40, 0x50};
  const auto manifest = RepoClient::makeManifest(
    "/spec193/o05/vector", "application/octet-stream", source, 1, {}, "");
  store.put(manifest, source);
  BOOST_CHECK(store.get(manifest.objectName).payload == source);

  const auto payloadPath = temporary.path() / "objects" / "payloads" / "sha256" /
                           manifest.sha256.substr(0, 2) / manifest.sha256;
  auto altered = source;
  altered.front() ^= 0x01;
  {
    std::ofstream output(payloadPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(altered.data()), altered.size());
    BOOST_REQUIRE(output.good());
  }
  BOOST_CHECK_EXCEPTION(store.get(manifest.objectName), std::runtime_error,
    [] (const std::runtime_error& error) {
      return std::string(error.what()).find("repo-file-vector-payload-corrupt") !=
             std::string::npos;
    });
  {
    std::ofstream output(payloadPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(source.data()), source.size());
    BOOST_REQUIRE(output.good());
  }
  BOOST_CHECK(store.get(manifest.objectName).payload == source);
}

BOOST_AUTO_TEST_CASE(UserStorageGrantHandlersSeparateOwnersRetryAndRejectSpoof)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/owner-handlers";
  capability.repoMode = "persistent";
  const auto database = (temp.path() / "metadata.sqlite3").string();
  RepoNode node(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "owner-handlers"),
    capability.repoNode, "/repo/provider", database);
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  RepoUserStorageGrantRequest request{1, "same-id", "/provider/a", "/producer/kv", now + 60000};
  const auto wire = toBytes(request.toJson());
  const auto first = RepoUserStorageGrantReply::fromJson(toString(node.handleUserStorageGrant(wire, "/user/u")));
  const auto retry = RepoUserStorageGrantReply::fromJson(toString(node.handleUserStorageGrant(wire, "/user/u")));
  BOOST_CHECK_EQUAL(first.grant.repoNode, capability.repoNode);
  BOOST_CHECK_EQUAL(first.grant.ownerIdentity, "/user/u");
  BOOST_CHECK_EQUAL(first.grant.writerIdentity, "/provider/a");
  BOOST_CHECK_EQUAL(first.grant.objectName, "/producer/kv");
  BOOST_CHECK_EQUAL(first.grant.ownerEpoch, 1U);
  BOOST_CHECK_EQUAL(retry.toJson(), first.toJson());
  const auto other = RepoUserStorageGrantReply::fromJson(toString(node.handleUserStorageGrant(wire, "/user/v")));
  BOOST_CHECK_EQUAL(other.grant.ownerIdentity, "/user/v");
  BOOST_CHECK_EQUAL(queryCount(database, "repo_user_storage_grants"), 2);
  request.writerIdentity = "/provider/b";
  BOOST_CHECK_THROW(node.handleUserStorageGrant(toBytes(request.toJson()), "/user/u"), std::runtime_error);
  request.grantId = "expired"; request.expiresAtMs = 1;
  BOOST_CHECK_THROW(node.handleUserStorageGrant(toBytes(request.toJson()), "/user/u"), std::invalid_argument);
  auto spoof = toString(wire); spoof.pop_back(); spoof += ",\"ownerIdentity\":\"/user/v\"}";
  BOOST_CHECK_THROW(node.handleUserStorageGrant(toBytes(spoof), "/user/u"), std::invalid_argument);
  BOOST_CHECK_THROW(node.handleUserStorageGrant(wire, "/"), std::invalid_argument);
  BOOST_CHECK_EQUAL(queryCount(database, "repo_user_storage_grants"), 2);
  RepoNode local(ndn::Name("/service/local"), capability,
    makeTieredRepoStore((temp.path() / "local.sqlite3").string(), 4096));
  BOOST_CHECK_THROW(local.handleUserStorageGrant(wire, "/user/u"), std::runtime_error);
  BOOST_CHECK_THROW(local.handleUserStorageUsage(toBytes("{\"schemaVersion\":1}"), "/user/u"), std::runtime_error);
}

void
checkSqliteStartupAccounting(const std::vector<uint8_t>& bytes)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/sqlite-accounting"; capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto payloadDatabase = (temp.path() / "payload.sqlite3").string();
  const auto journalDatabase = (temp.path() / "metadata.sqlite3").string();
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/sqlite-object"; manifest.objectType = "artifact-v2-bytes";
  manifest.sha256 = sha256(bytes); manifest.size = bytes.size();
  manifest.generation = 1; manifest.operationId = "sqlite-operation";
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  {
    SqliteArtifactMetadataStore journal(journalDatabase);
    BOOST_REQUIRE(journal.reconcileUserStorageInventory(capability.repoNode, {}, now).ready);
    const auto grant = journal.grantUserStorage("/user/u", capability.repoNode, "grant",
      "/provider/a", manifest.objectName, now + 60000, now);
    (void)journal.reserveUserStorage("/provider/a", capability.repoNode, "/user/u",
      grant.grantId, manifest.operationId, {manifest.objectName, manifest.sha256, 1, bytes.size(), 1}, now);
    BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).reservedBytes, bytes.size() + 1);
    const auto backend = makeSqliteRepoStore(payloadDatabase);
    backend->put(manifest, bytes);
    const auto probe = backend->probeObjectMetadata(manifest.objectName);
    BOOST_CHECK(probe.payloadPresent);
    BOOST_CHECK(probe.payloadRepresentationValid);
    BOOST_CHECK_EQUAL(probe.payloadSize, bytes.size());
    BOOST_CHECK(backend->get(manifest.objectName).payload == bytes);
  }
  const auto makeNode = [&] {
    return std::make_unique<RepoNode>(ndn::Name("/service/repo"), capability,
      makeSqliteRepoStore(payloadDatabase), capability.repoNode, "/provider/a", journalDatabase);
  };
  auto node = makeNode();
  const auto expectedCharge = bytes.size() + manifest.toJson().size();
  {
    SqliteArtifactMetadataStore journal(journalDatabase, false, 16, false);
    BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).reservedBytes, 0U);
    BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, expectedCharge);
    BOOST_CHECK_EQUAL(journal.pendingUserStorageWrites(capability.repoNode).size(), 0U);
  }
  node.reset(); node = makeNode();
  BOOST_CHECK_EQUAL(node->core().probeObjectMetadata(manifest.objectName).manifestWireBytes, manifest.toJson().size());
  node.reset();
  sqlite3* raw = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_open_v2(payloadDatabase.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
  BOOST_REQUIRE_EQUAL(sqlite3_exec(database.get(),
    "UPDATE objects SET manifest_json=manifest_json||char(10)||'  '", nullptr, nullptr, nullptr), SQLITE_OK);
  BOOST_CHECK_THROW(makeNode(), std::runtime_error);
  SqliteArtifactMetadataStore retained(journalDatabase, false, 16, false);
  BOOST_CHECK_EQUAL(retained.userStorageUsage("/user/u", capability.repoNode).committedBytes, expectedCharge);
  BOOST_CHECK_EQUAL(retained.userStorageUsage("/user/u", capability.repoNode).reservedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(PersistentPublisherWaitsForAuthenticatedSeedStatusBeforeOwnerAuthorization)
{
  TempRoot temporary;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-publisher-owner", "tpm-memory:spec204-publisher-owner");
  const auto publisher = makeRsaIdentity(keyChain, ndn::Name("/spec192/publisher-restart"));
  const auto authority = makeRsaIdentity(keyChain, ndn::Name("/spec204/authority"));
  ndn_service_framework::ServiceUser user(ndn_service_framework::ServiceUser::LocalMockTag{},
    face, ndn::Name("/spec204/group"), publisher, authority,
    std::filesystem::absolute("examples/trust-any.conf").string());
  const std::vector<uint8_t> sourceBytes{1, 2};
  const auto ref = makeReference("owner-callback", sourceBytes);
  const auto sourcePath = temporary.path() / "owner-callback.bin";
  {
    std::ofstream sourceFile(sourcePath, std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE(sourceFile.good());
    sourceFile.write(reinterpret_cast<const char*>(sourceBytes.data()),
                     static_cast<std::streamsize>(sourceBytes.size()));
    BOOST_REQUIRE(sourceFile.good());
  }
  SelectedRepoSet selected;
  selected.seedRepoId = "/repo/a"; selected.consistency = RepoWriteConsistency::All;
  selected.targets = {{"/repo/b", ndn::Name("/provider/b"), ndn::Name(RepoClient::DEFAULT_SERVICE_NAME)},
                      {"/repo/a", ndn::Name("/provider/a"), ndn::Name(RepoClient::DEFAULT_SERVICE_NAME)}};
  unsigned calls = 0;
  PersistentRepoClientConfig config;
  config.userStorageOwnerIdentity = user.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  config.userStorageAuthorizer = [&] (const PublishOperationIntent&, auto) {
    ++calls;
    return std::vector<RepoUserStorageTargetBinding>{};
  };
  auto api = ArtifactRepositoryApi::makePersistent(user, config);
  BOOST_CHECK_EXCEPTION(api.beginPublishFile(sourcePath.string(), ref,
    selected, "owner-key", 1000), std::exception, [] (const auto& error) {
      return std::string(error.what()).find("repo-") != std::string::npos;
    });
  BOOST_CHECK_EQUAL(calls, 0U);
}

BOOST_AUTO_TEST_CASE(OwnerRegistrationValidatesDescriptorExpiryAndSharedDeadline)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-owner-register", "tpm-memory:spec204-owner-register");
  const auto identity = makeRsaIdentity(keyChain, ndn::Name("/user/u"));
  const auto authority = makeRsaIdentity(keyChain, ndn::Name("/spec204/authority"));
  ndn_service_framework::ServiceUser owner(ndn_service_framework::ServiceUser::LocalMockTag{},
    face, ndn::Name("/spec204/group"), identity, authority,
    std::filesystem::absolute("examples/trust-any.conf").string());
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  BOOST_CHECK_THROW(ArtifactRepositoryApi::makeUserStorageAuthorizer(owner, 0), std::invalid_argument);
  BOOST_CHECK_THROW(ArtifactRepositoryApi::makeUserStorageAuthorizer(owner, UINT64_MAX), std::invalid_argument);
  BOOST_CHECK_THROW(ArtifactRepositoryApi::makeUserStorageAuthorizer(owner, now + 60000, 0, 100), std::invalid_argument);
  BOOST_CHECK_THROW(ArtifactRepositoryApi::makeUserStorageAuthorizer(owner, now + 60000, 100, 100), std::invalid_argument);
  PersistentRepoClientConfig config;
  config.userStorageAuthorizer = [] (const PublishOperationIntent&, auto) {
    return std::vector<RepoUserStorageTargetBinding>{};
  };
  BOOST_CHECK_THROW(ArtifactRepositoryApi::makePersistent(owner, config), ArtifactValidationError);
  config.userStorageOwnerIdentity = "/";
  BOOST_CHECK_THROW(ArtifactRepositoryApi::makePersistent(owner, config), std::invalid_argument);
  config.userStorageOwnerIdentity = owner.getName().toUri(ndn::name::UriFormat::ALTERNATE);
  BOOST_CHECK_NO_THROW(ArtifactRepositoryApi::makePersistent(owner, config));
  auto registerGrants = ArtifactRepositoryApi::makeUserStorageAuthorizer(owner, now + 60000, 10, 100);
  PublishOperationIntent provisional;
  provisional.schemaVersion = 2; provisional.reference = makeReference("register", {1, 2});
  provisional.generation = 1; provisional.idempotencyKeyDigest = digestPublishIdempotencyKey("register-key");
  provisional.seed = {"/repo/a", "/provider/a", "/service/repo"};
  provisional.targets = {provisional.seed}; provisional.consistency = "ONE";
  BOOST_CHECK_EXCEPTION(registerGrants(provisional, std::chrono::steady_clock::now()),
    std::runtime_error, [] (const auto& error) {
      return std::string(error.what()).find("repo-operation-timeout") == 0;
    });
  auto invalid = provisional; invalid.schemaVersion = 1;
  BOOST_CHECK_THROW(registerGrants(invalid, std::chrono::steady_clock::now() + std::chrono::seconds(1)), std::invalid_argument);
  invalid = provisional; invalid.requestDigest = std::string(64, 'a');
  BOOST_CHECK_THROW(registerGrants(invalid, std::chrono::steady_clock::now() + std::chrono::seconds(1)), std::invalid_argument);
  invalid = provisional; invalid.targets.clear();
  BOOST_CHECK_THROW(registerGrants(invalid, std::chrono::steady_clock::now() + std::chrono::seconds(1)), std::exception);
  invalid = provisional; invalid.reference.logicalName = "/another-producer/object";
  BOOST_CHECK_THROW(registerGrants(invalid, std::chrono::steady_clock::now() + std::chrono::seconds(1)), std::exception);
}

BOOST_AUTO_TEST_CASE(SqliteStartupSettlesMeasuredManifestAndRejectsChargeDrift)
{
  checkSqliteStartupAccounting({1, 2, 3, 4});
  checkSqliteStartupAccounting({});
}

BOOST_AUTO_TEST_CASE(OwnedUserStageAbortRequiresFullIdentityAndRestoresCapacity)
{
  TempRoot temp;
  const auto root = temp.path() / "objects";
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "owned-abort");
  StorageCapability capability; capability.repoNode = "/repo/owned-abort"; capability.freeBytes = 4096;
  RepoCore core(capability, backend);
  const std::vector<uint8_t> bytes{1, 2, 3};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/owned-stage"; manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "owned-stage";
  core.putRange(manifest, {0, 2}, {1, 2});
  const auto stage = root / "staging" / (manifest.sha256 + ".1.part");
  BOOST_REQUIRE(std::filesystem::exists(stage));
  BOOST_REQUIRE(std::filesystem::exists(stage.string() + ".ranges"));
  auto changed = manifest; changed.operationId = "other-operation";
  BOOST_CHECK(!core.abortUserStorageRangesIfOwned(changed));
  BOOST_CHECK(!backend->abortUserStorageRangesIfOwned(changed));
  changed = manifest; changed.generation = 0;
  BOOST_CHECK(!core.abortUserStorageRangesIfOwned(changed));
  changed = manifest; changed.policyEpoch = "other-policy";
  BOOST_CHECK(!core.abortUserStorageRangesIfOwned(changed));
  BOOST_CHECK(std::filesystem::exists(stage));
  BOOST_CHECK(core.abortUserStorageRangesIfOwned(manifest));
  BOOST_CHECK(!std::filesystem::exists(stage));
  BOOST_CHECK(!std::filesystem::exists(stage.string() + ".ranges"));
  BOOST_CHECK(!std::filesystem::exists(stage.string() + ".finalize-intent"));
  BOOST_CHECK(!core.abortUserStorageRangesIfOwned(manifest));
  const std::vector<uint8_t> full(4096, 8);
  auto capacity = manifest; capacity.objectName = "/producer/full-capacity";
  capacity.operationId = "full-capacity"; capacity.sha256 = sha256(full); capacity.size = full.size();
  BOOST_CHECK_NO_THROW(core.putRange(capacity, {0, 0}, {}));
  BOOST_CHECK(core.abortUserStorageRangesIfOwned(capacity));
  BOOST_CHECK_EQUAL(backend->size(), 0U);
}

BOOST_AUTO_TEST_CASE(OwnedUserStageAbortPreservesSameDigestAliasesAcrossGenerations)
{
  for (const uint64_t generation : {1U, 2U}) {
    TempRoot temp;
    const auto root = temp.path() / "objects";
    auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "alias-abort");
    StorageCapability capability; capability.repoNode = "/repo/alias-abort"; capability.freeBytes = 4096;
    RepoCore core(capability, backend);
    const std::vector<uint8_t> bytes{1, 2, 3};
    RepoObjectManifest manifest;
    manifest.objectName = "/producer/stage-a"; manifest.sha256 = sha256(bytes);
    manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "stage-a";
    auto alias = manifest; alias.objectName = "/producer/stage-b";
    alias.operationId = "stage-b"; alias.generation = generation;
    core.putRange(manifest, {0, 2}, {1, 2}); core.putRange(alias, {0, 2}, {1, 2});
    const auto stageA = root / "staging" / (manifest.sha256 + ".1.part");
    const auto stageB = root / "staging" / (manifest.sha256 + "." + std::to_string(generation) + ".part");
    BOOST_REQUIRE(std::filesystem::exists(stageA)); BOOST_REQUIRE(std::filesystem::exists(stageB));
    BOOST_CHECK(!core.abortUserStorageRangesIfOwned(manifest));
    BOOST_CHECK(!core.abortUserStorageRangesIfOwned(alias));
    BOOST_CHECK(!backend->abortUserStorageRangesIfOwned(manifest));
    BOOST_CHECK(!backend->abortUserStorageRangesIfOwned(alias));
    BOOST_CHECK(std::filesystem::exists(stageA)); BOOST_CHECK(std::filesystem::exists(stageB));
  }
}

BOOST_AUTO_TEST_CASE(OwnedUserStageAbortRefusesUnknownAndLinkedStagingBeforeDeletion)
{
  TempRoot temp;
  const auto root = temp.path() / "objects";
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "guarded-abort");
  StorageCapability capability; capability.repoNode = "/repo/guarded-abort"; capability.freeBytes = 4096;
  RepoCore core(capability, backend);
  const std::vector<uint8_t> bytes{1, 2, 3};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/guarded-stage"; manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "guarded-stage";
  core.putRange(manifest, {0, 2}, {1, 2});
  const auto stage = root / "staging" / (manifest.sha256 + ".1.part");
  const auto unknown = root / "staging" / (manifest.sha256 + ".99.part");
  { std::ofstream output(unknown); output << "unresolved"; BOOST_REQUIRE(output.good()); }
  BOOST_CHECK(!core.abortUserStorageRangesIfOwned(manifest));
  BOOST_CHECK(std::filesystem::exists(stage)); BOOST_CHECK_EQUAL(std::filesystem::file_size(unknown), 10U);
  std::filesystem::remove(unknown); // Fixture resolves this unrelated unknown holder explicitly.
  const std::filesystem::path ranges = stage.string() + ".ranges";
  const auto saved = temp.path() / "external-ranges";
  BOOST_REQUIRE(std::filesystem::exists(ranges));
  const auto savedSize = std::filesystem::file_size(ranges);
  std::filesystem::rename(ranges, saved); std::filesystem::create_symlink(saved, ranges);
  BOOST_CHECK(!core.abortUserStorageRangesIfOwned(manifest));
  BOOST_CHECK(std::filesystem::exists(stage)); BOOST_CHECK(std::filesystem::is_symlink(ranges));
  BOOST_CHECK_EQUAL(std::filesystem::file_size(saved), savedSize);
  std::filesystem::remove(ranges); std::filesystem::rename(saved, ranges);
  const auto savedDirectory = temp.path() / "saved-staging";
  std::filesystem::rename(root / "staging", savedDirectory);
  std::filesystem::create_directory_symlink(savedDirectory, root / "staging");
  BOOST_CHECK_THROW(core.abortUserStorageRangesIfOwned(manifest), std::runtime_error);
  BOOST_CHECK(std::filesystem::exists(savedDirectory / stage.filename()));
  BOOST_CHECK(std::filesystem::exists(savedDirectory / ranges.filename()));
  std::filesystem::remove(root / "staging"); std::filesystem::rename(savedDirectory, root / "staging");
  BOOST_CHECK(core.abortUserStorageRangesIfOwned(manifest));
  BOOST_CHECK(!std::filesystem::exists(stage)); BOOST_CHECK(!std::filesystem::exists(ranges));
}

BOOST_AUTO_TEST_CASE(OwnedUserStageAbortRequiresOwnershipForOrphansAndRefusesUnsupportedStores)
{
  TempRoot temp;
  const auto root = temp.path() / "objects";
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "orphan-abort");
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/orphan-stage"; manifest.sha256 = sha256({1, 2, 3});
  manifest.size = 3; manifest.generation = 1; manifest.operationId = "orphan-stage";
  BOOST_CHECK(backend->abortUserStorageRangesIfOwned(manifest)); // Proven absent; no reservation invented.
  const auto stage = root / "staging" / (manifest.sha256 + ".1.part");
  { std::ofstream output(stage); output << "unresolved"; BOOST_REQUIRE(output.good()); }
  BOOST_CHECK(!backend->abortUserStorageRangesIfOwned(manifest));
  BOOST_CHECK_EQUAL(std::filesystem::file_size(stage), 10U);
  auto missing = manifest; missing.operationId.clear();
  BOOST_CHECK(!backend->abortUserStorageRangesIfOwned(missing));
  missing = manifest; missing.generation = 0;
  BOOST_CHECK(!backend->abortUserStorageRangesIfOwned(missing));
  BOOST_CHECK(!makeSqliteRepoStore((temp.path() / "objects.sqlite3").string())->abortUserStorageRangesIfOwned(manifest));
  BOOST_CHECK(std::filesystem::exists(stage));
}

BOOST_AUTO_TEST_CASE(FailedRollbackProtectsPriorOrphanAndRequiresVerifiedSharing)
{
  TempRoot temp;
  const auto root = temp.path() / "objects";
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "failed-rollback");
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  const ArtifactReference reference{"/artifact/prior-orphan", "sha256", sha256(bytes), bytes.size(),
    "artifact-manifest-v2", "/producer/manifest/prior-orphan", "/producer", "epoch"};
  FilesystemArtifactPayloadStore payload(root.string(), 4096);
  payload.begin(reference, 1);
  payload.writeRange(reference, 1, {0, bytes.size()}, bytes);
  payload.markVerified(reference, 1, {0, bytes.size()});
  payload.flush(reference, 1);
  payload.finalize(reference, 1);
  StorageCapability capability; capability.repoNode = "/repo/failed-rollback"; capability.freeBytes = 4096;
  RepoCore core(capability, backend);
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/failed-orphan-user"; manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "failed-orphan-user";
  core.putRange(manifest, {0, bytes.size()}, bytes);
  BOOST_CHECK(!core.abortFailedRangesIfOwned(manifest));
  BOOST_CHECK_EQUAL(core.reapExpiredRangeReservations(std::chrono::steady_clock::duration::zero()), 0U);
  BOOST_CHECK(payload.readRange(reference, 1, {0, bytes.size()}) == bytes);
  auto peer = manifest; peer.objectName = "/producer/peer"; peer.operationId = "peer";
  backend->put(peer, bytes); // Actual durable reference; no invented sharing claim.
  const auto uncertain = root / "manifests" / "unknown.json";
  { std::ofstream output(uncertain); output << "unresolved"; BOOST_REQUIRE(output.good()); }
  BOOST_CHECK(!core.abortFailedRangesIfOwned(manifest));
  BOOST_CHECK(backend->get(peer.objectName).payload == bytes);
  std::filesystem::remove(uncertain); // Fixture explicitly resolves its malformed row.
  const auto good = backend->getManifest(peer.objectName).toJson();
  const auto peerPath = root / "manifests" /
    (sha256(std::vector<uint8_t>(peer.objectName.begin(), peer.objectName.end())) + ".json");
  auto wrongRootSize = peer; wrongRootSize.size = 999;
  auto metadataRoot = peer; metadataRoot.objectType = "ndn-segmented-data"; metadataRoot.segmentCount = 2;
  const std::vector<std::string> invalid{
    good.substr(0, good.size() - 1),
    "{\"objectName\":\"" + peer.objectName + "\"," + good.substr(1),
    "{\"shadow\":{\"size\":4}," + wrongRootSize.toJson().substr(1),
    "{\"shadow\":{\"objectType\":\"object\",\"segmentCount\":1}," + metadataRoot.toJson().substr(1)};
  for (const auto& bad : invalid) {
    { std::ofstream output(peerPath); output << bad; BOOST_REQUIRE(output.good()); }
    BOOST_CHECK(!core.abortFailedRangesIfOwned(manifest));
    BOOST_CHECK(payload.readRange(reference, 1, {0, bytes.size()}) == bytes);
    { std::ofstream output(peerPath); output << good; BOOST_REQUIRE(output.good()); }
  }
  BOOST_CHECK(core.abortFailedRangesIfOwned(manifest));
  BOOST_CHECK(core.abortFailedRangesIfOwned(manifest)); // Confirmed absence is idempotent.
  BOOST_CHECK(!backend->abortFailedRangesIfOwned(peer)); // Matching committed transaction is not rollback.
  BOOST_CHECK(!makeSqliteRepoStore((temp.path() / "unsupported.sqlite3").string())->abortFailedRangesIfOwned(manifest));
  BOOST_CHECK(backend->get(peer.objectName).payload == bytes);
}

BOOST_AUTO_TEST_CASE(CommittedRangeRetryRejectsInvalidPhysicalRepresentationBeforeMetadata)
{
  for (bool directory : {false, true}) {
    TempRoot temp;
    const auto root = temp.path() / "objects";
    auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "retry-invalid-payload");
    const std::vector<uint8_t> bytes{1, 2, 3, 4};
    RepoObjectManifest manifest;
    manifest.objectName = "/producer/invalid-retry"; manifest.sha256 = sha256(bytes);
    manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "invalid-retry";
    backend->put(manifest, bytes);
    const auto before = backend->getManifest(manifest.objectName).toJson();
    const auto path = root / "payloads" / "sha256" / manifest.sha256.substr(0, 2) / manifest.sha256;
    if (directory) {
      std::filesystem::remove(path);
      std::filesystem::create_directory(path);
    }
    else std::filesystem::resize_file(path, 2);
    BOOST_CHECK_EXCEPTION(backend->commitRanges(manifest), std::runtime_error,
      [](const std::runtime_error& error) {
        return std::string(error.what()) == "repo-file-committed-payload-invalid";
      });
    BOOST_CHECK_EQUAL(backend->getManifest(manifest.objectName).toJson(), before);
    BOOST_CHECK(directory ? std::filesystem::is_directory(path) : std::filesystem::file_size(path) == 2U);
  }
}

BOOST_AUTO_TEST_CASE(FailedRollbackUsesTieredAuthorityWithoutDeletingOlderCommit)
{
  TempRoot temp;
  auto authority = std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "tiered-failed-rollback");
  auto tiered = makeTieredRepoStore(authority, 4096, "file", 1024);
  const std::vector<uint8_t> oldBytes{1, 2, 3};
  RepoObjectManifest old;
  old.objectName = "/producer/tiered-failed-stage"; old.sha256 = sha256(oldBytes);
  old.size = oldBytes.size(); old.generation = 1; old.operationId = "committed";
  authority->put(old, oldBytes);
  StorageCapability capability; capability.repoNode = "/repo/tiered-failed-rollback"; capability.freeBytes = 4096;
  RepoCore core(capability, tiered);
  const std::vector<uint8_t> next{4, 5, 6};
  auto replacement = old; replacement.sha256 = sha256(next);
  replacement.generation = 2; replacement.operationId = "replacement";
  core.putRange(replacement, {0, 2}, {4, 5});
  BOOST_CHECK(tiered->get(old.objectName).payload == oldBytes);
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 1U);
  auto wrong = replacement; wrong.operationId = "other";
  BOOST_CHECK(!tiered->abortFailedRangesIfOwned(wrong));
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 1U);
  BOOST_CHECK(core.abortFailedRangesIfOwned(replacement));
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 0U);
  BOOST_CHECK(authority->get(old.objectName).payload == oldBytes);
}

BOOST_AUTO_TEST_CASE(OwnedUserStageAbortUsesTieredAuthorityAndInvalidatesOnlyAfterSuccess)
{
  TempRoot temp;
  const auto root = temp.path() / "objects";
  auto authority = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "tiered-abort");
  auto tiered = makeTieredRepoStore(authority, 4096, "file", 1024);
  const std::vector<uint8_t> oldBytes{1, 2, 3};
  RepoObjectManifest old;
  old.objectName = "/producer/tiered-stage"; old.sha256 = sha256(oldBytes);
  old.size = oldBytes.size(); old.generation = 1; old.operationId = "committed";
  authority->put(old, oldBytes);
  StorageCapability capability; capability.repoNode = "/repo/tiered-abort"; capability.freeBytes = 4096;
  RepoCore core(capability, tiered);
  const std::vector<uint8_t> bytes{4, 5, 6};
  auto manifest = old; manifest.sha256 = sha256(bytes); manifest.generation = 2; manifest.operationId = "replacement";
  core.putRange(manifest, {0, 2}, {4, 5});
  BOOST_CHECK(tiered->get(old.objectName).payload == oldBytes);
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 1U);
  auto wrong = manifest; wrong.operationId = "other";
  BOOST_CHECK(!tiered->abortUserStorageRangesIfOwned(wrong));
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 1U);
  BOOST_CHECK(core.abortUserStorageRangesIfOwned(manifest));
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 0U);
  BOOST_CHECK_EQUAL(authority->getManifest(old.objectName).operationId, "committed");
  BOOST_CHECK(tiered->get(old.objectName).payload == oldBytes);
  BOOST_CHECK(!std::filesystem::exists(root / "staging" / (manifest.sha256 + ".2.part")));
}

BOOST_AUTO_TEST_CASE(FencedUserRemovalRejectsDriftAndPreservesSharedOrUnknownStaging)
{
  TempRoot temp;
  const auto root = temp.path() / "objects";
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "fenced-removal");
  const std::vector<uint8_t> bytes{1, 2, 3};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/fenced"; manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "put";
  backend->put(manifest, bytes);
  const RepoUserStorageObject expected{manifest.objectName, manifest.sha256, 1, bytes.size(), manifest.toJson().size()};
  StorageCapability capability; capability.repoNode = "/repo/fenced"; capability.freeBytes = 4096;
  RepoCore core(capability, backend);
  for (int field = 0; field < 4; ++field) {
    auto wrong = expected;
    if (field == 0) ++wrong.generation;
    if (field == 1) ++wrong.manifestBytes;
    if (field == 2) ++wrong.payloadWireBytes;
    if (field == 3) wrong.wireSha256 = std::string(64, 'a');
    BOOST_CHECK(!core.removeUserStorageObject(wrong));
    BOOST_CHECK(core.get(manifest.objectName) == bytes);
  }
  auto alias = manifest; alias.objectName = "/producer/staged-alias";
  alias.generation = 2; alias.operationId = "stage";
  const auto payload = root / "payloads" / "sha256" / manifest.sha256.substr(0, 2) / manifest.sha256;
  const auto retained = temp.path() / "retained-payload";
  // Establish real partial staging while the committed digest is temporarily
  // absent; restore it before the cleanup action. Normal dedup would skip staging.
  std::filesystem::rename(payload, retained);
  core.putRange(alias, {0, 1}, {bytes[0]});
  std::filesystem::rename(retained, payload);
  const auto stage = root / "staging" / (manifest.sha256 + ".2.part");
  BOOST_REQUIRE(std::filesystem::exists(stage));
  BOOST_CHECK(!core.removeUserStorageObject(expected));
  BOOST_CHECK(core.get(manifest.objectName) == bytes);
  BOOST_CHECK(std::filesystem::exists(stage));
  core.abortRanges(alias.objectName);
  alias.generation = 3;
  std::filesystem::rename(payload, retained);
  backend->putRange(alias, {0, 1}, {bytes[0]}); // No Core reservation: backend check must protect this alias.
  std::filesystem::rename(retained, payload);
  BOOST_CHECK(!core.removeUserStorageObject(expected));
  BOOST_CHECK(core.get(manifest.objectName) == bytes);
  backend->abortRanges(alias.objectName);
  const auto unknown = root / "staging" / (manifest.sha256 + ".7.part.ranges");
  { std::ofstream output(unknown); output << "unresolved"; BOOST_REQUIRE(output.good()); }
  BOOST_CHECK(!core.removeUserStorageObject(expected));
  BOOST_CHECK(std::filesystem::exists(unknown));
  BOOST_CHECK(core.get(manifest.objectName) == bytes);
  std::filesystem::remove(unknown); // Explicit fixture settlement, not claimed owner cleanup.
  const std::vector<uint8_t> otherBytes{9, 9, 9};
  auto unrelated = alias; unrelated.objectName = "/producer/unrelated-stage";
  unrelated.sha256 = sha256(otherBytes);
  backend->putRange(unrelated, {0, 1}, {otherBytes[0]});
  const auto otherStage = root / "staging" / (unrelated.sha256 + ".3.part");
  BOOST_REQUIRE(std::filesystem::exists(otherStage));
  BOOST_CHECK(core.removeUserStorageObject(expected));
  BOOST_CHECK(std::filesystem::exists(otherStage));
  BOOST_CHECK(core.removeUserStorageObject(expected));
  BOOST_CHECK_THROW(core.getManifest(manifest.objectName), std::out_of_range);
  BOOST_CHECK(!backend->RepoStoreBackend::canRemoveUserStorageObject(expected));
}

BOOST_AUTO_TEST_CASE(FencedUserRemovalRetainsUntrackedStagingAfterBackendReopen)
{
  TempRoot temp;
  const auto root = temp.path() / "objects";
  const std::vector<uint8_t> bytes{1, 7};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/reopened-removal"; manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "put";
  auto alias = manifest; alias.objectName = "/producer/reopened-alias"; alias.generation = 2;
  const auto stage = root / "staging" / (manifest.sha256 + ".2.part");
  {
    FilesystemRepoStoreBackend initial(root.string(), 4096, 1024, "fenced-initial");
    initial.putRange(alias, {0, 1}, {bytes[0]});
    initial.put(manifest, bytes);
    BOOST_REQUIRE(std::filesystem::exists(stage));
  }
  // Healthy-catalog startup deliberately reclaims staging. An uncertain sidecar
  // takes the existing conservative recovery branch, leaving an untracked stage.
  const auto uncertain = root / "manifests" / "uncertain.json";
  { std::ofstream output(uncertain); output << "invalid"; BOOST_REQUIRE(output.good()); }
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "fenced-reopened");
  std::filesystem::remove(uncertain); // Explicit fixture catalog repair; staging is still unresolved.
  StorageCapability capability; capability.repoNode = "/repo/fenced"; capability.freeBytes = 4096;
  RepoCore core(capability, backend);
  const RepoUserStorageObject expected{manifest.objectName, manifest.sha256, 1, bytes.size(), manifest.toJson().size()};
  BOOST_REQUIRE(std::filesystem::exists(stage));
  BOOST_CHECK(!core.removeUserStorageObject(expected));
  BOOST_CHECK(std::filesystem::exists(stage));
  BOOST_CHECK(core.get(manifest.objectName) == bytes);
  for (const auto& suffix : {"", ".ranges", ".finalize-intent"})
    std::filesystem::remove(stage.string() + suffix); // Fixture's explicit physical settlement only.
  BOOST_CHECK(core.removeUserStorageObject(expected));
  BOOST_CHECK_THROW(core.getManifest(manifest.objectName), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(RecoveryUncertainManifestPreservesOrphanPayloadAndStaging)
{
  const std::vector<uint8_t> bytes{2, 5};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/recovery-uncertain"; manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "put";
  const std::vector<std::string> invalid{"invalid", "{}", manifest.toJson(),
    "{\"objectName\":\"/producer/a\",\"objectName\":\"/producer/b\"}"};
  for (const auto& wire : invalid) {
    TempRoot temp;
    const auto root = temp.path() / "objects";
    auto alias = manifest; alias.objectName = "/producer/partial-alias"; alias.generation = 2;
    const auto stage = root / "staging" / (manifest.sha256 + ".2.part");
    const auto payload = root / "payloads" / "sha256" / manifest.sha256.substr(0, 2) / manifest.sha256;
    {
      FilesystemRepoStoreBackend initial(root.string(), 4096, 1024, "uncertain-initial");
      initial.putRange(alias, {0, 1}, {bytes[0]});
      initial.put(manifest, bytes);
      BOOST_REQUIRE(std::filesystem::exists(stage));
      std::vector<std::filesystem::path> paths;
      for (const auto& entry : std::filesystem::directory_iterator(root / "manifests"))
        paths.push_back(entry.path());
      BOOST_REQUIRE_EQUAL(paths.size(), 1U);
      std::filesystem::remove(paths.front()); // Actual committed orphan, no manifest ownership proof.
      std::ofstream output(root / "manifests" / "uncertain.json"); output << wire;
      BOOST_REQUIRE(output.good());
    }
    FilesystemRepoStoreBackend reopened(root.string(), 4096, 1024, "uncertain-reopened");
    BOOST_CHECK(std::filesystem::exists(stage));
    BOOST_REQUIRE(std::filesystem::exists(payload));
    BOOST_CHECK_EQUAL(std::filesystem::file_size(payload), bytes.size());
    std::ifstream input(payload, std::ios::binary);
    const std::vector<uint8_t> retained{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    BOOST_CHECK(retained == bytes);
    BOOST_CHECK(std::filesystem::exists(root / "manifests" / "uncertain.json"));
  }
}

BOOST_AUTO_TEST_CASE(FencedUserRemovalRequiresRetryAfterAmbiguousDirectorySync)
{
  TempRoot temp;
  auto backend = std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "fenced-sync");
  const std::vector<uint8_t> bytes{5};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/fenced-sync"; manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "put";
  backend->put(manifest, bytes);
  const RepoUserStorageObject expected{manifest.objectName, manifest.sha256, 1, 1, manifest.toJson().size()};
  StorageCapability capability; capability.repoNode = "/repo/fenced"; capability.freeBytes = 4096;
  RepoCore core(capability, backend);
  {
    DirectoryFsyncFailureScope faults;
    BOOST_CHECK_THROW(core.removeUserStorageObject(expected), std::runtime_error);
  }
  BOOST_CHECK(core.removeUserStorageObject(expected));
  const auto published = detail::parseJson(toString(core.handleCapability()), 1024 * 1024);
  const auto& values = detail::requireObject(published);
  BOOST_CHECK_EQUAL(detail::member(values, "usedBytes", detail::JsonValue::Kind::Unsigned).number, backend->usedBytes());
  BOOST_CHECK_EQUAL(detail::member(values, "usedBytes", detail::JsonValue::Kind::Unsigned).number, 0U);
  BOOST_CHECK_EQUAL(detail::member(values, "freeBytes", detail::JsonValue::Kind::Unsigned).number, 4096U);
  BOOST_CHECK(core.removeUserStorageObject(expected));
  BOOST_CHECK_THROW(core.getManifest(manifest.objectName), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(FencedUserRemovalRejectsLinkedDirectoriesBeforeErase)
{
  TempRoot temp;
  const auto root = temp.path() / "objects";
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4096, 1024, "fenced-links");
  const std::vector<uint8_t> bytes{6};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/fenced-links"; manifest.sha256 = sha256(bytes);
  manifest.size = 1; manifest.generation = 1; manifest.operationId = "put";
  backend->put(manifest, bytes);
  const RepoUserStorageObject expected{manifest.objectName, manifest.sha256, 1, 1, manifest.toJson().size()};
  StorageCapability capability; capability.repoNode = "/repo/fenced"; capability.freeBytes = 4096;
  RepoCore core(capability, backend);
  const auto external = temp.path() / "external";
  std::filesystem::create_directory(external);
  { std::ofstream output(external / manifest.sha256); output << "untouched"; BOOST_REQUIRE(output.good()); }
  const std::vector<std::filesystem::path> paths{root / "data-receipts", root / "staging",
    root / "payloads" / "sha256" / manifest.sha256.substr(0, 2)};
  for (const auto& path : paths) {
    const auto backup = temp.path() / "directory-backup";
    std::filesystem::rename(path, backup);
    std::filesystem::create_directory_symlink(external, path);
    BOOST_CHECK_THROW(core.removeUserStorageObject(expected), std::runtime_error);
    BOOST_CHECK_EQUAL(backend->getManifest(manifest.objectName).sha256, manifest.sha256);
    std::ifstream input(external / manifest.sha256); std::string content; input >> content;
    BOOST_CHECK_EQUAL(content, "untouched");
    std::filesystem::remove(path);
    std::filesystem::rename(backup, path);
    BOOST_CHECK(core.get(manifest.objectName) == bytes);
  }
  BOOST_CHECK(core.removeUserStorageObject(expected));
}

BOOST_AUTO_TEST_CASE(FencedUserRemovalUsesSqliteTieredAuthorityAndPreservesOtherRows)
{
  TempRoot temp;
  auto authority = makeSqliteRepoStore((temp.path() / "objects.sqlite3").string());
  const std::vector<uint8_t> bytes{3};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/sqlite-fenced"; manifest.sha256 = sha256(bytes);
  manifest.size = 1; manifest.generation = 1;
  auto alias = manifest; alias.objectName = "/producer/sqlite-alias";
  authority->put(manifest, bytes); authority->put(alias, bytes);
  auto tiered = makeTieredRepoStore(authority, 4096, "sqlite", 1024);
  BOOST_CHECK(tiered->get(manifest.objectName).payload == bytes);
  StorageCapability capability; capability.repoNode = "/repo/fenced"; capability.freeBytes = 4096;
  RepoCore core(capability, tiered);
  const RepoUserStorageObject expected{manifest.objectName, manifest.sha256, 1, 1, manifest.toJson().size()};
  BOOST_CHECK(core.removeUserStorageObject(expected));
  BOOST_CHECK(core.removeUserStorageObject(expected));
  BOOST_CHECK(tiered->get(alias.objectName).payload == bytes);
  BOOST_CHECK_THROW(tiered->get(manifest.objectName), std::out_of_range);
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 1U);
}

BOOST_AUTO_TEST_CASE(UserRemovalConfirmationRejectsAmbiguousCoreDeletionUntilDurableRetry)
{
  TempRoot temp;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  auto backend = std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "removal");
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/removal"; manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size(); manifest.generation = 1; manifest.operationId = "remove";
  backend->put(manifest, bytes);
  const RepoUserStorageObject expected{manifest.objectName, manifest.sha256, 1,
    bytes.size(), manifest.toJson().size()};
  StorageCapability capability; capability.repoNode = "/repo/removal"; capability.freeBytes = 4096;
  RepoCore core(capability, backend);
  const auto payload = temp.path() / "objects" / "payloads" / "sha256" /
    manifest.sha256.substr(0, 2) / manifest.sha256;
  BOOST_CHECK(!core.confirmUserStorageRemoval(expected));
  {
    DirectoryFsyncFailureScope faults;
    BOOST_CHECK(core.remove(manifest.objectName)); // Legacy visibility success is not durability.
    BOOST_CHECK_THROW(core.confirmUserStorageRemoval(expected), std::runtime_error);
    BOOST_CHECK(std::filesystem::exists(payload));
  }
  BOOST_CHECK(core.confirmUserStorageRemoval(expected));
  BOOST_CHECK(!std::filesystem::exists(payload));
  BOOST_CHECK(core.confirmUserStorageRemoval(expected));
  BOOST_CHECK_THROW(backend->getManifest(manifest.objectName), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(UserRemovalConfirmationKeepsSharedDigestAndCleansEmptyOrphan)
{
  TempRoot temp;
  FilesystemRepoStoreBackend backend((temp.path() / "objects").string(), 4096, 1024, "shared-removal");
  const std::vector<uint8_t> bytes{5, 6, 7};
  RepoObjectManifest first;
  first.objectName = "/producer/first"; first.sha256 = sha256(bytes);
  first.size = bytes.size(); first.generation = 1;
  auto second = first; second.objectName = "/producer/second";
  backend.put(first, bytes); backend.put(second, bytes);
  BOOST_REQUIRE(backend.erase(first.objectName));
  BOOST_CHECK(backend.confirmUserStorageRemoval({first.objectName, first.sha256, 1, bytes.size(), first.toJson().size()}));
  BOOST_CHECK(backend.get(second.objectName).payload == bytes);
  const auto shared = temp.path() / "objects" / "payloads" / "sha256" /
    first.sha256.substr(0, 2) / first.sha256;
  BOOST_CHECK(std::filesystem::exists(shared));
  BOOST_REQUIRE(backend.erase(second.objectName));
  BOOST_CHECK(backend.confirmUserStorageRemoval({second.objectName, second.sha256, 1, bytes.size(), second.toJson().size()}));
  BOOST_CHECK(!std::filesystem::exists(shared));
  RepoObjectManifest empty;
  empty.objectName = "/producer/empty"; empty.sha256 = sha256({}); empty.generation = 1;
  backend.put(empty, {});
  const auto emptyPayload = temp.path() / "objects" / "payloads" / "sha256" /
    empty.sha256.substr(0, 2) / empty.sha256;
  BOOST_REQUIRE(std::filesystem::exists(emptyPayload));
  BOOST_REQUIRE(backend.erase(empty.objectName));
  BOOST_CHECK(std::filesystem::exists(emptyPayload)); // Legacy erase leaves this zero-byte orphan.
  BOOST_CHECK(backend.confirmUserStorageRemoval({empty.objectName, empty.sha256, 1, 0, empty.toJson().size()}));
  BOOST_CHECK(!std::filesystem::exists(emptyPayload));
}

BOOST_AUTO_TEST_CASE(UserRemovalConfirmationProtectsActiveAliasStaging)
{
  TempRoot temp;
  FilesystemRepoStoreBackend backend((temp.path() / "objects").string(), 4096, 1024, "staged-removal");
  const std::vector<uint8_t> bytes{9};
  RepoObjectManifest other;
  other.objectName = "/producer/other-stage"; other.sha256 = sha256(bytes);
  other.size = bytes.size(); other.generation = 2;
  backend.putRange(other, {0, 1}, bytes);
  const RepoUserStorageObject removed{"/producer/deleted", other.sha256, 2, 1, 1};
  const auto stage = temp.path() / "objects" / "staging" / (other.sha256 + ".2.part");
  BOOST_REQUIRE(std::filesystem::exists(stage));
  BOOST_CHECK(!backend.confirmUserStorageRemoval(removed));
  BOOST_CHECK(std::filesystem::exists(stage));
  backend.abortRanges(other.objectName);
  BOOST_CHECK(backend.confirmUserStorageRemoval(removed));
  BOOST_CHECK(!std::filesystem::exists(stage));
  auto invalid = removed; invalid.generation = 0;
  BOOST_CHECK_THROW(backend.confirmUserStorageRemoval(invalid), std::invalid_argument);
  BOOST_CHECK(!backend.RepoStoreBackend::confirmUserStorageRemoval(removed));
}

BOOST_AUTO_TEST_CASE(UserRemovalConfirmationRejectsLinkedDirectoriesBeforeCleanup)
{
  TempRoot temp;
  const auto root = temp.path() / "objects";
  FilesystemRepoStoreBackend backend(root.string(), 4096, 1024, "linked-removal");
  const std::string digest = sha256({1});
  const RepoUserStorageObject removed{"/producer/deleted", digest, 2, 1, 1};
  const auto external = temp.path() / "external";
  std::filesystem::create_directory(external);
  const std::vector<std::string> names{digest, digest + ".2.part",
    digest + ".2.part.ranges", digest + ".2.part.finalize-intent"};
  for (const auto& name : names) {
    std::ofstream output(external / name, std::ios::binary);
    output << "untouched";
    BOOST_REQUIRE(output.good());
  }
  const std::vector<std::filesystem::path> paths{
    root / "payloads" / "sha256" / digest.substr(0, 2), root / "staging",
    root / "payloads" / "sha256", root / "payloads", root / "manifests"};
  for (const auto& path : paths) {
    const auto backup = temp.path() / "directory-backup";
    const bool existed = std::filesystem::exists(path);
    if (existed) std::filesystem::rename(path, backup);
    std::filesystem::create_directory_symlink(external, path);
    BOOST_CHECK_THROW(backend.confirmUserStorageRemoval(removed), std::runtime_error);
    for (const auto& name : names) {
      std::ifstream input(external / name, std::ios::binary);
      std::string content;
      input >> content;
      BOOST_CHECK_EQUAL(content, "untouched");
    }
    std::filesystem::remove(path);
    if (existed) std::filesystem::rename(backup, path);
  }
  BOOST_CHECK(backend.confirmUserStorageRemoval(removed));
}

BOOST_AUTO_TEST_CASE(SqliteRemovalConfirmationRequiresCompleteCheckpointAndTieredInvalidation)
{
  TempRoot temp;
  const auto databasePath = (temp.path() / "objects.sqlite3").string();
  auto authority = makeSqliteRepoStore(databasePath);
  const std::vector<uint8_t> bytes{1, 4, 9};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/sqlite-remove"; manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size(); manifest.generation = 1;
  authority->put(manifest, bytes);
  const RepoUserStorageObject expected{manifest.objectName, manifest.sha256, 1,
    bytes.size(), manifest.toJson().size()};
  BOOST_CHECK(!authority->confirmUserStorageRemoval(expected));
  sqlite3* raw = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_open_v2(databasePath.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> reader(raw, sqlite3_close);
  BOOST_REQUIRE_EQUAL(sqlite3_exec(reader.get(), "BEGIN; SELECT COUNT(*) FROM objects;", nullptr, nullptr, nullptr), SQLITE_OK);
  auto tiered = makeTieredRepoStore(authority, 4096, "sqlite", 1024);
  BOOST_CHECK(tiered->get(manifest.objectName).payload == bytes);
  BOOST_REQUIRE_EQUAL(tiered->cacheStatus().entryCount, 1U);
  BOOST_REQUIRE(authority->erase(manifest.objectName));
  BOOST_CHECK(!tiered->confirmUserStorageRemoval(expected));
  BOOST_REQUIRE_EQUAL(sqlite3_exec(reader.get(), "COMMIT", nullptr, nullptr, nullptr), SQLITE_OK);
  BOOST_CHECK(tiered->confirmUserStorageRemoval(expected));
  BOOST_CHECK_EQUAL(tiered->cacheStatus().entryCount, 0U);
  BOOST_CHECK_THROW(tiered->get(manifest.objectName), std::out_of_range);
  BOOST_CHECK(tiered->confirmUserStorageRemoval(expected));
  reader.reset(); tiered.reset(); authority.reset();
  const auto restarted = makeSqliteRepoStore(databasePath);
  BOOST_CHECK(!restarted->has(manifest.objectName));
}

BOOST_AUTO_TEST_CASE(UserStorageUsageReportsOwnedJournalBytesAcrossRestart)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/usage-handlers";
  capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  const auto makeNode = [&] {
    return std::make_unique<RepoNode>(ndn::Name("/service/repo"), capability,
      std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "usage-handlers"),
      capability.repoNode, "/repo/provider", database);
  };
  auto node = makeNode();
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  RepoUserStorageGrantRequest request{1, "first", "/provider/a", "/producer/committed", now + 60000};
  const auto first = RepoUserStorageGrantReply::fromJson(toString(node->handleUserStorageGrant(toBytes(request.toJson()), "/user/u")));
  request.grantId = "pending"; request.objectName = "/producer/pending";
  const auto pendingGrant = RepoUserStorageGrantReply::fromJson(toString(node->handleUserStorageGrant(toBytes(request.toJson()), "/user/u")));
  const auto admittedAt = std::max(first.grant.createdAtMs, pendingGrant.grant.createdAtMs) + 1;
  const std::vector<uint8_t> committedPayload(74, 1);
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/committed"; manifest.objectType = "artifact-v2-bytes";
  manifest.sha256 = sha256(committedPayload); manifest.size = committedPayload.size();
  manifest.generation = 1; manifest.operationId = "committed-op";
  (void)node->core().handleStore(encodeStoreRequest(manifest, committedPayload));
  const auto manifestBytes = node->core().probeObjectMetadata(manifest.objectName).manifestWireBytes;
  {
    SqliteArtifactMetadataStore journal(database);
    BOOST_REQUIRE(journal.reconcileUserStorageInventory(capability.repoNode, {}, admittedAt).ready);
    RepoUserStorageObject committed{"/producer/committed", manifest.sha256, 1, 74, manifestBytes};
    RepoUserStorageObject pending{"/producer/pending", std::string(64, 'b'), 1, 99, 1};
    (void)journal.reserveUserStorage("/provider/a", capability.repoNode, "/user/u", "first", "committed-op", committed, admittedAt + 1);
    (void)journal.commitUserStorage(capability.repoNode, "/user/u", "committed-op", admittedAt + 2);
    (void)journal.reserveUserStorage("/provider/a", capability.repoNode, "/user/u", "pending", "pending-op", pending, admittedAt + 3);
  }
  const auto usageRequest = toBytes(RepoUserStorageUsageRequest{}.toJson());
  for (unsigned restart = 0; restart != 2; ++restart) {
    const auto usage = RepoUserStorageUsageReply::fromJson(toString(node->handleUserStorageUsage(usageRequest, "/user/u"))).usage;
    BOOST_CHECK_EQUAL(usage.repoNode, capability.repoNode);
    BOOST_CHECK_EQUAL(usage.ownerIdentity, "/user/u");
    BOOST_CHECK_EQUAL(usage.limitBytes, 1000000000ULL);
    BOOST_CHECK_EQUAL(usage.committedBytes, committedPayload.size() + manifestBytes);
    BOOST_CHECK_EQUAL(usage.reservedBytes, 100U);
    const auto other = RepoUserStorageUsageReply::fromJson(toString(node->handleUserStorageUsage(usageRequest, "/user/v"))).usage;
    BOOST_CHECK_EQUAL(other.committedBytes, 0U); BOOST_CHECK_EQUAL(other.reservedBytes, 0U);
    const RepoUserStorageListRequest page{1, {}, 1};
    const auto listed = RepoUserStorageListReply::fromJson(toString(
      node->handleUserStorageList(toBytes(page.toJson()), "/user/u")));
    BOOST_REQUIRE_EQUAL(listed.objects.size(), 1U);
    BOOST_CHECK_EQUAL(listed.ownerIdentity, "/user/u");
    BOOST_CHECK_EQUAL(listed.repoNode, capability.repoNode);
    BOOST_CHECK_EQUAL(listed.objects.front().object.objectName, "/producer/committed");
    BOOST_CHECK_EQUAL(listed.objects.front().object.payloadWireBytes +
      listed.objects.front().object.manifestBytes, usage.committedBytes);
    BOOST_CHECK_EQUAL(listed.nextAfterObjectName, "/producer/committed");
    const auto terminal = RepoUserStorageListReply::fromJson(toString(node->handleUserStorageList(
      toBytes(RepoUserStorageListRequest{1, listed.nextAfterObjectName, 1}.toJson()), "/user/u")));
    BOOST_CHECK(terminal.objects.empty()); BOOST_CHECK(terminal.nextAfterObjectName.empty());
    const auto otherList = RepoUserStorageListReply::fromJson(toString(
      node->handleUserStorageList(toBytes(page.toJson()), "/user/v")));
    BOOST_CHECK(otherList.objects.empty()); BOOST_CHECK_EQUAL(otherList.ownerIdentity, "/user/v");
    if (restart == 0) { node.reset(); node = makeNode(); }
  }
  const RepoUserStorageGrantRequest original{1, "first", "/provider/a", "/producer/committed", now + 60000};
  BOOST_CHECK_EQUAL(RepoUserStorageGrantReply::fromJson(toString(node->handleUserStorageGrant(toBytes(original.toJson()), "/user/u"))).toJson(), first.toJson());
  BOOST_CHECK_THROW(node->handleUserStorageUsage(toBytes("{\"schemaVersion\":1,\"ownerIdentity\":\"/user/u\"}"), "/user/v"), std::invalid_argument);
  BOOST_CHECK_EQUAL(queryCount(database, "repo_user_storage_claims"), 1);
  BOOST_CHECK_EQUAL(queryCount(database, "repo_user_storage_writes"), 2);
  TempRoot otherRoot;
  auto otherCapability = capability; otherCapability.repoNode = "/repo/usage-other";
  RepoNode otherNode(ndn::Name("/service/repo"), otherCapability,
    std::make_shared<FilesystemRepoStoreBackend>((otherRoot.path() / "objects").string(), 4096, 1024, "usage-other"),
    otherCapability.repoNode, "/repo/provider-other", (otherRoot.path() / "metadata.sqlite3").string());
  const auto independent = RepoUserStorageUsageReply::fromJson(toString(otherNode.handleUserStorageUsage(usageRequest, "/user/u"))).usage;
  BOOST_CHECK_EQUAL(independent.repoNode, "/repo/usage-other");
  BOOST_CHECK_EQUAL(independent.committedBytes, 0U);
  BOOST_CHECK_EQUAL(independent.reservedBytes, 0U);
  const auto independentList = RepoUserStorageListReply::fromJson(toString(otherNode.handleUserStorageList(
    toBytes(RepoUserStorageListRequest{}.toJson()), "/user/u")));
  BOOST_CHECK(independentList.objects.empty());
  BOOST_CHECK_EQUAL(independentList.repoNode, "/repo/usage-other");
}

BOOST_AUTO_TEST_CASE(UserStorageListShortensLargeEncodedPagesWithoutSkippingOwners)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/list-budget"; capability.repoMode = "persistent";
  const auto database = (temp.path() / "metadata.sqlite3").string();
  RepoNode node(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "list-budget"),
    capability.repoNode, "/provider/a", database);
  SqliteArtifactMetadataStore journal(database, true, 16, true);
  BOOST_REQUIRE(journal.reconcileUserStorageInventory(capability.repoNode, {}, 1000).ready);
  std::vector<std::string> names;
  // Metadata-only fixtures exercise listing's encoding budget; they are not
  // evidence of physical publication or restart reconciliation.
  for (size_t i = 0; i < 300; ++i) {
    const auto id = std::string("grant-") + std::to_string(i);
    const auto name = std::string("/producer/") + std::to_string(10000 + i) + "/" + std::string(3500, 'a');
    names.push_back(name);
    const RepoUserStorageObject object{name, std::string(64, 'a'), 1, 1, 1};
    (void)journal.grantUserStorage("/user/u", capability.repoNode, id, "/provider/a", name, 10000, 1000);
    (void)journal.reserveUserStorage("/provider/a", capability.repoNode, "/user/u", id,
      std::string("write-") + std::to_string(i), object, 1001);
    (void)journal.commitUserStorage(capability.repoNode, "/user/u",
      std::string("write-") + std::to_string(i), 1002);
  }
  size_t observed = 0;
  std::string cursor;
  do {
    const RepoUserStorageListRequest request{1, cursor, 1000};
    const auto wire = node.handleUserStorageList(toBytes(request.toJson()), "/user/u");
    BOOST_REQUIRE_LE(wire.size(), 1024U * 1024U);
    const auto reply = RepoUserStorageListReply::fromJson(toString(wire));
    reply.validateExpected({capability.repoNode, "/provider/a", "/service/repo"}, "/user/u", request);
    BOOST_REQUIRE(!reply.objects.empty());
    if (observed == 0) {
      BOOST_CHECK_LT(reply.objects.size(), names.size());
      BOOST_CHECK(!reply.nextAfterObjectName.empty());
    }
    for (const auto& claim : reply.objects) {
      BOOST_REQUIRE_LT(observed, names.size());
      BOOST_CHECK_EQUAL(claim.object.objectName, names[observed++]);
    }
    cursor = reply.nextAfterObjectName;
  } while (!cursor.empty());
  BOOST_CHECK_EQUAL(observed, names.size());
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 600U);
  const auto other = RepoUserStorageListReply::fromJson(toString(node.handleUserStorageList(
    toBytes(RepoUserStorageListRequest{}.toJson()), "/user/v")));
  BOOST_CHECK(other.objects.empty());
}

// These local fixtures measure real backend objects; byte admission/authentication
// is covered separately by owned STORE/network qualification.
static uint64_t
cleanupFixtureNow()
{
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
}

static RepoUserStorageWrite
recordCleanupOwner(RepoNode& node, SqliteArtifactMetadataStore& journal,
                   const std::string& owner, const std::string& grantId,
                   const std::string& objectName)
{
  const auto now = cleanupFixtureNow();
  const auto grant = RepoUserStorageGrantReply::fromJson(toString(node.handleUserStorageGrant(
    toBytes(RepoUserStorageGrantRequest{1, grantId, "/provider/a", objectName, now + 60000}.toJson()), owner))).grant;
  const auto probe = node.core().probeObjectMetadata(objectName);
  if (!probe.manifestPresent || !probe.manifestMeasurementAvailable)
    throw std::runtime_error("cleanup fixture requires actual measured manifest");
  const RepoUserStorageObject object{objectName, probe.manifest.sha256,
    probe.manifest.generation, probe.manifest.size, probe.manifestWireBytes};
  (void)journal.reserveUserStorage("/provider/a", grant.repoNode, owner, grantId,
    grantId + "-write", object, grant.createdAtMs);
  return journal.commitUserStorage(grant.repoNode, owner, grantId + "-write", grant.createdAtMs);
}

BOOST_AUTO_TEST_CASE(OwnerDeleteConfirmsRealRemovalQuotaReplayAndRestartEpochFence)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/owner-delete"; capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto makeNode = [&] {
    return std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
      std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "owner-delete"),
      capability.repoNode, "/provider/repo", database);
  };
  auto node = makeNode();
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  (void)journal.reconcileUserStorageInventory(capability.repoNode, {}, cleanupFixtureNow());
  const std::vector<uint8_t> bytes{1, 2, 3};
  (void)node->put("/producer/owned", bytes);
  const auto write = recordCleanupOwner(*node, journal, "/user/u", "grant", "/producer/owned");
  const auto charge = write.object.payloadWireBytes + write.object.manifestBytes;
  const RepoUserStorageDeleteRequest request{1, "cleanup", write.object.objectName, write.grant.ownerEpoch};
  const auto wire = toBytes(request.toJson());
  BOOST_CHECK_THROW(node->handleUserStorageDelete(wire, "/user/v"), std::runtime_error);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, charge);
  const auto result = RepoUserStorageDeleteReply::fromJson(toString(node->handleUserStorageDelete(wire, "/user/u")));
  BOOST_CHECK_EQUAL(result.state, "DELETED"); BOOST_CHECK_EQUAL(result.releasedBytes, charge);
  BOOST_CHECK_EQUAL(result.ownerIdentity, "/user/u"); BOOST_CHECK_EQUAL(result.repoNode, capability.repoNode);
  BOOST_CHECK_THROW(node->get(write.object.objectName), std::out_of_range);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 0U);
  BOOST_CHECK(journal.listUserStorage("/user/u", capability.repoNode, {}, 100).empty());
  BOOST_CHECK_THROW(journal.validateUserStorageGrant("/provider/a", capability.repoNode,
    "/user/u", "grant", write.object.objectName, 1, write.grant.createdAtMs + 1), std::runtime_error);
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(toString(node->handleUserStorageDelete(wire, "/user/u"))).releasedBytes, 0U);
  node.reset(); node = makeNode();
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(toString(node->handleUserStorageDelete(wire, "/user/u"))).releasedBytes, 0U);
  (void)node->put(write.object.objectName, bytes);
  const auto fresh = recordCleanupOwner(*node, journal, "/user/u", "fresh", write.object.objectName);
  BOOST_CHECK_EQUAL(fresh.grant.ownerEpoch, 2U);
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(toString(node->handleUserStorageDelete(wire, "/user/u"))).releasedBytes, 0U);
  BOOST_CHECK(node->get(write.object.objectName) == bytes);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes,
    fresh.object.payloadWireBytes + fresh.object.manifestBytes);
}

BOOST_AUTO_TEST_CASE(StartupConfirmsPreparedOwnerRemovalBeforeInventoryAndFreshEpoch)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/cleanup-restart"; capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto makeNode = [&] {
    return std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
      std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "cleanup-restart"),
      capability.repoNode, "/provider/repo", database);
  };
  auto node = makeNode();
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  (void)journal.reconcileUserStorageInventory(capability.repoNode, {}, cleanupFixtureNow());
  const std::vector<uint8_t> bytes{1, 2, 3};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/restart"; manifest.objectType = "artifact-v2-bytes";
  manifest.sha256 = sha256(bytes); manifest.size = bytes.size();
  manifest.generation = 1; manifest.operationId = "restart-publication";
  (void)node->core().handleStore(encodeStoreRequest(manifest, bytes));
  const auto write = recordCleanupOwner(*node, journal, "/user/u", "grant", "/producer/restart");
  const auto charge = write.object.payloadWireBytes + write.object.manifestBytes;
  const auto prepared = journal.prepareUserStorageCleanup("/user/u", capability.repoNode,
    "cleanup", write.object.objectName, 1, cleanupFixtureNow());
  BOOST_CHECK(prepared.state == RepoUserStorageClaimState::CleanupPrepared);
  BOOST_CHECK(node->core().getRange(write.object.objectName, RepoByteRange{0, bytes.size()}) == bytes);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, charge);
  node.reset(); node = makeNode();
  BOOST_CHECK_THROW(node->core().getManifest(write.object.objectName), std::out_of_range);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 0U);
  BOOST_CHECK(journal.listUserStorage("/user/u", capability.repoNode, {}).empty());
  BOOST_CHECK(journal.userStorageReconciliation(capability.repoNode).ready);
  BOOST_CHECK_EQUAL(journal.userStorageReconciliation(capability.repoNode).pendingCleanupCount, 0U);
  BOOST_CHECK_THROW(journal.validateUserStorageGrant("/provider/a", capability.repoNode,
    "/user/u", "grant", write.object.objectName, 1, cleanupFixtureNow()), std::runtime_error);
  const RepoUserStorageDeleteRequest request{1, "cleanup", write.object.objectName, 1};
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(toString(node->handleUserStorageDelete(
    toBytes(request.toJson()), "/user/u"))).releasedBytes, 0U);
  const auto now = cleanupFixtureNow();
  const auto fresh = RepoUserStorageGrantReply::fromJson(toString(node->handleUserStorageGrant(
    toBytes(RepoUserStorageGrantRequest{1, "fresh", "/provider/a", write.object.objectName, now + 60000}.toJson()), "/user/u"))).grant;
  BOOST_CHECK_EQUAL(fresh.ownerEpoch, 2U);
  node.reset(); node = makeNode();
  BOOST_CHECK(journal.userStorageReconciliation(capability.repoNode).ready);
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(toString(node->handleUserStorageDelete(
    toBytes(request.toJson()), "/user/u"))).releasedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(StartupPreparedCleanupRetainsAnotherOwnersSharedPhysicalObject)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/cleanup-shared-restart"; capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto makeNode = [&] {
    return std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
      std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "cleanup-shared-restart"),
      capability.repoNode, "/provider/repo", database);
  };
  auto node = makeNode();
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  (void)journal.reconcileUserStorageInventory(capability.repoNode, {}, cleanupFixtureNow());
  const std::vector<uint8_t> bytes{4, 5, 6};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/shared-restart"; manifest.objectType = "artifact-v2-bytes";
  manifest.sha256 = sha256(bytes); manifest.size = bytes.size();
  manifest.generation = 1; manifest.operationId = "shared-restart-publication";
  (void)node->core().handleStore(encodeStoreRequest(manifest, bytes));
  const auto u = recordCleanupOwner(*node, journal, "/user/u", "u", "/producer/shared-restart");
  const auto v = recordCleanupOwner(*node, journal, "/user/v", "v", "/producer/shared-restart");
  (void)journal.prepareUserStorageCleanup("/user/u", capability.repoNode,
    "cleanup", u.object.objectName, 1, cleanupFixtureNow());
  node.reset(); node = makeNode();
  BOOST_CHECK(node->core().getRange(v.object.objectName, RepoByteRange{0, bytes.size()}) == bytes);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 0U);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/v", capability.repoNode).committedBytes,
    v.object.payloadWireBytes + v.object.manifestBytes);
  BOOST_CHECK(journal.listUserStorage("/user/u", capability.repoNode, {}).empty());
  const auto retained = journal.listUserStorage("/user/v", capability.repoNode, {});
  BOOST_REQUIRE_EQUAL(retained.size(), 1U);
  BOOST_CHECK(retained.front().state == RepoUserStorageClaimState::Committed);
  BOOST_CHECK(journal.userStorageReconciliation(capability.repoNode).ready);
  BOOST_CHECK_EQUAL(journal.userStorageReconciliation(capability.repoNode).pendingCleanupCount, 0U);
  node.reset(); node = makeNode();
  BOOST_CHECK(node->core().getRange(v.object.objectName, RepoByteRange{0, bytes.size()}) == bytes);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/v", capability.repoNode).committedBytes,
    v.object.payloadWireBytes + v.object.manifestBytes);
}

BOOST_AUTO_TEST_CASE(StartupPreparedReservedCleanupConfirmsAbsentPhysicalState)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/cleanup-reserved-restart"; capability.repoMode = "persistent";
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto makeNode = [&] {
    return std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
      std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "cleanup-reserved-restart"),
      capability.repoNode, "/provider/repo", database);
  };
  auto node = makeNode();
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  const auto now = cleanupFixtureNow();
  (void)journal.reconcileUserStorageInventory(capability.repoNode, {}, now);
  const RepoUserStorageObject object{"/producer/reserved-restart", std::string(64, 'a'), 1, 29, 1};
  (void)journal.grantUserStorage("/user/u", capability.repoNode, "grant", "/provider/a", object.objectName, now + 60000, now);
  (void)journal.reserveUserStorage("/provider/a", capability.repoNode, "/user/u", "grant", "write", object, now);
  (void)journal.prepareUserStorageCleanup("/user/u", capability.repoNode, "cleanup", object.objectName, 1, now);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 30U);
  node.reset(); node = makeNode();
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 0U);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).reservedBytes, 0U);
  BOOST_CHECK(journal.userStorageReconciliation(capability.repoNode).ready);
  BOOST_CHECK_EQUAL(journal.userStorageReconciliation(capability.repoNode).pendingCleanupCount, 0U);
  const auto write = journal.lookupUserStorageWrite(capability.repoNode, "/user/u", "write");
  BOOST_REQUIRE(write); BOOST_CHECK(write->state == RepoUserStorageWriteState::Cancelled);
  BOOST_CHECK_THROW(journal.validateUserStorageGrant("/provider/a", capability.repoNode,
    "/user/u", "grant", object.objectName, 1, cleanupFixtureNow()), std::runtime_error);
  const auto result = RepoUserStorageDeleteReply::fromJson(toString(node->handleUserStorageDelete(
    toBytes(RepoUserStorageDeleteRequest{1, "cleanup", object.objectName, 1}.toJson()), "/user/u")));
  BOOST_CHECK_EQUAL(result.state, "DELETED"); BOOST_CHECK_EQUAL(result.releasedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(StartupPreparedCleanupRetainsChargeAndNotReadyForForeignPendingDigest)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/cleanup-pending-restart"; capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto makeNode = [&] {
    return std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
      std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "cleanup-pending-restart"),
      capability.repoNode, "/provider/repo", database);
  };
  auto node = makeNode();
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  (void)journal.reconcileUserStorageInventory(capability.repoNode, {}, cleanupFixtureNow());
  const std::vector<uint8_t> bytes{7, 8, 9};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/cleanup-pending"; manifest.objectType = "artifact-v2-bytes";
  manifest.sha256 = sha256(bytes); manifest.size = bytes.size();
  manifest.generation = 1; manifest.operationId = "pending-restart-publication";
  (void)node->core().handleStore(encodeStoreRequest(manifest, bytes));
  const auto u = recordCleanupOwner(*node, journal, "/user/u", "u", "/producer/cleanup-pending");
  auto pending = u.object; pending.objectName = "/producer/foreign-pending";
  const auto now = cleanupFixtureNow();
  (void)journal.grantUserStorage("/user/v", capability.repoNode, "v", "/provider/b", pending.objectName, now + 60000, now);
  (void)journal.reserveUserStorage("/provider/b", capability.repoNode, "/user/v", "v", "v-write", pending, now);
  (void)journal.prepareUserStorageCleanup("/user/u", capability.repoNode, "cleanup", u.object.objectName, 1, now);
  const auto charge = u.object.payloadWireBytes + u.object.manifestBytes;
  node.reset(); node = makeNode();
  BOOST_CHECK(node->core().getRange(u.object.objectName, RepoByteRange{0, bytes.size()}) == bytes);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, charge);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/v", capability.repoNode).reservedBytes, charge);
  BOOST_CHECK(!journal.userStorageReconciliation(capability.repoNode).ready);
  BOOST_CHECK_EQUAL(journal.userStorageReconciliation(capability.repoNode).pendingCleanupCount, 1U);
  BOOST_CHECK_EQUAL(journal.userStorageReconciliation(capability.repoNode).pendingWriteCount, 1U);
  const auto own = journal.listUserStorage("/user/u", capability.repoNode, {});
  BOOST_REQUIRE_EQUAL(own.size(), 1U);
  BOOST_CHECK(own.front().state == RepoUserStorageClaimState::CleanupPrepared);
  const auto result = RepoUserStorageDeleteReply::fromJson(toString(node->handleUserStorageDelete(
    toBytes(RepoUserStorageDeleteRequest{1, "cleanup", u.object.objectName, 1}.toJson()), "/user/u")));
  BOOST_CHECK_EQUAL(result.state, "PENDING"); BOOST_CHECK_EQUAL(result.releasedBytes, 0U);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, charge);
}

BOOST_AUTO_TEST_CASE(OwnerDeletePreservesSameObjectOwnersAndDigestAliases)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/shared-delete"; capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  RepoNode node(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "shared-delete"),
    capability.repoNode, "/provider/repo", database);
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  (void)journal.reconcileUserStorageInventory(capability.repoNode, {}, cleanupFixtureNow());
  const std::vector<uint8_t> bytes{4, 5, 6};
  (void)node.put("/producer/shared", bytes); (void)node.put("/producer/alias", bytes);
  const auto u = recordCleanupOwner(node, journal, "/user/u", "u", "/producer/shared");
  const auto v = recordCleanupOwner(node, journal, "/user/v", "v", "/producer/shared");
  const auto w = recordCleanupOwner(node, journal, "/user/w", "w", "/producer/alias");
  auto eraseOwner = [&](const std::string& owner, const RepoUserStorageWrite& write) {
    return RepoUserStorageDeleteReply::fromJson(toString(node.handleUserStorageDelete(
      toBytes(RepoUserStorageDeleteRequest{1, "cleanup", write.object.objectName, write.grant.ownerEpoch}.toJson()), owner)));
  };
  BOOST_CHECK_EQUAL(eraseOwner("/user/u", u).state, "DELETED");
  BOOST_CHECK(node.get(u.object.objectName) == bytes);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 0U);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/v", capability.repoNode).committedBytes,
    v.object.payloadWireBytes + v.object.manifestBytes);
  BOOST_CHECK_EQUAL(eraseOwner("/user/v", v).state, "DELETED");
  BOOST_CHECK_THROW(node.get(u.object.objectName), std::out_of_range);
  BOOST_CHECK(node.get(w.object.objectName) == bytes);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/w", capability.repoNode).committedBytes,
    w.object.payloadWireBytes + w.object.manifestBytes);
  BOOST_CHECK_EQUAL(eraseOwner("/user/w", w).state, "DELETED");
  BOOST_CHECK_THROW(node.get(w.object.objectName), std::out_of_range);
  const auto payload = temp.path() / "objects" / "payloads" / "sha256" /
    u.object.wireSha256.substr(0, 2) / u.object.wireSha256;
  BOOST_CHECK(!std::filesystem::exists(payload));
}

BOOST_AUTO_TEST_CASE(OwnerDeleteRetainsChargeForPendingWriterAndUnknownStaging)
{
  TempRoot temp;
  StorageCapability capability;
  capability.repoNode = "/repo/pending-delete"; capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  RepoNode node(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "pending-delete"),
    capability.repoNode, "/provider/repo", database);
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  (void)journal.reconcileUserStorageInventory(capability.repoNode, {}, cleanupFixtureNow());
  const std::vector<uint8_t> bytes{7, 8, 9};
  (void)node.put("/producer/pending", bytes);
  const auto write = recordCleanupOwner(node, journal, "/user/u", "u", "/producer/pending");
  const auto grant = journal.grantUserStorage("/user/v", capability.repoNode, "v", "/provider/a",
    "/producer/alias", write.grant.expiresAtMs, write.grant.createdAtMs);
  auto other = write.object; other.objectName = "/producer/alias";
  (void)journal.reserveUserStorage("/provider/a", capability.repoNode, "/user/v", "v", "pending", other, grant.createdAtMs);
  const auto request = toBytes(RepoUserStorageDeleteRequest{1, "cleanup", write.object.objectName, 1}.toJson());
  auto result = RepoUserStorageDeleteReply::fromJson(toString(node.handleUserStorageDelete(request, "/user/u")));
  BOOST_CHECK_EQUAL(result.state, "PENDING"); BOOST_CHECK_EQUAL(result.releasedBytes, 0U);
  const auto charge = write.object.payloadWireBytes + write.object.manifestBytes;
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, charge);
  BOOST_CHECK(node.get(write.object.objectName) == bytes);
  BOOST_CHECK_THROW(journal.validateUserStorageGrant("/provider/a", capability.repoNode,
    "/user/u", "u", write.object.objectName, 1, write.grant.createdAtMs + 1), std::runtime_error);
  // Fixture settlement is explicit; the handler must not cancel this other owner.
  const auto pendingWrite = journal.lookupUserStorageWrite(capability.repoNode, "/user/v", "pending");
  BOOST_REQUIRE(pendingWrite);
  BOOST_CHECK_EQUAL(pendingWrite->reservedBytes,
    other.payloadWireBytes + other.manifestBytes);
  (void)journal.cancelUserStorage(capability.repoNode, "/user/v", "pending", grant.createdAtMs);
  const auto stage = temp.path() / "objects" / "staging" / (write.object.wireSha256 + ".3.part");
  { std::ofstream output(stage); output << "unresolved"; BOOST_REQUIRE(output.good()); }
  result = RepoUserStorageDeleteReply::fromJson(toString(node.handleUserStorageDelete(request, "/user/u")));
  BOOST_CHECK_EQUAL(result.state, "PENDING"); BOOST_CHECK_EQUAL(result.releasedBytes, 0U);
  BOOST_CHECK(std::filesystem::exists(stage));
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, charge);
  std::filesystem::remove(stage); // Only the fixture resolves unknown ownership.
  result = RepoUserStorageDeleteReply::fromJson(toString(node.handleUserStorageDelete(request, "/user/u")));
  BOOST_CHECK_EQUAL(result.state, "DELETED"); BOOST_CHECK_EQUAL(result.releasedBytes, charge);
}

BOOST_AUTO_TEST_CASE(RegisteredOwnerDeleteFinalizesPublicationBeforeLateWorkerStartup)
{
  TempRoot temp;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-delete-publish", "tpm-memory:spec204-delete-publish");
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/delete-repo"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/delete-authority"));
  const auto trust = std::filesystem::absolute("examples/trust-any.conf").string();
  ndn_service_framework::ServiceProvider provider(ndn_service_framework::ServiceProvider::LocalMockTag{},
    face, ndn::Name("/spec204/group"), providerCert, authorityCert, trust);
  StorageCapability capability;
  capability.repoNode = "/repo/publish-delete"; capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "publish-delete"),
    capability.repoNode, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), database);
  node->registerServices(provider);
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  const std::vector<uint8_t> bytes{10, 11, 12};
  const auto reference = makeReference("owned-delete", bytes);
  const auto writer = ndn::Name(reference.publisherIdentity).toUri(ndn::name::UriFormat::ALTERNATE);
  const auto objectName = artifactStorageObjectName(reference);
  const auto expiry = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count()) + 60000;
  auto response = dispatch(provider, ndn::Name("/user/u"), makeRepoServiceName(node->servicePrefix(), "USER_STORAGE_GRANT"),
    toBytes(RepoUserStorageGrantRequest{1, "grant", writer, objectName, expiry}.toJson()), "grant");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto grant = RepoUserStorageGrantReply::fromJson(responseString(response)).grant;
  PublishOperationIntent intent;
  intent.schemaVersion = 2; intent.reference = reference; intent.generation = 1;
  intent.idempotencyKeyDigest = digestPublishIdempotencyKey("delete-publish");
  intent.seed = {capability.repoNode, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), "/service/repo"};
  intent.consistency = "ONE"; intent.targets = {intent.seed};
  const RepoUserStorageBinding binding{1, "/user/u", "grant", grant.ownerEpoch};
  intent.userStorageBindings = {{1, intent.seed, binding, writer}};
  intent.requestDigest = makePublishRequestDigest(reference, 1, intent.seed, "ONE", intent.targets, intent.userStorageBindings);
  const auto storeService = makeRepoServiceName(node->servicePrefix(), "STORE");
  response = dispatch(provider, ndn::Name(writer), storeService, toBytes(intent.toJson()), "intent");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  ArtifactStoreRequest write;
  write.schemaVersion = 3; write.operationId = responseString(response); write.generation = 1;
  write.reference = reference; write.storageBinding = binding; write.bytes = bytes;
  response = dispatch(provider, ndn::Name(writer), storeService, write.toWire(), "store");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charge = journal.userStorageUsage("/user/u", capability.repoNode).committedBytes;
  BOOST_REQUIRE(charge > bytes.size());
  const auto deleteService = makeRepoServiceName(node->servicePrefix(), "USER_STORAGE_DELETE");
  const auto request = toBytes(RepoUserStorageDeleteRequest{1, "cleanup", objectName, grant.ownerEpoch}.toJson());
  response = dispatch(provider, ndn::Name("/user/v"), deleteService, request, "foreign-delete");
  BOOST_CHECK(!response.getStatus());
  response = dispatch(provider, ndn::Name("/user/u"), deleteService, request, "pending-delete");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto pending = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(pending.state, "DELETED"); BOOST_CHECK_EQUAL(pending.releasedBytes, charge);
  BOOST_CHECK_THROW(node->get(objectName), std::out_of_range);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 0U);
  const auto terminal = journal.lookupPublishOperation(write.operationId, writer);
  BOOST_REQUIRE(terminal); BOOST_CHECK_EQUAL(terminal->state, "COMMITTED");
  BOOST_CHECK(journal.recoverablePublishOperations().empty());
  response = dispatch(provider, ndn::Name(writer), storeService, toBytes(intent.toJson()), "old-intent");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_NE(response.getErrorInfo().find("repo-user-storage-deleted"), std::string::npos);
  auto publisher = std::make_shared<ndn_service_framework::ServiceUser>(
    ndn_service_framework::ServiceUser::LocalMockTag{}, face, ndn::Name("/spec204/group"),
    providerCert, authorityCert, trust);
  node->registerServices(provider, publisher, 1, 100);
  BOOST_CHECK_EQUAL(journal.lookupPublishOperation(write.operationId, writer)->aggregateReceiptJson,
    terminal->aggregateReceiptJson);
  response = dispatch(provider, ndn::Name("/user/u"), deleteService, request, "confirmed-delete");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto deleted = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(deleted.state, "DELETED"); BOOST_CHECK_EQUAL(deleted.releasedBytes, 0U);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 0U);
  BOOST_CHECK_THROW(node->get(objectName), std::out_of_range);
  // History remains immutable; it does not prove current owner availability.
  BOOST_CHECK_EQUAL(journal.lookupPublishOperation(write.operationId, writer)->state, "COMMITTED");
}

BOOST_AUTO_TEST_CASE(UserStorageRegisteredServicesUseTheCoreAdapterActor)
{
  TempRoot temp;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-owner-handler", "tpm-memory:spec204-owner-handler");
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/repo-provider"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/authority"));
  ndn_service_framework::ServiceProvider provider(ndn_service_framework::ServiceProvider::LocalMockTag{},
    face, ndn::Name("/spec204/group"), providerCert, authorityCert,
    std::filesystem::absolute("examples/trust-any.conf").string());
  StorageCapability capability;
  capability.repoNode = "/repo/registered-owner"; capability.repoMode = "persistent";
  auto node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(), 4096, 1024, "registered-owner"),
    capability.repoNode, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), (temp.path() / "metadata.sqlite3").string());
  node->registerServices(provider);
  const auto expiry = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count()) + 60000;
  const auto grantService = makeRepoServiceName(node->servicePrefix(), "USER_STORAGE_GRANT");
  const auto usageService = makeRepoServiceName(node->servicePrefix(), "USER_STORAGE_USAGE");
  const RepoUserStorageGrantRequest request{1, "grant", "/provider/a", "/producer/kv", expiry};
  const auto response = dispatch(provider, ndn::Name("/user/v"), grantService, toBytes(request.toJson()), "grant-owner");
  BOOST_REQUIRE(response.getStatus());
  BOOST_CHECK_EQUAL(RepoUserStorageGrantReply::fromJson(responseString(response)).grant.ownerIdentity, "/user/v");
  const auto usage = dispatch(provider, ndn::Name("/user/u"), usageService, toBytes(RepoUserStorageUsageRequest{}.toJson()), "usage-owner");
  BOOST_REQUIRE(usage.getStatus());
  BOOST_CHECK_EQUAL(RepoUserStorageUsageReply::fromJson(responseString(usage)).usage.ownerIdentity, "/user/u");
  const auto spoof = dispatch(provider, ndn::Name("/user/u"), usageService,
    toBytes("{\"schemaVersion\":1,\"ownerIdentity\":\"/user/v\"}"), "usage-spoof");
  BOOST_CHECK(!spoof.getStatus());
  BOOST_CHECK_NE(spoof.getErrorInfo().find("artifact-json-invalid"), std::string::npos);
  const auto listService = makeRepoServiceName(node->servicePrefix(), "USER_STORAGE_LIST");
  const auto list = dispatch(provider, ndn::Name("/user/u"), listService,
    toBytes(RepoUserStorageListRequest{}.toJson()), "list-owner");
  BOOST_REQUIRE(list.getStatus());
  const auto listed = RepoUserStorageListReply::fromJson(responseString(list));
  BOOST_CHECK_EQUAL(listed.ownerIdentity, "/user/u"); BOOST_CHECK(listed.objects.empty());
  const auto listSpoof = dispatch(provider, ndn::Name("/user/u"), listService,
    toBytes("{\"schemaVersion\":1,\"afterObjectName\":\"\",\"limit\":100,\"ownerIdentity\":\"/user/v\"}"), "list-spoof");
  BOOST_CHECK(!listSpoof.getStatus());
  // LocalMock dispatch proves adapter wiring, not network signature validation.
}

BOOST_AUTO_TEST_CASE(OwnedInsertPreservesOriginalPacketsAndAppliesRepoQuota)
{
  TempRoot temp;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-owned-insert",
                                   "tpm-memory:spec204-owned-insert");
  (void)makeRsaIdentity(keyChain, ndn::Name("/provider/a"));
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/owned-insert-repo"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/owned-insert-authority"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
    ndn::Name("/spec204/group"), providerCert, authorityCert,
    std::filesystem::absolute("examples/trust-any.conf").string());

  StorageCapability capability;
  capability.repoNode = "/repo/owned-insert";
  capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>(
      (temp.path() / "objects").string(), 4096, 1024, "owned-insert"),
    capability.repoNode, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), database);
  node->registerServices(provider);

  const std::vector<uint8_t> content{1, 4, 9, 16};
  ndn::Name dataName("/provider/a/kv/snapshot");
  dataName.appendSegment(0);
  ndn::Data packet(dataName);
  packet.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
  keyChain.sign(packet);
  const auto wireBlock = packet.wireEncode();
  const std::vector<uint8_t> wire(wireBlock.begin(), wireBlock.end());
  ndn::Name fullDataName("/provider/a/kv/full");
  fullDataName.appendSegment(0);
  ndn::Data fullPacket(fullDataName);
  fullPacket.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
  keyChain.sign(fullPacket);
  const auto fullWireBlock = fullPacket.wireEncode();
  const std::vector<uint8_t> fullWire(fullWireBlock.begin(), fullWireBlock.end());

  RepoDataReference reference;
  reference.objectName = dataName.toUri(ndn::name::UriFormat::ALTERNATE);
  reference.dataPrefix = "/provider/a/kv/snapshot";
  reference.firstSegment = 0;
  reference.finalSegment = 0;
  reference.hasFinalSegment = true;
  reference.expectedSha256 = sha256(wire);
  reference.expectedSize = wire.size();
  reference.storeWirePackets = true;

  size_t fetchCount = 0;
  const auto setGrantExpiry = [&database] (uint64_t expiresAtMs) {
    sqlite3* rawDatabase = nullptr;
    if (sqlite3_open_v2(database.c_str(), &rawDatabase,
                        SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
      const std::string error = rawDatabase == nullptr ? "open failed" : sqlite3_errmsg(rawDatabase);
      if (rawDatabase != nullptr) sqlite3_close(rawDatabase);
      throw std::runtime_error("fixture could not open grant journal: " + error);
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> grantDatabase(
      rawDatabase, sqlite3_close);
    sqlite3_stmt* rawStatement = nullptr;
    if (sqlite3_prepare_v2(grantDatabase.get(),
        "UPDATE repo_user_storage_grants SET expires_at_ms=? "
        "WHERE repo_node='/repo/owned-insert' AND owner_identity='/user/u' "
        "AND grant_id='kv-grant'", -1, &rawStatement, nullptr) != SQLITE_OK) {
      throw std::runtime_error("fixture could not prepare grant expiry update");
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      rawStatement, sqlite3_finalize);
    if (sqlite3_bind_int64(statement.get(), 1,
          static_cast<sqlite3_int64>(expiresAtMs)) != SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_DONE ||
        sqlite3_changes(grantDatabase.get()) != 1) {
      throw std::runtime_error("fixture could not update the expected grant expiry");
    }
  };
  std::atomic<bool> expireGrantDuringFetch{false};
  node->setDataReferenceFetcher([&] (const RepoDataReference& requested) {
    ++fetchCount;
    if (expireGrantDuringFetch.exchange(false)) {
      setGrantExpiry(1);
    }
    const ndn::Name requestedName(requested.objectName);
    if (requestedName == dataName)
      return std::vector<std::vector<uint8_t>>{wire};
    if (requestedName == fullDataName)
      return std::vector<std::vector<uint8_t>>{fullWire};
    throw std::runtime_error("fixture received unexpected object name");
  });

  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  const auto expiry = now + 60000;
  const auto grantService = makeRepoServiceName(node->servicePrefix(), "USER_STORAGE_GRANT");
  const auto grantResponse = dispatch(provider, ndn::Name("/user/u"), grantService,
    toBytes(RepoUserStorageGrantRequest{1, "kv-grant", "/provider/a",
      reference.objectName, expiry}.toJson()), "owned-insert-grant");
  BOOST_REQUIRE_MESSAGE(grantResponse.getStatus(), grantResponse.getErrorInfo());
  const auto grant = RepoUserStorageGrantReply::fromJson(responseString(grantResponse)).grant;

  RepoUserStorageInsertRequest request;
  request.operationId = "owned-kv-insert-1";
  request.generation = 7;
  request.reference = reference;
  request.storageBinding = {1, "/user/u", grant.grantId, grant.ownerEpoch};
  const auto encoded = encodeUserStorageInsertRequest(request);
  BOOST_CHECK(encodeUserStorageInsertRequest(decodeUserStorageInsertRequest(encoded)) == encoded);
  const auto insertService = makeRepoServiceName(node->servicePrefix(), "INSERT");
  auto response = dispatch(provider, ndn::Name("/provider/a"), insertService,
                           encoded, "owned-insert-first");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  auto status = parseOperationStatusJson(responseString(response));
  BOOST_REQUIRE_EQUAL(status.state, "DONE");
  BOOST_CHECK_EQUAL(status.operationId, request.operationId);
  BOOST_CHECK_EQUAL(fetchCount, 1U);
  BOOST_CHECK(node->getDataPacket(reference.objectName) == wire);
  auto stored = node->getManifest(reference.objectName);
  BOOST_CHECK_EQUAL(stored.objectType, "original-packet-set");
  BOOST_CHECK_EQUAL(stored.generation, request.generation);
  BOOST_CHECK_EQUAL(stored.packetNames.size(), 1U);
  BOOST_CHECK_EQUAL(stored.packetNames.front(), dataName.toUri(ndn::name::UriFormat::CANONICAL));

  response = dispatch(provider, ndn::Name("/provider/a"), insertService,
                      encoded, "owned-insert-retry");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  status = parseOperationStatusJson(responseString(response));
  BOOST_CHECK_EQUAL(status.state, "DONE");
  BOOST_CHECK_EQUAL(fetchCount, 2U);
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  const auto physical = node->core().probeObjectMetadata(stored.objectName);
  const auto expectedCharge = wire.size() + physical.manifestWireBytes;
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes,
                    expectedCharge);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).reservedBytes, 0U);

  const auto makeRejectedRequest = [&] (const std::string& name, const std::string& opId) {
    auto rejected = request;
    rejected.operationId = opId;
    rejected.reference.objectName = name;
    rejected.reference.dataPrefix = ndn::Name(name).getPrefix(-1).toUri(ndn::name::UriFormat::ALTERNATE);
    return encodeUserStorageInsertRequest(rejected);
  };
  const auto unauthorizedName = "/provider/a/kv/unauthorized/seg=0";
  response = dispatch(provider, ndn::Name("/attacker/a"), insertService,
    makeRejectedRequest(unauthorizedName, "owned-kv-insert-unauthorized"), "owned-insert-wrong-writer");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!node->hasDataPacket(unauthorizedName));
  BOOST_CHECK_EQUAL(fetchCount, 2U);

  const auto ungrantedName = "/provider/a/kv/ungranted/seg=0";
  response = dispatch(provider, ndn::Name("/provider/a"), insertService,
    makeRejectedRequest(ungrantedName, "owned-kv-insert-ungranted"), "owned-insert-wrong-object");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto ungranted = parseOperationStatusJson(responseString(response));
  BOOST_CHECK_EQUAL(ungranted.state, "FAILED");
  BOOST_CHECK_NE(ungranted.message.find("repo-user-storage-unauthorized"), std::string::npos);
  BOOST_CHECK(!node->hasDataPacket(ungrantedName));
  BOOST_CHECK_EQUAL(fetchCount, 2U);

  // Fill an independent owner's live Repo account in the same journal. The
  // INSERT preflight must reject before calling the packet fetcher.
  std::vector<RepoUserStorageInventoryEntry> inventory;
  for (const auto& manifest : node->list()) {
    const auto probe = node->core().probeObjectMetadata(manifest.objectName);
    RepoUserStorageObject object{
      ndn::Name(manifest.objectName).toUri(ndn::name::UriFormat::ALTERNATE),
      manifest.sha256, std::max<uint64_t>(1, manifest.generation),
      probe.payloadSize, probe.manifestWireBytes};
    inventory.push_back({std::move(object), false});
  }
  BOOST_REQUIRE(journal.reconcileUserStorageInventory(capability.repoNode, inventory, now).ready);

  const auto userURemaining = REPO_USER_STORAGE_LIMIT_BYTES - expectedCharge;
  BOOST_REQUIRE_GT(userURemaining, 1U);
  RepoUserStorageObject userUFiller{"/provider/a/kv/filler-u", std::string(64, 'b'), 1,
                                   userURemaining - 1, 1};
  const auto userUFillerGrant = journal.grantUserStorage(
    "/user/u", capability.repoNode, "u-filler-grant", "/provider/a",
    userUFiller.objectName, expiry, now);
  (void)journal.reserveUserStorage("/provider/a", capability.repoNode, "/user/u",
    userUFillerGrant.grantId, "u-filler-write", userUFiller, now);
  (void)journal.commitUserStorage(capability.repoNode, "/user/u", "u-filler-write", now + 1);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes,
                    REPO_USER_STORAGE_LIMIT_BYTES);
  response = dispatch(provider, ndn::Name("/provider/a"), insertService,
                      encoded, "owned-insert-retry-at-limit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(parseOperationStatusJson(responseString(response)).state, "DONE");
  BOOST_CHECK_EQUAL(fetchCount, 3U);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes,
                    REPO_USER_STORAGE_LIMIT_BYTES);

  RepoUserStorageObject fullAccount{"/provider/a/kv/filler", std::string(64, 'a'), 1,
                                    REPO_USER_STORAGE_LIMIT_BYTES - 1, 1};
  const auto fillerGrant = journal.grantUserStorage(
    "/user/full", capability.repoNode, "filler-grant", "/provider/a",
    fullAccount.objectName, expiry, now);
  (void)journal.reserveUserStorage("/provider/a", capability.repoNode, "/user/full",
    fillerGrant.grantId, "filler-write", fullAccount, now);
  (void)journal.commitUserStorage(capability.repoNode, "/user/full", "filler-write", now + 1);

  const auto fullGrantResponse = dispatch(provider, ndn::Name("/user/full"), grantService,
    toBytes(RepoUserStorageGrantRequest{1, "kv-full-grant", "/provider/a",
      "/provider/a/kv/full/seg=0", expiry}.toJson()), "owned-insert-full-grant");
  BOOST_REQUIRE_MESSAGE(fullGrantResponse.getStatus(), fullGrantResponse.getErrorInfo());
  const auto fullGrant = RepoUserStorageGrantReply::fromJson(responseString(fullGrantResponse)).grant;
  RepoUserStorageInsertRequest fullRequest = request;
  fullRequest.operationId = "owned-kv-insert-over-limit";
  fullRequest.reference.objectName = fullDataName.toUri(ndn::name::UriFormat::ALTERNATE);
  fullRequest.reference.dataPrefix = "/provider/a/kv/full";
  fullRequest.reference.expectedSize = fullWire.size();
  fullRequest.reference.expectedSha256 = sha256(fullWire);
  fullRequest.storageBinding = {1, "/user/full", fullGrant.grantId, fullGrant.ownerEpoch};
  response = dispatch(provider, ndn::Name("/provider/a"), insertService,
    encodeUserStorageInsertRequest(fullRequest), "owned-insert-over-limit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto overLimit = parseOperationStatusJson(responseString(response));
  BOOST_CHECK_EQUAL(overLimit.state, "FAILED");
  BOOST_CHECK_NE(overLimit.message.find("QUOTA_EXCEEDED"), std::string::npos);
  BOOST_CHECK(!node->hasDataPacket(fullRequest.reference.objectName));
  BOOST_CHECK_EQUAL(fetchCount, 4U);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/full", capability.repoNode).committedBytes,
                    REPO_USER_STORAGE_LIMIT_BYTES);

  setGrantExpiry(1);

  response = dispatch(provider, ndn::Name("/provider/a"), insertService,
    encoded, "owned-insert-expired-grant-retry");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto expiredGrantRetry = parseOperationStatusJson(responseString(response));
  BOOST_CHECK_EQUAL(expiredGrantRetry.state, "FAILED");
  BOOST_CHECK_NE(expiredGrantRetry.message.find("repo-user-storage-grant-expired"),
                 std::string::npos);
  BOOST_CHECK_EQUAL(fetchCount, 4U);

  const auto retryExpiry = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count()) + 60000;
  setGrantExpiry(retryExpiry);
  expireGrantDuringFetch.store(true);
  response = dispatch(provider, ndn::Name("/provider/a"), insertService,
    encoded, "owned-insert-grant-expires-during-fetch");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto midFetchExpiry = parseOperationStatusJson(responseString(response));
  BOOST_CHECK_EQUAL(midFetchExpiry.state, "FAILED");
  BOOST_CHECK_NE(midFetchExpiry.message.find("repo-user-storage-grant-expired"),
                 std::string::npos);
  BOOST_CHECK_EQUAL(fetchCount, 5U);
  BOOST_CHECK(node->getDataPacket(reference.objectName) == wire);
}

BOOST_AUTO_TEST_CASE(OwnedInsertRejectsForeignTamperedAndChangedPackets)
{
  TempRoot temp;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-insert-signatures",
                                   "tpm-memory:spec204-insert-signatures");
  const auto producerSigner = makeRsaIdentity(keyChain, ndn::Name("/provider/a"));
  const auto foreignSigner = makeRsaIdentity(keyChain, ndn::Name("/attacker/a"));
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/insert-signature-repo"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/insert-signature-authority"));
  const auto trustAnchors = temp.path() / "trust-anchors";
  std::filesystem::create_directories(trustAnchors);
  const auto saveTrustAnchor = [&] (const ndn::security::Certificate& certificate,
                                    const std::string& filename) {
    const auto path = trustAnchors / filename;
    std::ofstream output(path);
    if (!output) {
      throw std::runtime_error("failed to open Spec204 test trust anchor");
    }
    ndn::io::save(certificate, output);
    output.close();
    if (!output) {
      throw std::runtime_error("failed to write Spec204 test trust anchor");
    }
    const auto loaded = ndn::io::load<ndn::security::Certificate>(path.string());
    if (!loaded || loaded->getName() != certificate.getName()) {
      throw std::runtime_error("failed to reload Spec204 test trust anchor");
    }
  };
  saveTrustAnchor(producerSigner, "producer.ndncert");
  saveTrustAnchor(foreignSigner, "foreign.ndncert");
  const auto trustSchemaPath = temp.path() / "trust-schema.conf";
  std::ofstream trustSchema(trustSchemaPath);
  trustSchema << R"CONF(
rule
{
  id "Spec204 Owned Insert Validation"
  for data
  checker
  {
    type customized
    sig-type rsa-sha256
    key-locator
    {
      type name
      regex ^<>*<KEY><>{1,3}$
    }
  }
}
trust-anchor
{
  type dir
  dir trust-anchors
  refresh 1h
}
)CONF";
  trustSchema.close();
  if (!trustSchema) throw std::runtime_error("failed to write Spec204 test trust schema");
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
    ndn::Name("/spec204/group"), providerCert, authorityCert,
    trustSchemaPath.string());

  StorageCapability capability;
  capability.repoNode = "/repo/owned-insert-signatures";
  capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>(
      (temp.path() / "objects").string(), 4096, 1024, "owned-insert-signatures"),
    capability.repoNode,
    provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), database);
  node->registerServices(provider);

  struct PacketSource final : ndnsf_distributed_repo::detail::DataReferencePacketSource
  {
    PacketSource(std::vector<uint8_t> validated, std::vector<uint8_t> reread)
      : validated(std::move(validated)), reread(std::move(reread)) {}

    std::size_t packetCount() const noexcept override { return 1; }
    uint64_t totalWireBytes() const override { return validated.size(); }
    std::vector<uint8_t> readPacket(std::size_t index) const override
    {
      if (index != 0) throw std::out_of_range("unexpected packet index");
      const auto read = reads.fetch_add(1, std::memory_order_relaxed);
      return read == 0 ? validated : reread;
    }
    std::vector<uint8_t> readRange(uint64_t offset, std::size_t length) const override
    {
      if (offset > validated.size() || length > validated.size() - offset)
        throw std::out_of_range("unexpected packet range");
      const auto begin = validated.begin() + static_cast<std::ptrdiff_t>(offset);
      return {begin, begin + static_cast<std::ptrdiff_t>(length)};
    }
    std::size_t packetReadCount() const
    {
      return reads.load(std::memory_order_relaxed);
    }

    std::vector<uint8_t> validated;
    std::vector<uint8_t> reread;
    mutable std::atomic<std::size_t> reads{0};
  };

  const auto makeWire = [&] (const ndn::Name& name,
                             const std::vector<uint8_t>& content,
                             const ndn::security::Certificate& signer) {
    ndn::Data packet(name);
    packet.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
    keyChain.sign(packet, ndn::security::signingByCertificate(signer));
    const auto block = packet.wireEncode();
    return std::vector<uint8_t>(block.begin(), block.end());
  };

  std::shared_ptr<PacketSource> activeSource;
  node->setDataReferenceSpoolFetcher([&] (const RepoDataReference&) {
    return activeSource;
  });
  const auto grantService = makeRepoServiceName(node->servicePrefix(), "USER_STORAGE_GRANT");
  const auto insertService = makeRepoServiceName(node->servicePrefix(), "INSERT");
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  const auto expiry = now + 60000;
  SqliteArtifactMetadataStore journal(database, true, 16, false);

  const auto runRejected = [&] (const std::string& label,
                                const std::vector<uint8_t>& validated,
                                const std::vector<uint8_t>& reread,
                                const std::string& expectedFailure,
                                std::size_t expectedPacketReads) {
    auto dataPrefix = ndn::Name("/provider/a/kv");
    dataPrefix.append(label);
    auto dataName = dataPrefix;
    dataName.appendSegment(0);

    RepoDataReference reference;
    reference.objectName = dataName.toUri(ndn::name::UriFormat::ALTERNATE);
    reference.dataPrefix = dataPrefix.toUri(ndn::name::UriFormat::ALTERNATE);
    reference.firstSegment = 0;
    reference.finalSegment = 0;
    reference.hasFinalSegment = true;
    reference.expectedSha256 = sha256(validated);
    reference.expectedSize = validated.size();
    reference.storeWirePackets = true;
    activeSource = std::make_shared<PacketSource>(validated, reread);

    const auto grantId = "signature-grant-" + label;
    const auto grantResponse = dispatch(provider, ndn::Name("/user/u"), grantService,
      toBytes(RepoUserStorageGrantRequest{1, grantId, "/provider/a",
        reference.objectName, expiry}.toJson()), "owned-insert-signature-grant-" + label);
    BOOST_REQUIRE_MESSAGE(grantResponse.getStatus(), grantResponse.getErrorInfo());
    const auto grant = RepoUserStorageGrantReply::fromJson(responseString(grantResponse)).grant;

    RepoUserStorageInsertRequest request;
    request.operationId = "owned-insert-signature-" + label;
    request.generation = 1;
    request.reference = reference;
    request.storageBinding = {1, "/user/u", grant.grantId, grant.ownerEpoch};
    const auto response = dispatch(provider, ndn::Name("/provider/a"), insertService,
      encodeUserStorageInsertRequest(request), "owned-insert-signature-call-" + label);
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    const auto status = parseOperationStatusJson(responseString(response));
    BOOST_CHECK_MESSAGE(status.state == "FAILED",
      "case " << label << " finished as " << status.state
              << " with message " << status.message);
    BOOST_CHECK_MESSAGE(status.message.find(expectedFailure) != std::string::npos,
      "case " << label << " expected " << expectedFailure
              << " but got " << status.message);
    BOOST_CHECK_EQUAL(activeSource->packetReadCount(), expectedPacketReads);
    BOOST_CHECK(!node->hasDataPacket(reference.objectName));
    const auto usage = journal.userStorageUsage("/user/u", capability.repoNode);
    BOOST_CHECK_EQUAL(usage.committedBytes, 0U);
    BOOST_CHECK_EQUAL(usage.reservedBytes, 0U);
  };

  auto foreignName = ndn::Name("/provider/a/kv/foreign/seg=0");
  const auto foreignWire = makeWire(foreignName, {1, 4, 9, 16}, foreignSigner);
  runRejected("foreign", foreignWire, foreignWire,
              "repo-original-packet-signature-unauthorized-or-invalid", 1);

  auto tamperedName = ndn::Name("/provider/a/kv/tampered/seg=0");
  auto tamperedWire = makeWire(tamperedName, {1, 4, 9, 16}, producerSigner);
  BOOST_REQUIRE_GT(tamperedWire.size(), 1U);
  tamperedWire.back() ^= 0x01;
  runRejected("tampered", tamperedWire, tamperedWire,
              "repo-original-packet-signature-unauthorized-or-invalid", 1);

  auto changingName = ndn::Name("/provider/a/kv/changed/seg=0");
  const auto originalWire = makeWire(changingName, {1, 4, 9, 16}, producerSigner);
  const auto changedWire = makeWire(changingName, {2, 4, 9, 16}, producerSigner);
  BOOST_REQUIRE_EQUAL(originalWire.size(), changedWire.size());
  runRejected("changed", originalWire, changedWire,
              "repo-original-packet-validation-source-changed", 2);

  auto zeroWorkerName = ndn::Name("/provider/a/kv/zero-worker/seg=0");
  const auto zeroWorkerWire = makeWire(zeroWorkerName, {2, 7, 1, 8}, producerSigner);
  ndn_service_framework::ServiceProviderTestAccess::setLocalMockMode(provider, false);
  runRejected("zero-worker", zeroWorkerWire, zeroWorkerWire,
              "repo-original-packet-signature-unauthorized-or-invalid", 1);
  ndn_service_framework::ServiceProviderTestAccess::setLocalMockMode(provider, true);

  const auto authorizedPrefix = ndn::Name("/provider/a/kv/authorized");
  auto authorizedName = authorizedPrefix;
  authorizedName.appendSegment(0);
  const auto authorizedWire = makeWire(authorizedName, {8, 6, 7, 5}, producerSigner);
  RepoDataReference authorizedReference;
  authorizedReference.objectName = authorizedName.toUri(ndn::name::UriFormat::ALTERNATE);
  authorizedReference.dataPrefix = authorizedPrefix.toUri(ndn::name::UriFormat::ALTERNATE);
  authorizedReference.firstSegment = 0;
  authorizedReference.finalSegment = 0;
  authorizedReference.hasFinalSegment = true;
  authorizedReference.expectedSha256 = sha256(authorizedWire);
  authorizedReference.expectedSize = authorizedWire.size();
  authorizedReference.storeWirePackets = true;
  activeSource = std::make_shared<PacketSource>(authorizedWire, authorizedWire);

  const auto authorizedGrantResponse = dispatch(provider, ndn::Name("/user/u"), grantService,
    toBytes(RepoUserStorageGrantRequest{1, "signature-grant-authorized", "/provider/a",
      authorizedReference.objectName, expiry}.toJson()), "owned-insert-signature-grant-authorized");
  BOOST_REQUIRE_MESSAGE(authorizedGrantResponse.getStatus(), authorizedGrantResponse.getErrorInfo());
  const auto authorizedGrant =
    RepoUserStorageGrantReply::fromJson(responseString(authorizedGrantResponse)).grant;

  RepoUserStorageInsertRequest authorizedRequest;
  authorizedRequest.operationId = "owned-insert-signature-authorized";
  authorizedRequest.generation = 1;
  authorizedRequest.reference = authorizedReference;
  authorizedRequest.storageBinding = {1, "/user/u", authorizedGrant.grantId,
                                      authorizedGrant.ownerEpoch};
  const auto authorizedResponse = dispatch(provider, ndn::Name("/provider/a"), insertService,
    encodeUserStorageInsertRequest(authorizedRequest), "owned-insert-signature-call-authorized");
  BOOST_REQUIRE_MESSAGE(authorizedResponse.getStatus(), authorizedResponse.getErrorInfo());
  const auto authorizedStatus = parseOperationStatusJson(responseString(authorizedResponse));
  BOOST_REQUIRE_EQUAL(authorizedStatus.state, "DONE");
  BOOST_CHECK_EQUAL(activeSource->packetReadCount(), 2U);
  BOOST_CHECK(node->getDataPacket(authorizedReference.objectName) == authorizedWire);
  const auto authorizedUsage = journal.userStorageUsage("/user/u", capability.repoNode);
  BOOST_CHECK_GT(authorizedUsage.committedBytes, 0U);
  BOOST_CHECK_EQUAL(authorizedUsage.reservedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(OwnedInsertValidationStopsAndDrainsLateCallbacks)
{
  TempRoot temp;
  auto face = std::make_unique<ndn::DummyClientFace>();
  ndn::security::KeyChain keyChain("pib-memory:spec204-insert-validation-lifecycle",
                                   "tpm-memory:spec204-insert-validation-lifecycle");
  const auto producerSigner = makeRsaIdentity(keyChain, ndn::Name("/provider/a"));
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/lifecycle-repo"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/lifecycle-authority"));
  const auto trustAnchors = temp.path() / "trust-anchors";
  std::filesystem::create_directories(trustAnchors);
  const auto anchorPath = trustAnchors / "producer.ndncert";
  {
    std::ofstream output(anchorPath);
    if (!output) throw std::runtime_error("failed to open Spec204 lifecycle trust anchor");
    ndn::io::save(producerSigner, output);
    output.close();
    if (!output) throw std::runtime_error("failed to write Spec204 lifecycle trust anchor");
    const auto loaded = ndn::io::load<ndn::security::Certificate>(anchorPath.string());
    if (!loaded || loaded->getName() != producerSigner.getName()) {
      throw std::runtime_error("failed to reload Spec204 lifecycle trust anchor");
    }
  }
  const auto trustSchemaPath = temp.path() / "trust-schema.conf";
  {
    std::ofstream trustSchema(trustSchemaPath);
    trustSchema << R"CONF(
rule
{
  id "Spec204 Owned Insert Callback Lifetime"
  for data
  checker
  {
    type customized
    sig-type rsa-sha256
    key-locator
    {
      type name
      regex ^<>*<KEY><>{1,3}$
    }
  }
}
trust-anchor
{
  type dir
  dir trust-anchors
  refresh 1h
}
)CONF";
    trustSchema.close();
    if (!trustSchema) throw std::runtime_error("failed to write Spec204 lifecycle trust schema");
  }

  auto provider = std::make_unique<ndn_service_framework::ServiceProvider>(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, *face,
    ndn::Name("/spec204/group"), providerCert, authorityCert,
    trustSchemaPath.string());
  ndn_service_framework::ServiceProviderTestAccess::configureFaceBackedProducerValidator(
    *provider, trustSchemaPath.string(), ndn::Name("/spec204/group"), *face);
  provider->setHandlerThreads(1);
  const auto observeWaiters =
    ndn_service_framework::ServiceProviderTestAccess::observeProducerValidationWaiters(*provider);

  ndn::Data packet(ndn::Name("/provider/a/kv/callback/seg=0"));
  const std::vector<uint8_t> content{2, 7, 1, 8};
  packet.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
  keyChain.sign(packet, ndn::security::signingByCertificate(producerSigner));
  const auto block = packet.wireEncode();
  const std::vector<uint8_t> wire(block.begin(), block.end());
  const auto producer = ndn::Name("/provider/a");
  const auto validationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

  std::vector<std::future<bool>> waiters;
  waiters.reserve(16);
  for (std::size_t i = 0; i < 16; ++i) {
    waiters.emplace_back(std::async(std::launch::async, [&] {
      return provider->validateProducerPacketWireUntil(wire, producer, validationDeadline);
    }));
  }

  const auto admissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (observeWaiters() != 16 && std::chrono::steady_clock::now() < admissionDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  BOOST_REQUIRE_EQUAL(observeWaiters(), 16U);
  for (const auto& waiter : waiters) {
    BOOST_CHECK(waiter.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  }

  const auto overflowAccepted = provider->validateProducerPacketWireUntil(
    wire, producer, std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
  BOOST_CHECK(!overflowAccepted);
  BOOST_CHECK_EQUAL(observeWaiters(), 16U);

  provider->stopProducerPacketValidation();
  const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool allWaitersReady = true;
  for (auto& waiter : waiters) {
    allWaitersReady = waiter.wait_until(stopDeadline) == std::future_status::ready && allWaitersReady;
  }
  BOOST_REQUIRE(allWaitersReady);
  for (auto& waiter : waiters) {
    BOOST_CHECK(!waiter.get());
  }
  BOOST_CHECK_EQUAL(observeWaiters(), 16U);

  provider.reset();
  BOOST_CHECK_EQUAL(observeWaiters(), 16U);
  face->getIoContext().restart();
  (void)face->getIoContext().poll();
  const auto callbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (observeWaiters() != 0 && std::chrono::steady_clock::now() < callbackDeadline) {
    (void)face->getIoContext().poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  face.reset();
  BOOST_CHECK_EQUAL(observeWaiters(), 0U);
}

BOOST_AUTO_TEST_CASE(OwnedInsertShutdownClosesAdmissionBeforeFetch)
{
  TempRoot temp;
  ndn::DummyClientFace face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-insert-shutdown",
                                   "tpm-memory:spec204-insert-shutdown");
  const auto producerSigner = makeRsaIdentity(keyChain, ndn::Name("/provider/a"));
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/shutdown-repo"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec204/shutdown-authority"));
  const auto trustAnchors = temp.path() / "trust-anchors";
  std::filesystem::create_directories(trustAnchors);
  const auto anchorPath = trustAnchors / "producer.ndncert";
  {
    std::ofstream output(anchorPath);
    if (!output) throw std::runtime_error("failed to open Spec204 shutdown trust anchor");
    ndn::io::save(producerSigner, output);
    output.close();
    if (!output) throw std::runtime_error("failed to write Spec204 shutdown trust anchor");
  }
  const auto trustSchemaPath = temp.path() / "trust-schema.conf";
  {
    std::ofstream trustSchema(trustSchemaPath);
    trustSchema << R"CONF(
rule
{
  id "Spec204 Owned Insert Shutdown"
  for data
  checker
  {
    type customized
    sig-type rsa-sha256
    key-locator
    {
      type name
      regex ^<>*<KEY><>{1,3}$
    }
  }
}
trust-anchor
{
  type dir
  dir trust-anchors
  refresh 1h
}
)CONF";
    trustSchema.close();
    if (!trustSchema) throw std::runtime_error("failed to write Spec204 shutdown trust schema");
  }
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
    ndn::Name("/spec204/group"), providerCert, authorityCert,
    trustSchemaPath.string());

  StorageCapability capability;
  capability.repoNode = "/repo/owned-insert-shutdown";
  capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>(
      (temp.path() / "objects").string(), 4096, 1024, "owned-insert-shutdown"),
    capability.repoNode, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), database);
  node->registerServices(provider);

  const auto dataPrefix = ndn::Name("/provider/a/kv/shutdown");
  auto objectName = dataPrefix;
  objectName.appendSegment(0);
  const std::vector<uint8_t> expectedWire{1};
  RepoUserStorageInsertRequest request;
  request.operationId = "owned-insert-shutdown-admission";
  request.generation = 1;
  request.reference.objectName = objectName.toUri(ndn::name::UriFormat::ALTERNATE);
  request.reference.dataPrefix = dataPrefix.toUri(ndn::name::UriFormat::ALTERNATE);
  request.reference.firstSegment = 0;
  request.reference.finalSegment = 0;
  request.reference.hasFinalSegment = true;
  request.reference.expectedSha256 = sha256(expectedWire);
  request.reference.expectedSize = expectedWire.size();
  request.reference.storeWirePackets = true;
  request.storageBinding = {1, "/user/u", "shutdown-grant-unused", 1};

  std::atomic<std::size_t> fetchCount{0};
  node->setDataReferenceFetcher([&] (const RepoDataReference&) {
    fetchCount.fetch_add(1, std::memory_order_relaxed);
    return std::vector<std::vector<uint8_t>>{expectedWire};
  });
  node->beginShutdown();
  const auto insertService = makeRepoServiceName(node->servicePrefix(), "INSERT");
  const auto response = dispatch(provider, ndn::Name("/provider/a"), insertService,
    encodeUserStorageInsertRequest(request), "owned-insert-shutdown-admission-call");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto status = parseOperationStatusJson(responseString(response));
  BOOST_CHECK_EQUAL(status.state, "FAILED");
  BOOST_CHECK(status.message.find("repo-original-packet-insert-shutting-down") !=
              std::string::npos);
  BOOST_CHECK_EQUAL(fetchCount.load(std::memory_order_relaxed), 0U);
  BOOST_CHECK(!node->hasDataPacket(request.reference.objectName));
  SqliteArtifactMetadataStore journal(database, true, 16, false);
  const auto usage = journal.userStorageUsage("/user/u", capability.repoNode);
  BOOST_CHECK_EQUAL(usage.committedBytes, 0U);
  BOOST_CHECK_EQUAL(usage.reservedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(OwnedInsertRequiresExactSegmentRange)
{
  TempRoot temp;
  ndn::security::KeyChain keyChain("pib-memory:spec204-segment-range",
                                   "tpm-memory:spec204-segment-range");
  (void)makeRsaIdentity(keyChain, ndn::Name("/provider/a"));

  StorageCapability capability;
  capability.repoNode = "/repo/owned-insert-ranges";
  capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  auto node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>(
      (temp.path() / "objects").string(), 4096, 1024, "owned-insert-ranges"),
    capability.repoNode, "/repo/provider",
    (temp.path() / "metadata.sqlite3").string());

  const auto makePacketWire = [&] (const ndn::Name& name) {
    ndn::Data packet(name);
    const std::vector<uint8_t> content{3, 1, 4, 1, 5};
    packet.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
    keyChain.sign(packet);
    const auto block = packet.wireEncode();
    return std::vector<uint8_t>(block.begin(), block.end());
  };

  const auto checkRejectedRange = [&] (const std::string& label,
                                       uint64_t firstSegment,
                                       uint64_t finalSegment,
                                       const std::vector<uint64_t>& returnedSegments,
                                       const std::string& expectedFailure) {
    ndn::Name prefix("/provider/a/kv");
    prefix.append(label);
    std::vector<std::vector<uint8_t>> wires;
    std::vector<uint8_t> packetSetWire;
    for (const auto segment : returnedSegments) {
      auto packetName = prefix;
      packetName.appendSegment(segment);
      auto wire = makePacketWire(packetName);
      packetSetWire.insert(packetSetWire.end(), wire.begin(), wire.end());
      wires.push_back(std::move(wire));
    }
    BOOST_REQUIRE(!wires.empty());

    RepoUserStorageInsertRequest request;
    request.operationId = "owned-range-" + label;
    request.generation = 1;
    request.reference.objectName = prefix.toUri(ndn::name::UriFormat::ALTERNATE) +
      "/seg=" + std::to_string(returnedSegments.front());
    request.reference.dataPrefix = prefix.toUri(ndn::name::UriFormat::ALTERNATE);
    request.reference.firstSegment = firstSegment;
    request.reference.finalSegment = finalSegment;
    request.reference.hasFinalSegment = true;
    request.reference.expectedSha256 = sha256(packetSetWire);
    request.reference.expectedSize = packetSetWire.size();
    request.storageBinding = {1, "/user/u", "unused-grant", 1};

    const auto status = node->insertWirePackets(request, wires, "/provider/a");
    BOOST_CHECK_EQUAL(status.state, "FAILED");
    BOOST_CHECK(status.message.find(expectedFailure) != std::string::npos);
    for (const auto& wire : wires) {
      const ndn::Block block(ndn::span<const uint8_t>(wire.data(), wire.size()));
      const ndn::Data packet(block);
      BOOST_CHECK(!node->hasDataPacket(
        packet.getName().toUri(ndn::name::UriFormat::ALTERNATE)));
    }
  };

  checkRejectedRange("missing", 0, 1, {0}, "packet count does not match");
  checkRejectedRange("extra", 0, 0, {0, 1}, "packet count does not match");
  checkRejectedRange("wrong-first", 0, 1, {1, 2}, "requested segment order");
  checkRejectedRange("gap", 0, 1, {0, 2}, "requested segment order");
}

BOOST_AUTO_TEST_CASE(ConcurrentReferenceInsertsSerializeLargeFetchSpools)
{
  TempRoot temp;
  ndn::security::KeyChain keyChain("pib-memory:spec204-fetch-admission",
                                   "tpm-memory:spec204-fetch-admission");
  (void)makeRsaIdentity(keyChain, ndn::Name("/provider/a"));

  StorageCapability capability;
  capability.repoNode = "/repo/fetch-admission";
  capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  RepoNode node(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>(
      (temp.path() / "objects").string(), 4096, 1024, "fetch-admission"),
    capability.repoNode, "/repo/provider");

  const auto makePacket = [&] (const ndn::Name& name) {
    ndn::Data packet(name);
    const std::vector<uint8_t> content{2, 7, 1, 8, 2, 8};
    packet.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
    keyChain.sign(packet);
    const auto block = packet.wireEncode();
    return std::vector<uint8_t>(block.begin(), block.end());
  };
  ndn::Name firstName("/provider/a/kv/concurrent/first");
  firstName.appendSegment(0);
  ndn::Name secondName("/provider/a/kv/concurrent/second");
  secondName.appendSegment(0);
  const auto firstWire = makePacket(firstName);
  const auto secondWire = makePacket(secondName);

  const auto makeReference = [] (const ndn::Name& packetName,
                                 const std::vector<uint8_t>& wire) {
    RepoDataReference reference;
    reference.objectName = packetName.toUri(ndn::name::UriFormat::CANONICAL);
    reference.dataPrefix = packetName.getPrefix(-1).toUri(ndn::name::UriFormat::CANONICAL);
    reference.hasFinalSegment = true;
    reference.expectedSha256 = sha256(wire);
    reference.expectedSize = wire.size();
    reference.storeWirePackets = true;
    return reference;
  };
  const auto firstReference = makeReference(firstName, firstWire);
  const auto secondReference = makeReference(secondName, secondWire);

  std::atomic<unsigned> activeFetches{0};
  std::atomic<unsigned> peakFetches{0};
  std::atomic<unsigned> callbackCount{0};
  std::mutex overlapMutex;
  std::condition_variable overlapCondition;
  bool overlapObserved = false;
  node.setDataReferenceFetcher([&] (const RepoDataReference& reference) {
    const auto callback = callbackCount.fetch_add(1);
    const auto active = activeFetches.fetch_add(1) + 1;
    auto peak = peakFetches.load();
    while (active > peak && !peakFetches.compare_exchange_weak(peak, active)) {
    }
    if (active > 1) {
      {
        std::lock_guard<std::mutex> lock(overlapMutex);
        overlapObserved = true;
      }
      overlapCondition.notify_all();
    }
    if (callback == 0) {
      std::unique_lock<std::mutex> lock(overlapMutex);
      (void)overlapCondition.wait_for(lock, std::chrono::milliseconds(250), [&] {
        return overlapObserved;
      });
    }
    activeFetches.fetch_sub(1);
    if (reference.objectName == firstReference.objectName) return std::vector<std::vector<uint8_t>>{firstWire};
    if (reference.objectName == secondReference.objectName) return std::vector<std::vector<uint8_t>>{secondWire};
    throw std::runtime_error("fixture received unexpected object name");
  });

  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false};
  std::vector<uint8_t> firstReply;
  std::vector<uint8_t> secondReply;
  std::thread firstThread([&] {
    ready.fetch_add(1);
    while (!start.load()) std::this_thread::yield();
    firstReply = node.handleInsert(toBytes(firstReference.toJson()));
  });
  std::thread secondThread([&] {
    ready.fetch_add(1);
    while (!start.load()) std::this_thread::yield();
    secondReply = node.handleInsert(toBytes(secondReference.toJson()));
  });
  while (ready.load() != 2) std::this_thread::yield();
  start.store(true);
  firstThread.join();
  secondThread.join();

  BOOST_CHECK_EQUAL(parseOperationStatusJson(toString(firstReply)).state, "DONE");
  BOOST_CHECK_EQUAL(parseOperationStatusJson(toString(secondReply)).state, "DONE");
  BOOST_CHECK_EQUAL(peakFetches.load(), 1U);
  BOOST_CHECK(node.hasDataPacket(firstReference.objectName));
  BOOST_CHECK(node.hasDataPacket(secondReference.objectName));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::tests
