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
#include "tests/fixtures/fixed-keychain-trust.hpp"
#include "NDNSF-DistributedRepo/src/face-data-reference-fetcher.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <ndn-cxx/util/io.hpp>
#include <openssl/sha.h>
#include <sqlite3.h>

#include <boost/test/unit_test.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <utility>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/stat.h>

namespace ndn_service_framework {

struct ServiceProviderTestAccess
{
  static void configureFaceBackedProducerValidator(ServiceProvider& provider,
    const std::string& trustSchemaPath, ndn::Face& face)
  {
    provider.validator = std::make_shared<MessageValidator>(trustSchemaPath, std::nullopt, &face);
    provider.m_isLocalMock = false;
  }

  static std::function<std::size_t()> observeProducerValidationWaiters(ServiceProvider& provider)
  {
    const auto control = provider.validator->m_producerPacketValidationControl;
    return [control] {
      std::lock_guard<std::mutex> lock(control->mutex);
      return control->outstanding;
    };
  }

  static bool knowsCertificate(ServiceProvider& provider, const ndn::Name& name)
  {
    return provider.validator->getConfiguredValidatorForSegmentFetcher().isCertKnown(name);
  }

  static std::function<size_t()> observeRecipientDecrypts(ServiceProvider& provider)
  {
    const auto pending = provider.m_pendingContextCrypto;
    return [pending] { return pending->load(); };
  }

  static void checkCertificateTrust(ServiceProvider& provider, const ndn::Data& data)
  {
    provider.validator->validateWithConfiguredTrustSchema(data,
      [](const ndn::Data&) { BOOST_TEST_MESSAGE("certificate schema accepted"); },
      [](const ndn::Data&, const ndn::security::ValidationError& error) {
        BOOST_ERROR("certificate schema rejected: " << error);
      });
  }
};

} // namespace ndn_service_framework

namespace ndn_service_framework::test {

struct ProducerPacketValidationOracle
{
  static void configure(ServiceUser& user, const std::string& schema, ndn::Face& face)
  {
    user.validator = std::make_shared<MessageValidator>(schema, std::nullopt, &face);
  }

  static std::function<std::size_t()> pending(const ServiceUser& user)
  {
    const auto control = user.validator->m_producerPacketValidationControl;
    return [control] {
      std::lock_guard<std::mutex> lock(control->mutex);
      return control->outstanding;
    };
  }

  static bool knowsCertificate(const ServiceUser& user, const ndn::Name& name)
  {
    return user.validator->getConfiguredValidatorForSegmentFetcher().isCertKnown(name);
  }
};

} // namespace ndn_service_framework::test

namespace ndnsf_distributed_repo {

struct RepoCorePacketIndexTestAccess
{
  static void validate(RepoCore& core, const RepoObjectManifest& manifest)
  {
    core.validateReadPacketIndex(manifest);
  }

  static std::pair<size_t, size_t> measure(const RepoCore& core)
  {
    std::lock_guard<std::mutex> lock(core.m_packetIndexProofMutex);
    return {core.m_packetIndexProofs.size(), core.m_packetIndexProofBytes};
  }
};

struct RepoNodeTestAccess
{
  static std::function<std::size_t()>
  pending(const RepoNode& node)
  {
    const auto count = node.m_originalDataPending;
    return [count] { return count->load(); };
  }

  static std::unique_lock<std::mutex>
  holdWrites(RepoNode& node)
  {
    return std::unique_lock<std::mutex>(node.m_storeWriteMutex);
  }

  static void
  enqueue(RepoNode& node, const ndn::Interest& interest)
  {
    node.enqueueOriginalDataInterest(interest);
  }
};

} // namespace ndnsf_distributed_repo

namespace ndnsf_distributed_repo::tests {

namespace {

int
rejectDirectoryFsync(int fd)
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

struct OriginalPacketSource
{
  RepoObjectManifest manifest;
  std::vector<uint8_t> wire;

  explicit OriginalPacketSource(std::size_t packetCount = 2)
  {
    ndn::KeyChain keys("pib-memory:spec221-reader", "tpm-memory:spec221-reader");
    manifest.objectName = ndn::Name("/provider/a/kv/reader").appendSegment(0)
      .toUri(ndn::name::UriFormat::CANONICAL);
    manifest.dataPrefix = ndn::Name("/provider/a/kv/reader")
      .toUri(ndn::name::UriFormat::CANONICAL);
    manifest.objectType = "original-packet-set";
    manifest.generation = 1;
    manifest.operationId = "reader-operation";
    manifest.packetOffsets = {0};
    for (std::size_t i = 0; i < packetCount; ++i) {
      ndn::Data data(ndn::Name("/provider/a/kv/reader").appendSegment(i));
      const std::vector<uint8_t> content(6000, static_cast<uint8_t>(i));
      data.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
      // This fixture proves storage integrity; authentication is tested separately.
      keys.sign(data, ndn::security::signingWithSha256());
      const auto encoded = data.wireEncode();
      wire.insert(wire.end(), encoded.begin(), encoded.end());
      manifest.packetNames.push_back(data.getName().toUri(ndn::name::UriFormat::CANONICAL));
      manifest.packetOffsets.push_back(wire.size());
    }
    manifest.segmentCount = static_cast<uint32_t>(packetCount);
    manifest.size = wire.size();
    manifest.sha256 = sha256(wire);
  }

  std::vector<uint8_t>
  read(uint64_t offset, std::size_t length) const
  {
    if (offset > wire.size() || length > wire.size() - offset) {
      throw std::out_of_range("fixture range exceeds original wire");
    }
    return {wire.begin() + static_cast<std::ptrdiff_t>(offset),
            wire.begin() + static_cast<std::ptrdiff_t>(offset + length)};
  }
};

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
    writeFixedKeychainTrust(temp.path(), keyChain));

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
  intent.requestDigest = makePublishRequestDigest(
    reference, 1, seed, intent.consistency, intent.targets);
  const auto operationId = makePublishOperationId(
    callerName.toUri(ndn::name::UriFormat::ALTERNATE), seed, intent.idempotencyKeyDigest);
  const auto storeService = makeRepoServiceName(servicePrefix, "STORE");
  const auto statusService = makeRepoServiceName(servicePrefix, "STATUS");
  ndnsf_distributed_repo::RepoArtifactCommitReceipt seedReceiptBeforeRestart;
  {
    ndn_service_framework::ServiceProvider provider(
      ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
      ndn::Name("/spec192/group-restart"), providerCert, authorityCert,
    writeFixedKeychainTrust(temp.path(), keyChain));
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
    writeFixedKeychainTrust(temp.path(), keyChain));
    publisherUser->useSigningKeyChainForSigningOnlyForTest(keyChain);
    ndn_service_framework::ServiceProvider provider(
      ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
      ndn::Name("/spec192/group-restart"), providerCert, authorityCert,
    writeFixedKeychainTrust(temp.path(), keyChain));
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
    writeFixedKeychainTrust(temp.path(), keyChain));

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

BOOST_AUTO_TEST_CASE(DirectRemovalConfirmationRetainsRestartUnknownStages)
{
  for (const auto generation : {1U, 2U}) {
    TempRoot temporary;
    const auto root = temporary.path() / "objects";
    const auto digest = sha256({1, 2, 3});
    const auto stage = root / "staging" /
      (digest + "." + std::to_string(generation) + ".part");
    const std::vector<std::filesystem::path> files{
      stage, stage.string() + ".ranges", stage.string() + ".finalize-intent"};
    {
      FilesystemRepoStoreBackend initial(root.string(), 4096, 1024, "unknown-confirm");
      for (const auto& file : files) {
        std::ofstream output(file);
        output << "unresolved";
        BOOST_REQUIRE(output.good());
      }
      std::ofstream uncertain(root / "manifests" / "unknown.json");
      uncertain << "unresolved";
      BOOST_REQUIRE(uncertain.good());
    }
    auto backend = std::make_shared<FilesystemRepoStoreBackend>(
      root.string(), 4096, 1024, "unknown-confirm");
    // Resolve the unrelated fixture row; recovered stages still have no owner.
    std::filesystem::remove(root / "manifests" / "unknown.json");
    StorageCapability capability;
    capability.repoNode = "/repo/unknown-confirm";
    capability.freeBytes = 4096;
    RepoCore core(capability, backend);
    const RepoUserStorageObject expected{"/producer/removed", digest, 1, 3, 0};
    BOOST_CHECK(!backend->confirmUserStorageRemoval(expected));
    BOOST_CHECK(!core.confirmUserStorageRemoval(expected));
    for (const auto& file : files) {
      std::ifstream input(file);
      const std::string bytes{std::istreambuf_iterator<char>(input), {}};
      BOOST_CHECK_EQUAL(bytes, "unresolved");
    }
  }
}

BOOST_AUTO_TEST_CASE(DirectConfirmationPreservesOtherGenerationAndCoreReservations)
{
  TempRoot temporary;
  const auto root = temporary.path() / "objects";
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(
    root.string(), 4096, 1024, "active-confirm");
  const std::vector<uint8_t> bytes{1, 2, 3};
  const auto digest = sha256(bytes);
  const ArtifactReference reference{"/artifact/orphan", "sha256", digest, bytes.size(),
    "artifact-manifest-v2", "/producer/manifest/orphan", "/producer", "epoch"};
  FilesystemArtifactPayloadStore payload(root.string(), 4096);
  payload.begin(reference, 1);
  payload.writeRange(reference, 1, {0, bytes.size()}, bytes);
  payload.markVerified(reference, 1, {0, bytes.size()});
  payload.flush(reference, 1);
  RepoObjectManifest alias;
  alias.objectName = "/producer/alias";
  alias.sha256 = digest;
  alias.size = bytes.size();
  alias.generation = 2;
  alias.operationId = "alias";
  backend->putRange(alias, {0, 2}, {1, 2});
  const auto aliasStage = root / "staging" / (digest + ".2.part");
  BOOST_REQUIRE(std::filesystem::exists(aliasStage));
  BOOST_REQUIRE(std::filesystem::exists(aliasStage.string() + ".ranges"));
  payload.finalize(reference, 1);
  StorageCapability capability;
  capability.repoNode = "/repo/active-confirm";
  capability.freeBytes = 4096;
  RepoCore core(capability, backend);
  const RepoUserStorageObject expected{"/producer/removed", digest, 1, bytes.size(), 0};
  BOOST_CHECK(!backend->confirmUserStorageRemoval(expected));
  BOOST_CHECK(!core.confirmUserStorageRemoval(expected));
  BOOST_CHECK(payload.readRange(reference, 1, {0, bytes.size()}) == bytes);
  BOOST_REQUIRE(backend->abortUserStorageRangesIfOwned(alias));
  core.putRange(alias, {0, 2}, {1, 2});
  backend->abortRanges(alias.objectName); // Physical abort alone does not retire Core ownership.
  BOOST_REQUIRE(!std::filesystem::exists(aliasStage));
  BOOST_REQUIRE(backend->canRemoveUserStorageObject(expected));
  BOOST_CHECK(!core.confirmUserStorageRemoval(expected));
  BOOST_CHECK(payload.readRange(reference, 1, {0, bytes.size()}) == bytes);
  BOOST_CHECK(core.abortUserStorageRangesIfOwned(alias));
  BOOST_CHECK(core.confirmUserStorageRemoval(expected));
  BOOST_CHECK(!std::filesystem::exists(payload.committedPath(reference)));
}

BOOST_AUTO_TEST_CASE(CheckedManifestParserKeepsSignedParentAndUnsignedOwnershipSeparate)
{
  for (const auto& wire : {std::string("-1"), std::string("-9223372036854775808")}) {
    const auto value = detail::parseJson(wire, 4096);
    BOOST_CHECK(value.kind == detail::JsonValue::Kind::Signed);
    BOOST_CHECK_EQUAL(detail::renderJson(value), wire);
  }
  const auto maximum = detail::parseJson("18446744073709551615", 4096);
  BOOST_CHECK(maximum.kind == detail::JsonValue::Kind::Unsigned);
  BOOST_CHECK_EQUAL(maximum.number, std::numeric_limits<uint64_t>::max());
  for (const auto& wire : {"-9223372036854775809", "-", "--1", "-01", "-1.5", "-1e2"}) {
    BOOST_CHECK_THROW(detail::parseJson(wire, 4096), std::invalid_argument);
  }
  BOOST_CHECK_THROW(RepoUserStorageBinding::fromJson(
    "{\"schemaVersion\":1,\"ownerIdentity\":\"/owner\",\"grantId\":\"g\",\"ownerEpoch\":-1}"),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OwnerMetadataMeasurementUsesActualFileBytes)
{
  TempRoot temporary;
  const auto root = temporary.path() / "objects";
  FilesystemRepoStoreBackend backend(root.string(), 4096, 1024, "measurement");
  const std::vector<uint8_t> bytes{1, 2, 3};
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/measured";
  manifest.sha256 = sha256(bytes);
  manifest.size = bytes.size();
  manifest.generation = 1;
  backend.put(manifest, bytes);
  const auto metadata = std::filesystem::directory_iterator(root / "manifests")->path();
  const auto first = backend.probeObjectMetadata(manifest.objectName);
  BOOST_REQUIRE(first.manifestMeasurementAvailable);
  BOOST_CHECK_EQUAL(first.manifestWireBytes, std::filesystem::file_size(metadata));
  {
    std::ofstream output(metadata, std::ios::app | std::ios::binary);
    output << '\n';
    BOOST_REQUIRE(output.good());
  }
  const auto second = backend.probeObjectMetadata(manifest.objectName);
  BOOST_REQUIRE(second.manifestMeasurementAvailable);
  BOOST_CHECK_EQUAL(second.manifestWireBytes, first.manifestWireBytes + 1);
  BOOST_CHECK(second.manifest.hasSameMetadata(first.manifest));
  BOOST_CHECK(backend.get(manifest.objectName).payload == bytes);
}

BOOST_AUTO_TEST_CASE(SqliteMetadataMeasurementPreservesEmptyPayloadRepresentation)
{
  TempRoot temporary;
  const auto backend = makeSqliteRepoStore((temporary.path() / "objects.sqlite3").string());
  RepoObjectManifest manifest;
  manifest.objectName = "/producer/empty";
  manifest.sha256 = sha256({});
  manifest.generation = 1;
  backend->put(manifest, {});
  const auto probe = backend->probeObjectMetadata(manifest.objectName);
  BOOST_REQUIRE(probe.manifestPresent);
  BOOST_REQUIRE(probe.manifestMeasurementAvailable);
  BOOST_CHECK_EQUAL(probe.manifestWireBytes, manifest.toJson().size());
  BOOST_CHECK(probe.payloadPresent);
  BOOST_CHECK(probe.payloadRepresentationValid);
  BOOST_CHECK_EQUAL(probe.payloadSize, 0);
}

BOOST_AUTO_TEST_CASE(OwnerServicesUseAuthenticatedRequesterAndPersistExactGrants)
{
  TempRoot temp;
  ndn::Face face;
  ndn::KeyChain keys("pib-memory:spec221-owner-services", "tpm-memory:spec221-owner-services");
  const auto providerCertificate = makeRsaIdentity(keys, ndn::Name("/repo/provider"));
  const auto authorityCertificate = makeRsaIdentity(keys, ndn::Name("/repo/authority"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face, ndn::Name("/group"),
    providerCertificate, authorityCertificate, writeFixedKeychainTrust(temp.path(), keys));
  StorageCapability capability;
  capability.repoNode = "/repo/a";
  capability.repoMode = "persistent";
  capability.freeBytes = 1024 * 1024;
  const auto database = (temp.path() / "metadata.sqlite3").string();
  auto node = std::make_shared<RepoNode>(ndn::Name("/SERVICE/repo"), capability,
    makeSqliteRepoStore((temp.path() / "payload.sqlite3").string()),
    "/repo/a", "/repo/provider", database);
  node->registerServices(provider);
  const auto invoke = [&](const std::string& operation, const std::string& owner,
                          const std::string& json) {
    return dispatch(provider, ndn::Name(owner),
      makeRepoServiceName(node->servicePrefix(), operation),
      std::vector<uint8_t>(json.begin(), json.end()), operation);
  };
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  RepoUserStorageGrantRequest grant{1, "grant-a", "/provider/a",
    "/provider/a/kv/checkpoint", now + 60000};
  auto response = invoke("USER_STORAGE_GRANT", "/owner/a", grant.toJson());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto accepted = RepoUserStorageGrantReply::fromJson(responseString(response));
  const auto admittedAt = accepted.grant.createdAtMs;
  BOOST_CHECK_EQUAL(accepted.grant.ownerIdentity, "/owner/a");
  BOOST_CHECK_EQUAL(accepted.grant.writerIdentity, "/provider/a");
  BOOST_CHECK_EQUAL(accepted.grant.objectName, grant.objectName);
  BOOST_CHECK_EQUAL(accepted.grant.repoNode, "/repo/a");

  // A second authenticated principal cannot consume the first Owner's grant.
  SqliteArtifactMetadataStore journal(database);
  BOOST_CHECK_THROW(journal.validateUserStorageGrant("/provider/a", "/repo/a",
    "/owner/b", grant.grantId, grant.objectName, accepted.grant.ownerEpoch, admittedAt),
    std::exception);
  const RepoUserStorageObject object{grant.objectName, std::string(64, 'a'), 1, 99, 1};
  journal.reconcileUserStorageInventory("/repo/a", {}, admittedAt);
  journal.reserveUserStorage("/provider/a", "/repo/a", "/owner/a", grant.grantId,
    "owner-service-fixture", object, admittedAt);
  journal.commitUserStorage("/repo/a", "/owner/a",
    "owner-service-fixture", admittedAt);

  response = invoke("USER_STORAGE_USAGE", "/owner/a", RepoUserStorageUsageRequest{}.toJson());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto usage = RepoUserStorageUsageReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(usage.usage.committedBytes, 100U);
  response = invoke("USER_STORAGE_USAGE", "/owner/b", RepoUserStorageUsageRequest{}.toJson());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(RepoUserStorageUsageReply::fromJson(responseString(response))
    .usage.committedBytes, 0U);
  response = invoke("USER_STORAGE_LIST", "/owner/a", RepoUserStorageListRequest{}.toJson());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto listed = RepoUserStorageListReply::fromJson(responseString(response));
  BOOST_REQUIRE_EQUAL(listed.objects.size(), 1U);
  BOOST_CHECK_EQUAL(listed.objects.front().ownerIdentity, "/owner/a");
  response = invoke("USER_STORAGE_LIST", "/owner/b", RepoUserStorageListRequest{}.toJson());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK(RepoUserStorageListReply::fromJson(responseString(response)).objects.empty());

  // The service must not interpret payload text as an authenticated Owner.
  response = invoke("USER_STORAGE_USAGE", "/owner/b", "{\"schemaVersion\":1,"
    "\"ownerIdentity\":\"/owner/a\"}");
  BOOST_CHECK(!response.getStatus());
  grant.grantId = "expired";
  grant.expiresAtMs = now - 1;
  response = invoke("USER_STORAGE_GRANT", "/owner/a", grant.toJson());
  BOOST_CHECK(!response.getStatus());
  response = invoke("USER_STORAGE_GRANT", "/owner/a", "{}");
  BOOST_CHECK(!response.getStatus());

  SqliteArtifactMetadataStore reopened(database);
  const auto persisted = reopened.validateUserStorageGrant("/provider/a", "/repo/a",
    "/owner/a", "grant-a", object.objectName, accepted.grant.ownerEpoch, admittedAt);
  BOOST_CHECK_EQUAL(persisted.writerIdentity, "/provider/a");
  BOOST_CHECK_EQUAL(persisted.ownerEpoch, accepted.grant.ownerEpoch);
  // This fixture seeds metadata only; it proves no physical packet durability.
}

struct RecipientConsumerFixture
{
  using Provider = ndn_service_framework::ServiceProvider;
  TempRoot temp;
  ndn::KeyChain keys{"pib-memory:spec221-consumer", "tpm-memory:spec221-consumer"};
  ndn::DummyClientFace face{keys};
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> ioWork =
    boost::asio::make_work_guard(face.getIoContext());
  ndn::security::Certificate recipient = makeRsaIdentity(keys, ndn::Name("/provider/recipient"));
  ndn::security::Certificate authority = makeRsaIdentity(keys, ndn::Name("/authority"));
  Provider provider{Provider::LocalMockTag{}, face, ndn::Name("/group"), recipient, authority,
    writeFixedKeychainTrust(temp.path(), keys)};
  std::shared_ptr<std::atomic<bool>> active = std::make_shared<std::atomic<bool>>(true);
  ndn::Buffer secret = ndn::Buffer(32, 0x5a);
  ndn::Buffer aad = ndn::Buffer(32, 0x42);
  ndn_service_framework::RecipientEncryptedAssignment capsule;
  std::string publicDigest;
  std::unique_ptr<Provider::CollaborationContext> context;

  RecipientConsumerFixture()
  {
    provider.useSigningKeyChainForTest(keys);
    const auto publicKey = recipient.getPublicKey();
    publicDigest = "sha256:" + sha256({publicKey.begin(), publicKey.end()});
    capsule = ndn_service_framework::encryptRecipientAssignment(secret, publicKey,
      recipient.getIdentity(), recipient.getName(), aad);
    Provider::CollaborationAssignment assignment;
    assignment.role = "decoder";
    assignment.service = ndn::Name("/SERVICE/infer");
    assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    assignment.workCurrent = [live = active] { return live->load(); };
    context = std::make_unique<Provider::CollaborationContext>(provider,
      ndn::Name("/owner"), ndn::Name("/owner/request/1"),
      ndn_service_framework::RequestMessage{}, assignment);
  }

  bool consume(const ndn_service_framework::RecipientEncryptedAssignment& encrypted,
    const ndn::Name& certificate, const std::string& digest, const ndn::Buffer& associatedData,
    const std::function<void(const ndn::Buffer&)>& callback)
  {
    return context->consumeRecipientAssignmentCurrent(encrypted, certificate, digest,
      associatedData, std::chrono::steady_clock::now() + std::chrono::seconds(2), callback);
  }

  bool sign(ndn::Data& data)
  {
    return context->signProducerPacketCurrent(data,
      std::chrono::steady_clock::now() + std::chrono::seconds(2));
  }

  template<typename Work>
  bool pump(Work work)
  {
    face.getIoContext().restart();
    auto result = std::async(std::launch::async, std::move(work));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
      face.getIoContext().restart();
      face.getIoContext().poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOST_REQUIRE(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    return result.get();
  }
};

BOOST_AUTO_TEST_CASE(CurrentRecipientConsumerUsesOwnerThreadAndExactCryptoBindings)
{
  RecipientConsumerFixture fixture;
  size_t calls = 0;
  const auto mainThread = std::this_thread::get_id();
  bool workerOnly = false;
  ndn::Buffer recovered;
  const auto accept = [&](const ndn::Buffer& secret) {
    ++calls;
    workerOnly = std::this_thread::get_id() != mainThread;
    recovered = secret;
  };
  const auto run = [&](const auto& encrypted, const auto& certificate,
                       const auto& digest, const auto& aad) {
    return fixture.pump([&] { return fixture.consume(encrypted, certificate, digest, aad, accept); });
  };
  BOOST_CHECK(run(fixture.capsule, fixture.recipient.getName(), fixture.publicDigest, fixture.aad));
  BOOST_CHECK(workerOnly);
  BOOST_CHECK_EQUAL(calls, 1U);
  BOOST_CHECK(recovered == fixture.secret);
  BOOST_CHECK(!run(fixture.capsule, fixture.authority.getName(), fixture.publicDigest, fixture.aad));
  BOOST_CHECK(!run(fixture.capsule, fixture.recipient.getName(), "sha256:" + std::string(64, '0'),
    fixture.aad));
  BOOST_CHECK(!run(fixture.capsule, fixture.recipient.getName(), fixture.publicDigest,
    ndn::Buffer(32, 0)));
  BOOST_CHECK(!run(fixture.capsule, fixture.recipient.getName(), fixture.publicDigest,
    ndn::Buffer(65537, 0)));
  auto oversized = fixture.capsule;
  BOOST_CHECK_THROW(oversized.setField("ciphertext", std::string(4097, 'a')),
    std::invalid_argument);
  BOOST_CHECK(!fixture.pump([&] {
    return fixture.context->consumeRecipientAssignmentCurrent(fixture.capsule,
      fixture.recipient.getName(), fixture.publicDigest, fixture.aad,
      std::chrono::steady_clock::now(), accept);
  }));
  bool faceAccepted = true;
  fixture.face.getIoContext().restart();
  boost::asio::post(fixture.face.getIoContext(), [&] {
    faceAccepted = fixture.consume(fixture.capsule, fixture.recipient.getName(),
      fixture.publicDigest, fixture.aad, accept);
  });
  fixture.face.getIoContext().poll();
  BOOST_CHECK(!faceAccepted);
  BOOST_CHECK(!fixture.pump([&] {
    return fixture.consume(fixture.capsule, fixture.recipient.getName(), fixture.publicDigest,
      fixture.aad, [&](const ndn::Buffer&) {
        ++calls;
        fixture.context->revokeCurrentWorkAuthority();
      });
  }));
  BOOST_CHECK(!fixture.context->hasCurrentWorkAuthority());
  BOOST_CHECK(!run(fixture.capsule, fixture.recipient.getName(), fixture.publicDigest, fixture.aad));
  BOOST_CHECK_EQUAL(calls, 2U);
}

BOOST_AUTO_TEST_CASE(CurrentRecipientConsumerKeepsSlotsUntilCancelledCallbacksDrain)
{
  RecipientConsumerFixture fixture;
  fixture.face.getIoContext().restart();
  const auto pending = ndn_service_framework::ServiceProviderTestAccess::observeRecipientDecrypts(
    fixture.provider);
  std::atomic<size_t> calls{0};
  const auto invoke = [&] {
    return fixture.consume(fixture.capsule, fixture.recipient.getName(), fixture.publicDigest,
      fixture.aad, [&](const ndn::Buffer&) { ++calls; });
  };
  std::vector<std::future<bool>> waiters;
  for (size_t i = 0; i < 16; ++i)
    waiters.push_back(std::async(std::launch::async, invoke));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (pending() != 16 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  BOOST_REQUIRE_EQUAL(pending(), 16U);
  BOOST_CHECK(!invoke());
  ndn::Data packet(ndn::Name(fixture.recipient.getIdentity()).append("kv").appendSegment(0));
  packet.setContent(fixture.aad);
  BOOST_CHECK(!fixture.sign(packet));
  *fixture.active = false;
  for (auto& waiter : waiters) BOOST_CHECK(!waiter.get());
  BOOST_CHECK_EQUAL(pending(), 16U);
  *fixture.active = true;
  BOOST_CHECK(!invoke());
  fixture.face.getIoContext().poll();
  BOOST_CHECK_EQUAL(pending(), 0U);
  BOOST_CHECK_EQUAL(calls.load(), 0U);
  BOOST_CHECK(fixture.pump(invoke));
  BOOST_CHECK_EQUAL(calls.load(), 1U);
  BOOST_CHECK(fixture.pump([&] { return fixture.sign(packet); }));
  BOOST_CHECK(ndn::security::verifySignature(packet, fixture.recipient));
}

BOOST_AUTO_TEST_CASE(CurrentProducerSignerPreservesApplicationFieldsAndFailedInputs)
{
  RecipientConsumerFixture fixture;
  ndn::Data packet(ndn::Name(fixture.recipient.getIdentity()).append("kv").appendSegment(0));
  packet.setContent(fixture.aad);
  packet.setFreshnessPeriod(ndn::time::milliseconds(60000));
  packet.setFinalBlock(ndn::name::Component::fromSegment(0));
  const auto name = packet.getName();
  const auto meta = packet.getMetaInfo().wireEncode();
  const auto content = packet.getContent();
  BOOST_REQUIRE(fixture.pump([&] { return fixture.sign(packet); }));
  BOOST_CHECK(packet.getName() == name);
  BOOST_CHECK(packet.getMetaInfo().wireEncode() == meta);
  BOOST_CHECK(packet.getContent() == content);
  BOOST_CHECK(ndn::security::verifySignature(packet, fixture.recipient));
  const auto unchanged = [](const ndn::Data& data) {
    const auto& wire = data.wireEncode();
    return std::vector<uint8_t>(wire.begin(), wire.end());
  };
  auto foreign = packet;
  foreign.setName(ndn::Name("/foreign/kv").appendSegment(0));
  const auto foreignWire = unchanged(foreign);
  BOOST_CHECK(!fixture.pump([&] { return fixture.sign(foreign); }));
  BOOST_CHECK(unchanged(foreign) == foreignWire);
  auto oversized = packet;
  oversized.setContent(ndn::Buffer(ndn::MAX_NDN_PACKET_SIZE + 1));
  const auto oversizedWire = unchanged(oversized);
  BOOST_CHECK(!fixture.pump([&] { return fixture.sign(oversized); }));
  BOOST_CHECK(unchanged(oversized) == oversizedWire);
  const auto validWire = unchanged(packet);
  BOOST_CHECK(!fixture.pump([&] {
    return fixture.context->signProducerPacketCurrent(packet, std::chrono::steady_clock::now());
  }));
  BOOST_CHECK(unchanged(packet) == validWire);
  bool faceAccepted = true;
  bool faceExecuted = false;
  fixture.face.getIoContext().restart();
  boost::asio::post(fixture.face.getIoContext(), [&] {
    faceExecuted = true;
    faceAccepted = fixture.sign(packet);
  });
  fixture.face.getIoContext().poll();
  BOOST_CHECK(faceExecuted);
  BOOST_CHECK(!faceAccepted);
  BOOST_CHECK(unchanged(packet) == validWire);
  BOOST_REQUIRE(fixture.context->revokeCurrentWorkAuthority());
  BOOST_CHECK(!fixture.pump([&] { return fixture.sign(packet); }));
  BOOST_CHECK(unchanged(packet) == validWire);
}

BOOST_AUTO_TEST_CASE(TargetedProducerSignerNeedsNoCollaborationAndSharesBoundedOwner)
{
  RecipientConsumerFixture fixture;
  fixture.context.reset();
  const auto current = [active = fixture.active] {
    if (!active->load()) throw std::runtime_error("HANDOFF_CANCELLED");
  };
  const auto sign = [&](ndn::Data& packet) {
    return fixture.provider.signProducerDataUntil(packet,
      std::chrono::steady_clock::now() + std::chrono::seconds(2), current);
  };
  ndn::Data packet(ndn::Name(fixture.recipient.getIdentity())
    .append("withdrawal-intent").append("v1").append("event-1"));
  packet.setContent(fixture.aad);
  packet.setFreshnessPeriod(ndn::time::milliseconds(60000));
  const auto name = packet.getName();
  const auto meta = packet.getMetaInfo().wireEncode();
  BOOST_REQUIRE(fixture.pump([&] { return sign(packet); }));
  BOOST_CHECK(packet.getName() == name);
  BOOST_CHECK(packet.getMetaInfo().wireEncode() == meta);
  BOOST_CHECK(packet.getContent().value_size() == fixture.aad.size());
  BOOST_CHECK(ndn::security::verifySignature(packet, fixture.recipient));
  const auto wire = [](const ndn::Data& data) {
    const auto& encoded = data.wireEncode();
    return std::vector<uint8_t>(encoded.begin(), encoded.end());
  };
  const auto original = wire(packet);
  auto foreign = packet;
  foreign.setName(ndn::Name("/foreign/withdrawal-intent/v1/event-1"));
  const auto foreignWire = wire(foreign);
  BOOST_CHECK(!fixture.pump([&] { return sign(foreign); }));
  BOOST_CHECK(wire(foreign) == foreignWire);
  BOOST_CHECK(!fixture.pump([&] {
    return fixture.provider.signProducerDataUntil(packet,
      std::chrono::steady_clock::now(), current);
  }));
  BOOST_CHECK(wire(packet) == original);
  BOOST_CHECK(!fixture.pump([&] {
    return fixture.provider.signProducerDataUntil(packet,
      std::chrono::steady_clock::now() + std::chrono::seconds(2), {});
  }));
  BOOST_CHECK(wire(packet) == original);

  const auto pending = ndn_service_framework::ServiceProviderTestAccess::observeRecipientDecrypts(
    fixture.provider);
  std::vector<ndn::Data> packets(16, packet);
  std::vector<std::future<bool>> waiters;
  for (std::size_t i = 0; i < packets.size(); ++i)
    waiters.push_back(std::async(std::launch::async, [&, i] { return sign(packets[i]); }));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (pending() != 16 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  BOOST_REQUIRE_EQUAL(pending(), 16U);
  BOOST_CHECK(!sign(packet));
  *fixture.active = false;
  for (auto& waiter : waiters) BOOST_CHECK(!waiter.get());
  for (const auto& item : packets) BOOST_CHECK(wire(item) == original);
  BOOST_CHECK_EQUAL(pending(), 16U);
  *fixture.active = true;
  BOOST_CHECK(!sign(packet));
  fixture.face.getIoContext().poll();
  BOOST_CHECK_EQUAL(pending(), 0U);
  BOOST_CHECK(fixture.pump([&] { return sign(packet); }));
  BOOST_CHECK(ndn::security::verifySignature(packet, fixture.recipient));
}

BOOST_AUTO_TEST_CASE(OriginalPacketValidationRequiresTrustProducerAndExactWire)
{
  TempRoot temp;
  ndn::Face face;
  ndn::KeyChain keys("pib-memory:spec221-packet-validation", "tpm-memory:spec221-packet-validation");
  const auto certificate = makeRsaIdentity(keys, ndn::Name("/provider/a"));
  const auto foreign = makeRsaIdentity(keys, ndn::Name("/provider/b"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face, ndn::Name("/group"),
    certificate, foreign, writeFixedKeychainTrust(temp.path(), keys));
  ndn::Data packet(ndn::Name("/provider/a/kv/checkpoint/seg=0"));
  const std::vector<uint8_t> content{1, 2, 3};
  packet.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
  keys.sign(packet, ndn::security::signingByCertificate(certificate));
  const auto encoded = packet.wireEncode();
  const std::vector<uint8_t> wire(encoded.begin(), encoded.end());
  const auto verify = [&](const std::vector<uint8_t>& bytes, const ndn::Name& producer) {
    return provider.validateProducerPacketWireUntil(bytes, producer,
      std::chrono::steady_clock::now() + std::chrono::seconds(1));
  };
  BOOST_CHECK(verify(wire, ndn::Name("/provider/a")));
  BOOST_CHECK(!verify(wire, ndn::Name("/provider/b")));
  auto trailing = wire;
  trailing.push_back(0);
  BOOST_CHECK(!verify(trailing, ndn::Name("/provider/a")));
  BOOST_CHECK(!verify({}, ndn::Name("/provider/a")));
  BOOST_CHECK(!verify({1, 2, 3}, ndn::Name("/provider/a")));
  auto corrupted = wire;
  corrupted.back() ^= 1;
  BOOST_CHECK(!verify(corrupted, ndn::Name("/provider/a")));
  keys.sign(packet, ndn::security::signingByCertificate(foreign));
  const auto foreignWire = packet.wireEncode();
  BOOST_CHECK(!verify(std::vector<uint8_t>(foreignWire.begin(), foreignWire.end()),
    ndn::Name("/provider/a")));
  BOOST_CHECK(!provider.validateProducerPacketWireUntil(wire, ndn::Name("/provider/a"),
    std::chrono::steady_clock::now()));
  provider.stopProducerPacketValidation();
  BOOST_CHECK(!verify(wire, ndn::Name("/provider/a")));
}

BOOST_AUTO_TEST_CASE(OriginalCertificateAdmissionChecksSubjectIssuerAndSeedsOnlyCandidates)
{
  TempRoot temp;
  ndn::DummyClientFace face(ndn::DummyClientFace::Options{true, true});
  ndn::KeyChain keys("pib-memory:spec221-certificates", "tpm-memory:spec221-certificates");
  const auto source = makeRsaIdentity(keys, ndn::Name("/provider/a"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/authority"));
  const auto foreign = makeRsaIdentity(keys, ndn::Name("/foreign"));
  const auto target = makeRsaIdentity(keys, ndn::Name("/target/a"));
  const auto schemaPath = temp.path() / "certificate-trust.conf";
  std::ofstream schema(schemaPath);
  schema << "rule { id source-certificate for data "
            "filter { type name name /provider/a/KEY relation is-prefix-of } "
            "checker { type customized sig-type rsa-sha256 "
            "key-locator { type name name /authority relation equal } } }\n"
            "rule { id application for data checker { type customized sig-type rsa-sha256 "
            "key-locator { type name regex \"^<>*<KEY|8=KEY><>{1,3}$\" } } }\n";
  for (const auto& anchor : {authority, foreign}) {
    std::ostringstream encoded;
    ndn::io::save(anchor, encoded);
    auto base64 = encoded.str();
    base64.erase(std::remove(base64.begin(), base64.end(), '\n'), base64.end());
    schema << "trust-anchor { type base64 base64-string \"" << base64 << "\" }\n";
  }
  schema.close();
  BOOST_REQUIRE(schema);
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face, ndn::Name("/group"),
    target, authority, schemaPath.string());
  ndn_service_framework::ServiceProviderTestAccess::configureFaceBackedProducerValidator(
    provider, schemaPath.string(), face);
  provider.setHandlerThreads(1);
  ndn::security::MakeCertificateOptions options;
  options.version = 101;
  const auto issued = keys.makeCertificate(source,
    ndn::security::signingByCertificate(authority), options);
  BOOST_REQUIRE(issued.getValidityPeriod().isValid());
  ndn_service_framework::ServiceProviderTestAccess::checkCertificateTrust(provider, issued);
  keys.addCertificate(keys.getPib().getIdentity(source.getIdentity()).getDefaultKey(), issued);
  const auto wireOf = [](const ndn::Data& data) {
    const auto wire = data.wireEncode();
    return std::vector<uint8_t>(wire.begin(), wire.end());
  };
  const auto verify = [&](const std::vector<uint8_t>& wire, const ndn::Name& subject) {
    auto result = std::async(std::launch::async, [&] {
      return provider.validateProducerPacketWireUntil(wire, subject,
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
      face.getIoContext().restart();
      face.getIoContext().poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    BOOST_REQUIRE(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    return result.get();
  };
  using Access = ndn_service_framework::ServiceProviderTestAccess;
  BOOST_CHECK(!Access::knowsCertificate(provider, issued.getName()));
  BOOST_CHECK(!verify(wireOf(issued), ndn::Name("/provider/b")));
  auto damaged = wireOf(issued);
  damaged.back() ^= 1;
  BOOST_CHECK(!verify(damaged, source.getIdentity()));
  options.version = 102;
  const auto wrongIssuer = keys.makeCertificate(source,
    ndn::security::signingByCertificate(foreign), options);
  BOOST_CHECK(!verify(wireOf(wrongIssuer), source.getIdentity()));
  options.version = 103;
  const auto now = ndn::time::system_clock::now();
  options.validity = ndn::security::ValidityPeriod(now - ndn::time::hours(2),
                                                 now - ndn::time::hours(1));
  const auto expired = keys.makeCertificate(source,
    ndn::security::signingByCertificate(authority), options);
  BOOST_CHECK(!verify(wireOf(expired), source.getIdentity()));
  auto malformedKey = issued;
  const std::vector<uint8_t> invalidDer{1, 2, 3};
  malformedKey.setContent(invalidDer);
  keys.sign(malformedKey, ndn::security::signingByCertificate(authority)
    .setSignatureInfo(issued.getSignatureInfo()));
  BOOST_CHECK(!verify(wireOf(malformedKey), source.getIdentity()));
  BOOST_CHECK(!Access::knowsCertificate(provider, issued.getName()));
  BOOST_CHECK(verify(wireOf(issued), source.getIdentity()));
  BOOST_CHECK(Access::knowsCertificate(provider, issued.getName()));
  size_t certificateInterests = 0;
  auto connection = face.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (source.getKeyName().isPrefixOf(interest.getName())) ++certificateInterests;
  });
  ndn::Data application(ndn::Name("/provider/a/kv/checkpoint"));
  keys.sign(application, ndn::security::signingByCertificate(issued));
  BOOST_CHECK(verify(wireOf(application), source.getIdentity()));
  BOOST_CHECK_EQUAL(certificateInterests, 0U);
  // Trusting an issuer for certificates does not delegate application signing.
  keys.sign(application, ndn::security::signingByCertificate(authority));
  BOOST_CHECK(!verify(wireOf(application), source.getIdentity()));
  application.setContentType(ndn::tlv::ContentType_Key);
  application.setContent(source.getPublicKey());
  keys.sign(application, ndn::security::signingByCertificate(issued));
  BOOST_CHECK(verify(wireOf(application), source.getIdentity()));
  BOOST_CHECK(!Access::knowsCertificate(provider, application.getName()));
  application.setName(issued.getName());
  application.setContentType(ndn::tlv::ContentType_Blob);
  keys.sign(application, ndn::security::signingByCertificate(issued));
  BOOST_CHECK(!verify(wireOf(application), source.getIdentity()));
}

BOOST_AUTO_TEST_CASE(OriginalPacketValidationBoundsPendingCallbacksAndWakesOnShutdown)
{
  TempRoot temp;
  auto face = std::make_unique<ndn::DummyClientFace>();
  ndn::KeyChain keys("pib-memory:spec221-validation-stop", "tpm-memory:spec221-validation-stop");
  const auto certificate = makeRsaIdentity(keys, ndn::Name("/provider/a"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/authority"));
  const auto schema = writeFixedKeychainTrust(temp.path(), keys);
  auto provider = std::make_unique<ndn_service_framework::ServiceProvider>(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, *face, ndn::Name("/group"),
    certificate, authority, schema);
  ndn_service_framework::ServiceProviderTestAccess::configureFaceBackedProducerValidator(
    *provider, schema, *face);
  ndn::Data packet(ndn::Name("/provider/a/kv/checkpoint/seg=0"));
  keys.sign(packet, ndn::security::signingByCertificate(certificate));
  const auto encoded = packet.wireEncode();
  const std::vector<uint8_t> wire(encoded.begin(), encoded.end());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  // Zero-worker dispatch is on Face and must reject before waiting.
  provider->setHandlerThreads(0);
  BOOST_CHECK(!provider->validateProducerPacketWireUntil(wire, ndn::Name("/provider/a"), deadline));
  provider->setHandlerThreads(1);
  const auto outstanding =
    ndn_service_framework::ServiceProviderTestAccess::observeProducerValidationWaiters(*provider);
  bool faceThreadRejected = false;
  boost::asio::post(face->getIoContext(), [&] {
    faceThreadRejected = !provider->validateProducerPacketWireUntil(
      wire, ndn::Name("/provider/a"), deadline);
  });
  face->getIoContext().restart();
  (void)face->getIoContext().poll();
  BOOST_CHECK(faceThreadRejected);
  BOOST_CHECK_EQUAL(outstanding(), 0U);
  BOOST_CHECK(!provider->validateProducerPacketWireUntil(wire, ndn::Name("/provider/a"),
    std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
  BOOST_CHECK_EQUAL(outstanding(), 1U);
  face->getIoContext().restart();
  (void)face->getIoContext().poll();
  BOOST_CHECK_EQUAL(outstanding(), 0U);
  auto valid = std::async(std::launch::async, [&] {
    return provider->validateProducerPacketWireUntil(wire, ndn::Name("/provider/a"), deadline);
  });
  while (valid.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    face->getIoContext().restart();
    (void)face->getIoContext().poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  BOOST_REQUIRE(valid.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
  BOOST_CHECK(valid.get());
  BOOST_CHECK_EQUAL(outstanding(), 0U);
  std::vector<std::future<bool>> waiters;
  for (size_t i = 0; i < 16; ++i) {
    waiters.emplace_back(std::async(std::launch::async, [&] {
      return provider->validateProducerPacketWireUntil(wire, ndn::Name("/provider/a"), deadline);
    }));
  }
  const auto admissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (outstanding() != 16 && std::chrono::steady_clock::now() < admissionDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  BOOST_REQUIRE_EQUAL(outstanding(), 16U);
  BOOST_CHECK(!provider->validateProducerPacketWireUntil(wire, ndn::Name("/provider/a"), deadline));
  provider->stopProducerPacketValidation();
  for (auto& waiter : waiters) {
    BOOST_REQUIRE(waiter.wait_until(deadline) == std::future_status::ready);
    BOOST_CHECK(!waiter.get());
  }
  provider.reset();
  BOOST_CHECK_EQUAL(outstanding(), 16U);
  face->getIoContext().restart();
  (void)face->getIoContext().poll();
  face.reset();
  BOOST_CHECK_EQUAL(outstanding(), 0U);
}

BOOST_AUTO_TEST_CASE(OriginalPacketSetVerifiesReplicaCertificateBeforeContentWithoutProducer)
{
  using Oracle = ndn_service_framework::test::ProducerPacketValidationOracle;
  TempRoot temp;
  ndn::DummyClientFace face(ndn::DummyClientFace::Options{true, true});
  ndn::KeyChain keys("pib-memory:spec221-replica-cert", "tpm-memory:spec221-replica-cert");
  const auto source = makeRsaIdentity(keys, ndn::Name("/producer/source"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/authority"));
  const auto owner = makeRsaIdentity(keys, ndn::Name("/owner/fresh"));
  const auto schemaPath = temp.path() / "replica-trust.conf";
  std::ofstream schema(schemaPath);
  schema << "rule { id certificate for data "
            "filter { type name name /producer/source/KEY relation is-prefix-of } "
            "checker { type customized sig-type rsa-sha256 "
            "key-locator { type name name /authority relation equal } } }\n"
            "rule { id application for data checker { type customized sig-type rsa-sha256 "
            "key-locator { type name regex \"^<>*<KEY|8=KEY><>{1,3}$\" } } }\n";
  std::ostringstream encodedAnchor;
  ndn::io::save(authority, encodedAnchor);
  auto anchor = encodedAnchor.str();
  anchor.erase(std::remove(anchor.begin(), anchor.end(), '\n'), anchor.end());
  schema << "trust-anchor { type base64 base64-string \"" << anchor << "\" }\n";
  schema.close();
  BOOST_REQUIRE(schema);
  ndn_service_framework::ServiceUser user(
    ndn_service_framework::ServiceUser::LocalMockTag{}, face, ndn::Name("/group"),
    owner, authority, schemaPath.string());
  Oracle::configure(user, schemaPath.string(), face);
  ndn::security::MakeCertificateOptions options;
  options.version = 301;
  const auto certificate = keys.makeCertificate(source,
    ndn::security::signingByCertificate(authority), options);
  keys.addCertificate(keys.getPib().getIdentity(source.getIdentity()).getDefaultKey(), certificate);
  ndn::Data root(ndn::Name("/producer/source/kv/manifest"));
  ndn::Data segment(ndn::Name("/producer/source/kv").appendSegment(0));
  keys.sign(root, ndn::security::signingByCertificate(certificate));
  keys.sign(segment, ndn::security::signingByCertificate(certificate));
  std::map<ndn::Name, ndn::Data> stored;
  std::vector<std::vector<uint8_t>> expected;
  RepoDataReference reference;
  reference.objectName = root.getName().toUri();
  reference.dataPrefix = source.getIdentity().toUri();
  reference.forwardingHint = "/replica/storage";
  reference.packetSetMetadata.emplace();
  auto& metadata = *reference.packetSetMetadata;
  metadata.objectName = reference.objectName;
  metadata.objectType = "original-packet-set";
  metadata.dataPrefix = reference.dataPrefix;
  metadata.packetOffsets = {0};
  std::vector<uint8_t> aggregate;
  for (const auto& packet : std::vector<ndn::Data>{root, segment, certificate}) {
    const auto wire = packet.wireEncode();
    expected.emplace_back(wire.begin(), wire.end());
    aggregate.insert(aggregate.end(), wire.begin(), wire.end());
    metadata.packetNames.push_back(packet.getName().toUri());
    metadata.packetOffsets.push_back(aggregate.size());
    stored.emplace(packet.getName(), packet);
  }
  reference.expectedSize = metadata.size = aggregate.size();
  reference.expectedSha256 = metadata.sha256 = sha256(aggregate);
  std::size_t unexpectedInterests = 0;
  std::ostringstream unexpectedInterestNames;
  auto connection = face.onSendInterest.connect([&](const ndn::Interest& interest) {
    // The real LocalMock User still bootstraps its NAC public parameters.
    // This fixture supplies no NAC response; only certificate/Data fetches are counted here.
    if (interest.getName() == ndn::Name("/authority/PUBPARAMS")) {
      return;
    }
    const auto found = stored.find(interest.getName());
    if (found != stored.end() && interest.getForwardingHint().size() == 1 &&
        interest.getForwardingHint().front() == ndn::Name(reference.forwardingHint))
      face.receive(found->second);
    else {
      ++unexpectedInterests;
      unexpectedInterestNames << interest.getName() << '\n';
    }
  });
  BOOST_CHECK(!Oracle::knowsCertificate(user, certificate.getName()));
  std::vector<ndn::Name> verified;
  const auto verifier = [&](const ndn::Data& data) {
    verified.push_back(data.getName());
    const auto wire = data.wireEncode();
    return user.validateProducerPacketWireUntil(
      std::vector<uint8_t>(wire.begin(), wire.end()), source.getIdentity(),
      std::chrono::steady_clock::now() + std::chrono::seconds(2));
  };
  const auto run = [&](const RepoDataReference& selected) {
    auto result = std::async(std::launch::async, [&] {
      return RepoClient::fetchOriginalDataPacketSet(user, selected, verifier, 3000);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
      face.getIoContext().restart();
      (void)face.getIoContext().poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (result.wait_until(deadline) != std::future_status::ready)
      throw std::runtime_error("replica-fetch-fixture-deadline");
    return result.get();
  };
  BOOST_CHECK(run(reference) == expected);
  BOOST_REQUIRE_EQUAL(verified.size(), expected.size());
  BOOST_CHECK(verified.front() == certificate.getName());
  BOOST_CHECK(verified[1] == root.getName());
  BOOST_CHECK(Oracle::knowsCertificate(user, certificate.getName()));
  BOOST_CHECK_MESSAGE(unexpectedInterests == 0U, unexpectedInterestNames.str());
  verified.clear();
  auto damaged = reference;
  damaged.expectedSha256 = damaged.packetSetMetadata->sha256 = std::string(64, '0');
  BOOST_CHECK_THROW(run(damaged), std::runtime_error);
  BOOST_CHECK(verified.empty());
  auto invalid = reference;
  invalid.packetSetMetadata->packetOffsets.pop_back();
  BOOST_CHECK_THROW(run(invalid), std::invalid_argument);
  bool executed = false;
  bool rejected = false;
  boost::asio::post(face.getIoContext(), [&] {
    executed = true;
    try { (void)RepoClient::fetchOriginalDataPacketSet(user, reference, verifier, 100); }
    catch (const std::logic_error&) { rejected = true; }
  });
  face.getIoContext().restart();
  (void)face.getIoContext().poll();
  BOOST_CHECK(executed);
  BOOST_CHECK(rejected);
}

BOOST_AUTO_TEST_CASE(OwnerOriginalPacketValidationUsesConfiguredTrustAndSubject)
{
  TempRoot temp;
  ndn::DummyClientFace face;
  ndn::KeyChain keys("pib-memory:spec221-owner-validation", "tpm-memory:spec221-owner-validation");
  const auto owner = makeRsaIdentity(keys, ndn::Name("/owner/a"));
  const auto source = makeRsaIdentity(keys, ndn::Name("/provider/a"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/authority"));
  const auto schema = writeFixedKeychainTrust(temp.path(), keys);
  ndn_service_framework::ServiceUser user(
    ndn_service_framework::ServiceUser::LocalMockTag{}, face, ndn::Name("/group"),
    owner, authority, schema);
  const auto verify = [&](const ndn::Data& data, const ndn::Name& producer) {
    const auto wire = data.wireEncode();
    return user.validateProducerPacketWireUntil(
      std::vector<uint8_t>(wire.begin(), wire.end()), producer,
      std::chrono::steady_clock::now() + std::chrono::seconds(2));
  };
  ndn::Data data(ndn::Name("/provider/a/kv/checkpoint").appendSegment(0));
  data.setContent(ndn::Buffer{1, 2, 3});
  keys.sign(data, ndn::security::signingByCertificate(source));
  BOOST_CHECK(verify(source, source.getIdentity()));
  BOOST_CHECK(!verify(source, owner.getIdentity()));
  BOOST_CHECK(verify(data, source.getIdentity()));
  BOOST_CHECK(!verify(data, owner.getIdentity()));
  data.setContent(ndn::Buffer{4, 5, 6});
  BOOST_CHECK(!verify(data, source.getIdentity()));
  keys.sign(data, ndn::security::signingByCertificate(source));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  BOOST_CHECK(!user.validateProducerPacketWireUntil({}, source.getIdentity(), deadline));
  BOOST_CHECK(!user.validateProducerPacketWireUntil({0x06}, source.getIdentity(), deadline));
  user.stopProducerPacketValidation();
  BOOST_CHECK(!verify(data, source.getIdentity()));
}

BOOST_AUTO_TEST_CASE(OwnerOriginalPacketValidationBoundsCallbacksThroughTeardown)
{
  using Oracle = ndn_service_framework::test::ProducerPacketValidationOracle;
  TempRoot temp;
  auto face = std::make_unique<ndn::DummyClientFace>();
  ndn::KeyChain keys("pib-memory:spec221-owner-validation-stop",
                     "tpm-memory:spec221-owner-validation-stop");
  const auto owner = makeRsaIdentity(keys, ndn::Name("/owner/a"));
  const auto source = makeRsaIdentity(keys, ndn::Name("/provider/a"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/authority"));
  const auto schema = writeFixedKeychainTrust(temp.path(), keys);
  auto user = std::make_unique<ndn_service_framework::ServiceUser>(
    ndn_service_framework::ServiceUser::LocalMockTag{}, *face, ndn::Name("/group"),
    owner, authority, schema);
  Oracle::configure(*user, schema, *face);
  const auto pending = Oracle::pending(*user);
  ndn::Data packet(ndn::Name("/provider/a/kv/checkpoint").appendSegment(0));
  keys.sign(packet, ndn::security::signingByCertificate(source));
  const auto encoded = packet.wireEncode();
  const std::vector<uint8_t> wire(encoded.begin(), encoded.end());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool faceCallbackExecuted = false;
  bool faceThreadRejected = false;
  boost::asio::post(face->getIoContext(), [&] {
    faceCallbackExecuted = true;
    faceThreadRejected = !user->validateProducerPacketWireUntil(
      wire, source.getIdentity(), deadline);
  });
  face->getIoContext().restart();
  (void)face->getIoContext().poll();
  BOOST_REQUIRE(faceCallbackExecuted);
  BOOST_CHECK(faceThreadRejected);
  BOOST_CHECK_EQUAL(pending(), 0U);
  BOOST_CHECK(!user->validateProducerPacketWireUntil(wire, source.getIdentity(),
    std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
  BOOST_CHECK_EQUAL(pending(), 1U);
  face->getIoContext().restart();
  (void)face->getIoContext().poll();
  BOOST_CHECK_EQUAL(pending(), 0U);
  auto valid = std::async(std::launch::async, [&] {
    return user->validateProducerPacketWireUntil(wire, source.getIdentity(), deadline);
  });
  while (valid.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    face->getIoContext().restart();
    (void)face->getIoContext().poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  BOOST_REQUIRE(valid.wait_until(deadline) == std::future_status::ready);
  BOOST_CHECK(valid.get());
  BOOST_CHECK_EQUAL(pending(), 0U);
  std::vector<std::future<bool>> waiters;
  for (size_t i = 0; i < 16; ++i) {
    waiters.emplace_back(std::async(std::launch::async, [&] {
      return user->validateProducerPacketWireUntil(wire, source.getIdentity(), deadline);
    }));
  }
  const auto admissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (pending() != 16 && std::chrono::steady_clock::now() < admissionDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  BOOST_REQUIRE_EQUAL(pending(), 16U);
  BOOST_CHECK(!user->validateProducerPacketWireUntil(wire, source.getIdentity(), deadline));
  // Stop wakes callers; actor teardown precedes the late callbacks' drain.
  user->stopProducerPacketValidation();
  for (auto& waiter : waiters) {
    BOOST_REQUIRE(waiter.wait_until(deadline) == std::future_status::ready);
    BOOST_CHECK(!waiter.get());
  }
  user.reset();
  BOOST_CHECK_EQUAL(pending(), 16U);
  face->getIoContext().restart();
  (void)face->getIoContext().poll();
  face.reset();
  BOOST_CHECK_EQUAL(pending(), 0U);
}

BOOST_AUTO_TEST_CASE(OriginalPacketReaderCommitsReusesFileAndSurvivesReopen)
{
  TempRoot temp;
  OriginalPacketSource source;
  StorageCapability capability;
  capability.repoNode = "/repo/a";
  capability.freeBytes = 4 * 1024 * 1024;
  const auto root = (temp.path() / "objects").string();
  const auto file = temp.path() / "original.wire";
  {
    std::ofstream output(file, std::ios::binary);
    output.write(reinterpret_cast<const char*>(source.wire.data()), source.wire.size());
    BOOST_REQUIRE(output.good());
  }
  {
    auto backend = std::make_shared<FilesystemRepoStoreBackend>(root, 4 * 1024 * 1024,
      1024, "reader-first");
    RepoCore core(capability, backend);
    std::size_t reads = 0;
    std::size_t guards = 0;
    const auto stored = core.commitOriginalPacketReader(source.manifest,
      [&](uint64_t offset, std::size_t length) {
        ++reads;
        BOOST_CHECK_LE(length, 1024U * 1024U);
        return source.read(offset, length);
      }, [&] { ++guards; });
    BOOST_CHECK_GE(reads, source.manifest.packetNames.size() + 1);
    BOOST_CHECK_GT(guards, 0U);
    BOOST_CHECK(stored.hasSameMetadata(source.manifest));
    BOOST_CHECK(core.getRange(source.manifest.objectName, {0, source.wire.size()}) == source.wire);
    BOOST_CHECK(core.get(source.manifest.objectName) ==
      source.read(0, source.manifest.packetOffsets[1]));
    const auto repeated = core.commitOriginalPacketFile(source.manifest, file);
    BOOST_CHECK(repeated.hasSameMetadata(stored));
    BOOST_CHECK_EQUAL(core.list().size(), 1U);
  }
  {
    auto backend = std::make_shared<FilesystemRepoStoreBackend>(root, 4 * 1024 * 1024,
      1024, "reader-reopened");
    RepoCore core(capability, backend);
    BOOST_CHECK(core.getRange(source.manifest.objectName, {0, source.wire.size()}) == source.wire);
    BOOST_CHECK(core.getDataPacket(source.manifest.packetNames.back()) ==
      source.read(source.manifest.packetOffsets[1],
        source.manifest.packetOffsets[2] - source.manifest.packetOffsets[1]));
    BOOST_CHECK(core.commitOriginalPacketReader(source.manifest,
      [&](uint64_t offset, std::size_t length) { return source.read(offset, length); })
      .hasSameMetadata(source.manifest));
  }
}

BOOST_AUTO_TEST_CASE(OriginalPacketReaderRejectsMissingShortAndMutatedInput)
{
  TempRoot temp;
  OriginalPacketSource source;
  StorageCapability capability;
  capability.repoNode = "/repo/a";
  capability.freeBytes = 4 * 1024 * 1024;
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(
    (temp.path() / "objects").string(), 4 * 1024 * 1024, 1024, "reader-negative");
  RepoCore core(capability, backend);
  BOOST_CHECK_THROW(core.commitOriginalPacketReader(source.manifest, {}), std::invalid_argument);
  BOOST_CHECK_THROW(core.commitOriginalPacketReader(source.manifest,
    [&](uint64_t offset, std::size_t length) {
      auto bytes = source.read(offset, length);
      bytes.pop_back();
      return bytes;
    }), std::runtime_error);
  BOOST_CHECK(core.list().empty());
  std::size_t reads = 0;
  BOOST_CHECK_THROW(core.commitOriginalPacketReader(source.manifest,
    [&](uint64_t offset, std::size_t length) {
      auto bytes = source.read(offset, length);
      if (++reads > source.manifest.packetNames.size()) {
        bytes.back() ^= 1;
      }
      return bytes;
    }), std::runtime_error);
  BOOST_CHECK(core.list().empty());
  BOOST_CHECK_EQUAL(backend->size(), 0U);
  BOOST_CHECK_NO_THROW(core.commitOriginalPacketReader(source.manifest,
    [&](uint64_t offset, std::size_t length) { return source.read(offset, length); }));
}

BOOST_AUTO_TEST_CASE(OriginalPacketReaderFailureAfterWindowRollsBackAndRestoresCapacity)
{
  TempRoot temp;
  OriginalPacketSource source(180);
  StorageCapability capability;
  capability.repoNode = "/repo/a";
  capability.freeBytes = 4 * 1024 * 1024;
  const auto root = temp.path() / "objects";
  auto backend = std::make_shared<FilesystemRepoStoreBackend>(root.string(), 4 * 1024 * 1024,
    1024, "reader-window-failure");
  RepoCore core(capability, backend);
  std::size_t windows = 0;
  BOOST_CHECK_THROW(core.commitOriginalPacketReader(source.manifest,
    [&](uint64_t offset, std::size_t length) {
      if (length > ndn::MAX_NDN_PACKET_SIZE && ++windows == 2) {
        throw std::runtime_error("fixture second window failed");
      }
      return source.read(offset, length);
    }), std::runtime_error);
  BOOST_CHECK_EQUAL(windows, 2U);
  BOOST_CHECK(core.list().empty());
  BOOST_CHECK_EQUAL(backend->size(), 0U);
  BOOST_CHECK(std::filesystem::is_empty(root / "staging"));
  // A different operation must be able to reserve the full budget after rollback.
  RepoObjectManifest probe;
  probe.objectName = "/provider/a/kv/capacity-probe";
  probe.objectType = "blob";
  probe.operationId = "capacity-probe";
  probe.generation = 1;
  probe.size = capability.freeBytes;
  probe.sha256 = sha256(std::vector<uint8_t>(probe.size, 8));
  BOOST_CHECK_NO_THROW(core.putRange(probe, {0, 0}, {}));
  BOOST_CHECK(core.abortUserStorageRangesIfOwned(probe));
  const auto stored = core.commitOriginalPacketReader(source.manifest,
    [&](uint64_t offset, std::size_t length) { return source.read(offset, length); });
  BOOST_CHECK_EQUAL(stored.size, source.wire.size());
  BOOST_CHECK(core.getRange(source.manifest.objectName, {0, source.wire.size()}) == source.wire);
}

struct OwnedPacketFixture
{
  TempRoot temp;
  ndn::Face face;
  ndn::KeyChain keys{"pib-memory:spec221-owned-node", "tpm-memory:spec221-owned-node"};
  std::vector<std::vector<uint8_t>> packets;
  std::vector<uint8_t> original;
  RepoUserStorageInsertRequest request;
  std::shared_ptr<RepoNode> node;
  std::unique_ptr<ndn_service_framework::ServiceProvider> provider;
  unsigned fetches = 0;
  FilesystemRecoveryPolicy recoveryPolicy;

  static uint64_t
  now()
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  }

  explicit OwnedPacketFixture(
    FilesystemRecoveryPolicy policy = FilesystemRecoveryPolicy::KEEP_UNCONFIRMED)
    : recoveryPolicy(policy)
  {
    const auto producer = makeRsaIdentity(keys, ndn::Name("/provider/a"));
    const auto repo = makeRsaIdentity(keys, ndn::Name("/repo/provider"));
    const auto authority = makeRsaIdentity(keys, ndn::Name("/authority"));
    request.operationId = "owned-original";
    request.generation = 1;
    auto& reference = request.reference;
    reference.objectName = "/provider/a/kv/manifest";
    reference.dataPrefix = "/provider/a";
    reference.packetSetMetadata.emplace();
    auto& metadata = *reference.packetSetMetadata;
    metadata.objectName = reference.objectName;
    metadata.objectType = "original-packet-set";
    metadata.packetNames = {reference.objectName, "/provider/a/kv/seg=0",
                            "/provider/a/keys/kv"};
    for (const auto& name : metadata.packetNames) {
      ndn::Data data{ndn::Name(name)};
      const std::vector<uint8_t> content{1, 2, static_cast<uint8_t>(packets.size())};
      data.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
      keys.sign(data, ndn::security::signingByCertificate(producer));
      const auto wire = data.wireEncode();
      packets.emplace_back(wire.begin(), wire.end());
      original.insert(original.end(), wire.begin(), wire.end());
    }
    metadata.size = reference.expectedSize = original.size();
    metadata.sha256 = reference.expectedSha256 = sha256(original);
    open();
    provider = std::make_unique<ndn_service_framework::ServiceProvider>(
      ndn_service_framework::ServiceProvider::LocalMockTag{}, face, ndn::Name("/group"),
      repo, authority, writeFixedKeychainTrust(temp.path(), keys));
    node->registerServices(*provider);
    node->setDataReferenceFetcher([this](const RepoDataReference&) {
      ++fetches;
      return packets;
    });
    const RepoUserStorageGrantRequest grant{1, "owned-grant", "/provider/a",
      reference.objectName, now() + 60000};
    const auto wire = node->handleUserStorageGrant(toBytes(grant.toJson()), "/owner/a");
    const auto granted = RepoUserStorageGrantReply::fromJson(toString(wire));
    request.storageBinding = {1, "/owner/a", grant.grantId, granted.grant.ownerEpoch};
  }

  void
  open()
  {
    StorageCapability capability;
    capability.repoNode = "/repo/a";
    capability.repoMode = "persistent";
    capability.freeBytes = 4 * 1024 * 1024;
    node = std::make_shared<RepoNode>(ndn::Name("/SERVICE/repo"), capability,
      std::make_shared<FilesystemRepoStoreBackend>((temp.path() / "objects").string(),
        4 * 1024 * 1024, 1024, "owned-node", recoveryPolicy),
      "/repo/a", "/repo/provider",
      (temp.path() / "metadata.sqlite3").string());
  }

  RepoOperationStatus
  insert()
  {
    const auto response = dispatch(*provider, ndn::Name("/provider/a"),
      makeRepoServiceName(node->servicePrefix(), "INSERT"),
      encodeUserStorageInsertRequest(request), "owned-insert");
    if (!response.getStatus()) {
      throw std::runtime_error(response.getErrorInfo());
    }
    return parseOperationStatusJson(responseString(response));
  }

  RepoUserStorageUsage
  usage()
  {
    return RepoUserStorageUsageReply::fromJson(toString(node->handleUserStorageUsage(
      toBytes(RepoUserStorageUsageRequest{}.toJson()), "/owner/a"))).usage;
  }

  RepoUserStorageDeleteReply
  erase(const std::string& owner, const std::string& cleanupId, uint64_t epoch)
  {
    const RepoUserStorageDeleteRequest cleanup{1, cleanupId,
      request.reference.objectName, epoch};
    const auto response = dispatch(*provider, ndn::Name(owner),
      makeRepoServiceName(node->servicePrefix(), "USER_STORAGE_DELETE"),
      toBytes(cleanup.toJson()), cleanupId);
    if (!response.getStatus()) {
      throw std::runtime_error(response.getErrorInfo());
    }
    return RepoUserStorageDeleteReply::fromJson(responseString(response));
  }
};

BOOST_AUTO_TEST_CASE(OwnedInsertValidatesPacketsCommitsClaimAndReopensOriginalWire)
{
  OwnedPacketFixture fixture;
  fixture.node->setDataReferenceSpoolFetcher([&](const RepoDataReference&) {
    ++fixture.fetches;
    auto spool = std::make_shared<detail::FaceDataReferencePacketSpool>(
      fixture.packets.size(), fixture.original.size());
    for (std::size_t i = fixture.packets.size(); i > 0; --i) {
      spool->append(i - 1, fixture.packets[i - 1]);
    }
    spool->finalize();
    return spool;
  });
  const std::vector<uint8_t> modelBytes{7, 8, 9};
  fixture.node->core().put("/provider/a/model/weights", modelBytes, "artifact-v2-bytes");
  const auto result = fixture.insert();
  BOOST_REQUIRE_MESSAGE(result.state == "DONE", result.message);
  BOOST_CHECK_EQUAL(result.operationId, fixture.request.operationId);
  BOOST_CHECK_EQUAL(result.completedSegments, fixture.packets.size());
  BOOST_CHECK_EQUAL(fixture.fetches, 1U);
  const auto stored = fixture.node->getManifest(fixture.request.reference.objectName);
  BOOST_CHECK(fixture.node->getRange(stored.objectName, {0, stored.size}) == fixture.original);
  BOOST_CHECK(fixture.node->getRange(fixture.request.reference.objectName,
    {0, stored.size}) == fixture.original);
  BOOST_CHECK(fixture.node->handleFetchRange(encodeRangeReadRequest(
    fixture.request.reference.objectName, {0, stored.size})) == fixture.original);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, stored.size + stored.toJson().size());
  BOOST_CHECK_EQUAL(fixture.usage().reservedBytes, 0U);
  const auto repeated = fixture.insert();
  BOOST_REQUIRE_MESSAGE(repeated.state == "DONE", repeated.message);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, stored.size + stored.toJson().size());
  fixture.provider.reset();
  fixture.node.reset();
  fixture.open();
  BOOST_CHECK(fixture.node->getRange(stored.objectName, {0, stored.size}) == fixture.original);
  BOOST_CHECK(fixture.node->core().getRange("/provider/a/model/weights", {0, modelBytes.size()})
    == modelBytes);
  const auto listed = RepoUserStorageListReply::fromJson(toString(
    fixture.node->handleUserStorageList(toBytes(RepoUserStorageListRequest{}.toJson()), "/owner/a")));
  BOOST_REQUIRE_EQUAL(listed.objects.size(), 1U);
  BOOST_CHECK(listed.objects.front().state == RepoUserStorageClaimState::COMMITTED);
}

BOOST_AUTO_TEST_CASE(LargeOwnedPacketSetReadsOriginalWireAndRechecksDeletion)
{
  OwnedPacketFixture fixture;
  auto& reference = fixture.request.reference;
  auto& metadata = *reference.packetSetMetadata;
  metadata.packetNames.clear();
  metadata.packetOffsets = {0};
  fixture.packets.clear();
  fixture.original.clear();
  const auto certificate = fixture.keys.getPib().getIdentity(ndn::Name("/provider/a"))
    .getDefaultKey().getDefaultCertificate();
  constexpr std::size_t PACKET_COUNT = 1000;
  for (std::size_t i = 0; i < PACKET_COUNT; ++i) {
    const auto name = i == 0 ? ndn::Name(reference.objectName) :
      ndn::Name("/provider/a/kv").append(std::string(64, 'a')).append("packets")
        .append(std::string(64, 'b')).appendSegment(i - 1);
    ndn::Data data(name);
    const std::vector<uint8_t> content(8, static_cast<uint8_t>(i));
    data.setContent(ndn::span<const uint8_t>(content.data(), content.size()));
    fixture.keys.sign(data, ndn::security::signingByCertificate(certificate));
    const auto& wire = data.wireEncode();
    metadata.packetNames.push_back(name.toUri(ndn::name::UriFormat::ALTERNATE));
    fixture.packets.emplace_back(wire.begin(), wire.end());
    fixture.original.insert(fixture.original.end(), wire.begin(), wire.end());
    metadata.packetOffsets.push_back(fixture.original.size());
  }
  metadata.size = reference.expectedSize = fixture.original.size();
  metadata.sha256 = reference.expectedSha256 = sha256(fixture.original);
  const auto result = fixture.insert();
  BOOST_REQUIRE_MESSAGE(result.state == "DONE", result.message);
  const auto charged = fixture.usage().committedBytes;
  const auto began = std::chrono::steady_clock::now();
  bool exact = true;
  for (std::size_t i = 0; i < PACKET_COUNT; ++i) {
    const ndn::Interest interest(ndn::Name(metadata.packetNames[i]));
    exact = (fixture.node->getDataPacket(interest) == fixture.packets[i]) && exact;
  }
  const auto seconds = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - began).count();
  BOOST_TEST_MESSAGE("SPEC221_ORIGINAL_PACKET_READ packets=" << PACKET_COUNT
    << " wireBytes=" << fixture.original.size() << " wallSeconds=" << seconds);
  BOOST_CHECK(exact);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, charged);
  const auto proof = RepoCorePacketIndexTestAccess::measure(fixture.node->core());
  BOOST_CHECK_EQUAL(proof.first, 1U);
  BOOST_CHECK_LE(proof.second, 8U * 1024U * 1024U);
  const auto stored = fixture.node->getManifest(reference.objectName);
  for (unsigned mutation = 0; mutation < 3; ++mutation) {
    auto changed = stored;
    if (mutation == 0) ++changed.size;
    if (mutation == 1) changed.objectName += "/different";
    if (mutation == 2) changed.objectType = "ndn-data-wire";
    BOOST_CHECK_THROW(RepoCorePacketIndexTestAccess::validate(fixture.node->core(), changed),
                      std::invalid_argument);
  }
  auto changed = stored;
  changed.packetOffsets[500] = changed.packetOffsets[499];
  BOOST_REQUIRE_EQUAL(changed.toJson().size(), stored.toJson().size());
  const auto directory = fixture.temp.path() / "objects" / "manifests";
  const auto manifestFile = std::filesystem::directory_iterator(directory)->path();
  {
    std::ofstream output(manifestFile, std::ios::binary | std::ios::trunc);
    output << changed.toJson();
  }
  BOOST_CHECK_THROW(fixture.node->getDataPacket(ndn::Interest(ndn::Name(reference.objectName))),
                    std::invalid_argument);
  {
    std::ofstream output(manifestFile, std::ios::binary | std::ios::trunc);
    output << stored.toJson();
  }
  BOOST_CHECK(fixture.node->getDataPacket(ndn::Interest(ndn::Name(reference.objectName))) ==
              fixture.packets.front());
  BOOST_REQUIRE_EQUAL(fixture.erase("/owner/a", "large-reader-cleanup",
    fixture.request.storageBinding.ownerEpoch).state, "DELETED");
  BOOST_CHECK_THROW(fixture.node->getDataPacket(ndn::Interest(ndn::Name(reference.objectName))),
                    std::exception);
}

BOOST_AUTO_TEST_CASE(PacketIndexProofsHaveFixedEntryAndByteBounds)
{
  TempRoot temp;
  StorageCapability capability;
  capability.freeBytes = 1024 * 1024;
  RepoCore core(capability,
    makeFilesystemRepoStore((temp.path() / "objects").string(), 1024 * 1024, 1024));
  for (unsigned i = 0; i < 25; ++i) {
    RepoObjectManifest index;
    index.objectName = ndn::Name("/producer/index").appendNumber(i)
      .toUri(ndn::name::UriFormat::CANONICAL);
    index.objectType = "original-packet-set";
    index.size = 200;
    index.packetNames = {index.objectName, ndn::Name(index.objectName).appendSegment(0)
      .toUri(ndn::name::UriFormat::CANONICAL)};
    index.packetOffsets = {0, 100, 200};
    RepoCorePacketIndexTestAccess::validate(core, index);
    const auto proof = RepoCorePacketIndexTestAccess::measure(core);
    BOOST_CHECK_LE(proof.first, 16U);
    BOOST_CHECK_LE(proof.second, 8U * 1024U * 1024U);
  }
  const auto before = RepoCorePacketIndexTestAccess::measure(core);
  RepoObjectManifest large;
  large.objectType = "original-packet-set";
  large.packetOffsets = {0};
  for (unsigned i = 0; i < 2200; ++i) {
    large.packetNames.push_back(ndn::Name("/producer").append(std::string(4000, 'a'))
      .appendSegment(i).toUri(ndn::name::UriFormat::CANONICAL));
    large.packetOffsets.push_back(large.packetOffsets.back() + 5000);
  }
  large.objectName = large.packetNames.front();
  large.size = large.packetOffsets.back();
  RepoCorePacketIndexTestAccess::validate(core, large);
  BOOST_CHECK(RepoCorePacketIndexTestAccess::measure(core) == before);
}

BOOST_AUTO_TEST_CASE(OwnedManifestReadbackBindsAuthenticatedWriterGrantAndPhysicalPacketTuple)
{
  OwnedPacketFixture fixture;
  BOOST_REQUIRE_EQUAL(fixture.insert().state, "DONE");
  const auto query = encodeUserStorageInsertRequest(fixture.request);
  const auto stored = parseManifestJson(toString(fixture.node->handleManifest(query, "/provider/a")));
  BOOST_CHECK_EQUAL(stored.generation, fixture.request.generation);
  BOOST_CHECK_EQUAL(stored.sha256, fixture.request.reference.expectedSha256);
  BOOST_CHECK_THROW(fixture.node->handleManifest(query), std::runtime_error);
  BOOST_CHECK_THROW(fixture.node->handleManifest(query, "/owner/a"), std::runtime_error);
  auto changed = fixture.request;
  ++changed.generation;
  BOOST_CHECK_THROW(fixture.node->handleManifest(encodeUserStorageInsertRequest(changed),
    "/provider/a"), std::runtime_error);
  changed = fixture.request;
  changed.operationId += "-other";
  BOOST_CHECK_THROW(fixture.node->handleManifest(encodeUserStorageInsertRequest(changed),
    "/provider/a"), std::runtime_error);
  changed = fixture.request;
  changed.storageBinding.ownerIdentity = "/owner/b";
  BOOST_CHECK_THROW(fixture.node->handleManifest(encodeUserStorageInsertRequest(changed),
    "/provider/a"), std::exception);
  changed = fixture.request;
  auto& names = changed.reference.packetSetMetadata->packetNames;
  std::swap(names[1], names[2]);
  BOOST_CHECK_THROW(fixture.node->handleManifest(encodeUserStorageInsertRequest(changed),
    "/provider/a"), std::runtime_error);
  const auto charged = fixture.usage().committedBytes;
  const RepoUserStorageGrantRequest renewal{1, "renewed-writer-grant", "/provider/a",
    fixture.request.reference.objectName, fixture.now() + 60000};
  const auto renewed = RepoUserStorageGrantReply::fromJson(toString(
    fixture.node->handleUserStorageGrant(toBytes(renewal.toJson()), "/owner/a")));
  fixture.request.operationId = "renewed-writer-operation";
  fixture.request.storageBinding.grantId = renewal.grantId;
  fixture.request.storageBinding.ownerEpoch = renewed.grant.ownerEpoch;
  BOOST_REQUIRE_EQUAL(fixture.insert().state, "DONE");
  const auto renewedManifest = parseManifestJson(toString(fixture.node->handleManifest(
    encodeUserStorageInsertRequest(fixture.request), "/provider/a")));
  BOOST_CHECK_EQUAL(renewedManifest.operationId, stored.operationId);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, charged);
  BOOST_CHECK_EQUAL(fixture.erase("/owner/a", "manifest-cleanup",
    fixture.request.storageBinding.ownerEpoch).state, "DELETED");
  BOOST_CHECK_THROW(fixture.node->handleManifest(query, "/provider/a"), std::exception);
}

BOOST_AUTO_TEST_CASE(OwnedInsertRejectsSignatureAndWrongGrantBeforePhysicalCommit)
{
  OwnedPacketFixture fixture;
  fixture.packets.back().back() ^= 1;
  auto failed = fixture.insert();
  BOOST_CHECK_EQUAL(failed.state, "FAILED");
  BOOST_CHECK(failed.message.find("signature") != std::string::npos);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, 0U);
  BOOST_CHECK_EQUAL(fixture.usage().reservedBytes, 0U);
  BOOST_CHECK(fixture.node->core().list().empty());
  fixture.request.storageBinding.ownerIdentity = "/owner/b";
  const auto fetches = fixture.fetches;
  failed = fixture.insert();
  BOOST_CHECK_EQUAL(failed.state, "FAILED");
  BOOST_CHECK_EQUAL(fixture.fetches, fetches);
  BOOST_CHECK(fixture.node->core().list().empty());
}

BOOST_AUTO_TEST_CASE(OwnedInsertRechecksGrantAfterFetchingAndStopsBeforeMutation)
{
  OwnedPacketFixture fixture;
  const RepoUserStorageGrantRequest shortGrant{1, "short-grant", "/provider/a",
    fixture.request.reference.objectName, fixture.now() + 100};
  const auto granted = RepoUserStorageGrantReply::fromJson(toString(
    fixture.node->handleUserStorageGrant(toBytes(shortGrant.toJson()), "/owner/a")));
  fixture.request.storageBinding.grantId = shortGrant.grantId;
  fixture.request.storageBinding.ownerEpoch = granted.grant.ownerEpoch;
  fixture.node->setDataReferenceFetcher([&](const RepoDataReference&) {
    ++fixture.fetches;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return fixture.packets;
  });
  const auto failed = fixture.insert();
  BOOST_CHECK_EQUAL(failed.state, "FAILED");
  BOOST_CHECK_EQUAL(fixture.fetches, 1U);
  BOOST_CHECK_EQUAL(fixture.usage().reservedBytes, 0U);
  BOOST_CHECK(fixture.node->core().list().empty());
  fixture.node->stopOriginalPacketOperations();
  const auto stopped = fixture.insert();
  BOOST_CHECK_EQUAL(stopped.state, "FAILED");
  BOOST_CHECK(stopped.message.find("shutting-down") != std::string::npos);
  BOOST_CHECK_EQUAL(fixture.fetches, 1U);
}

BOOST_AUTO_TEST_CASE(OwnedInsertRespectsFundedQuotaBeforePublishingOriginalPackets)
{
  OwnedPacketFixture fixture;
  const RepoUserStorageGrantRequest fill{1, "fill-grant", "/provider/a",
    "/provider/a/funded-write", fixture.now() + 60000};
  const auto granted = RepoUserStorageGrantReply::fromJson(toString(
    fixture.node->handleUserStorageGrant(toBytes(fill.toJson()), "/owner/a")));
  SqliteArtifactMetadataStore journal((fixture.temp.path() / "metadata.sqlite3").string());
  journal.reconcileUserStorageInventory("/repo/a", {}, granted.grant.createdAtMs);
  // Seed a funded but unfinished write; no physical durability is claimed for it.
  const RepoUserStorageObject funded{fill.objectName, std::string(64, 'a'), 1,
    REPO_USER_STORAGE_LIMIT_BYTES - 128, 1};
  journal.reserveUserStorage("/provider/a", "/repo/a", "/owner/a", fill.grantId,
    "funded-write", funded, granted.grant.createdAtMs);
  const auto before = fixture.usage();
  const auto failed = fixture.insert();
  BOOST_CHECK_EQUAL(failed.state, "FAILED");
  BOOST_CHECK(failed.message.find("quota") != std::string::npos ||
    failed.message.find("QUOTA") != std::string::npos);
  BOOST_CHECK_EQUAL(fixture.usage().reservedBytes, before.reservedBytes);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, 0U);
  BOOST_CHECK(fixture.node->core().list().empty());
}

BOOST_AUTO_TEST_CASE(OwnedInsertRestartRecoversPhysicalCommitBeforeClaimPublication)
{
  OwnedPacketFixture fixture;
  auto manifest = *fixture.request.reference.packetSetMetadata;
  manifest.objectName = ndn::Name(manifest.objectName).toUri(ndn::name::UriFormat::CANONICAL);
  manifest.dataPrefix = ndn::Name("/provider/a").toUri(ndn::name::UriFormat::CANONICAL);
  manifest.generation = fixture.request.generation;
  manifest.operationId = fixture.request.operationId;
  manifest.segmentCount = fixture.packets.size();
  manifest.packetOffsets = {0};
  for (std::size_t i = 0; i < fixture.packets.size(); ++i) {
    manifest.packetNames[i] = ndn::Name(manifest.packetNames[i])
      .toUri(ndn::name::UriFormat::CANONICAL);
    manifest.packetOffsets.push_back(manifest.packetOffsets.back() + fixture.packets[i].size());
  }
  {
    SqliteArtifactMetadataStore journal((fixture.temp.path() / "metadata.sqlite3").string());
    journal.reconcileUserStorageInventory("/repo/a", {}, fixture.now());
    const RepoUserStorageObject reserved{fixture.request.reference.objectName,
      manifest.sha256, manifest.generation, manifest.size, manifest.toJson().size()};
    journal.reserveUserStorage("/provider/a", "/repo/a", "/owner/a",
      fixture.request.storageBinding.grantId, manifest.operationId, reserved, fixture.now());
    fixture.node->core().commitOriginalPacketReader(manifest,
      [&](uint64_t offset, std::size_t length) {
        return std::vector<uint8_t>(fixture.original.begin() + offset,
          fixture.original.begin() + offset + length);
      });
    BOOST_CHECK_THROW(fixture.node->getManifest(manifest.objectName), std::out_of_range);
    BOOST_CHECK_EQUAL(fixture.usage().committedBytes, 0U);
    BOOST_CHECK_GT(fixture.usage().reservedBytes, 0U);
  }
  fixture.provider.reset();
  fixture.node.reset();
  {
    DirectoryFsyncFailureScope failure;
    BOOST_CHECK_THROW(fixture.open(), std::exception);
  }
  {
    SqliteArtifactMetadataStore journal((fixture.temp.path() / "metadata.sqlite3").string());
    const auto usage = journal.userStorageUsage("/owner/a", "/repo/a");
    BOOST_CHECK_EQUAL(usage.committedBytes, 0U);
    BOOST_CHECK_GT(usage.reservedBytes, 0U);
  }
  fixture.open();
  BOOST_CHECK(fixture.node->getRange(manifest.objectName, {0, manifest.size}) == fixture.original);
  BOOST_CHECK_EQUAL(fixture.usage().reservedBytes, 0U);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes,
    manifest.size + fixture.node->getManifest(manifest.objectName).toJson().size());
}

BOOST_AUTO_TEST_CASE(OwnerCleanupRemovesCanonicalPacketIndexOnlyAfterDurableConfirmation)
{
  OwnedPacketFixture fixture;
  const auto stored = fixture.insert();
  BOOST_REQUIRE_MESSAGE(stored.state == "DONE", stored.message);
  const auto charged = fixture.usage().committedBytes;
  const auto epoch = fixture.request.storageBinding.ownerEpoch;
  BOOST_CHECK_THROW(fixture.erase("/owner/b", "foreign-cleanup", epoch), std::runtime_error);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, charged);
  const auto erased = fixture.erase("/owner/a", "cleanup", epoch);
  BOOST_CHECK_EQUAL(erased.state, "DELETED");
  BOOST_CHECK_EQUAL(erased.releasedBytes, charged);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, 0U);
  BOOST_CHECK(fixture.node->core().list().empty());
  BOOST_CHECK_EQUAL(fixture.erase("/owner/a", "cleanup", epoch).releasedBytes, 0U);
  const auto replay = fixture.insert();
  BOOST_CHECK_EQUAL(replay.state, "FAILED");
  BOOST_CHECK(fixture.node->core().list().empty());
}

BOOST_AUTO_TEST_CASE(OwnerCleanupPreservesAnotherOwnersIdenticalOriginalPackets)
{
  OwnedPacketFixture fixture;
  BOOST_REQUIRE_EQUAL(fixture.insert().state, "DONE");
  const auto epochA = fixture.request.storageBinding.ownerEpoch;
  const RepoUserStorageGrantRequest grant{1, "owner-b-grant", "/provider/a",
    fixture.request.reference.objectName, fixture.now() + 60000};
  const auto granted = RepoUserStorageGrantReply::fromJson(toString(
    fixture.node->handleUserStorageGrant(toBytes(grant.toJson()), "/owner/b")));
  fixture.request.storageBinding = {1, "/owner/b", grant.grantId, granted.grant.ownerEpoch};
  fixture.request.operationId = "owner-b-original";
  BOOST_REQUIRE_EQUAL(fixture.insert().state, "DONE");
  const auto root = fixture.request.reference.objectName;
  BOOST_CHECK_EQUAL(fixture.erase("/owner/a", "cleanup-a", epochA).state, "DELETED");
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, 0U);
  BOOST_CHECK(fixture.node->core().has(root));
  BOOST_CHECK(fixture.node->getRange(root, {0, fixture.original.size()}) == fixture.original);
  BOOST_CHECK_EQUAL(fixture.erase("/owner/b", "cleanup-b", granted.grant.ownerEpoch).state,
    "DELETED");
  BOOST_CHECK(fixture.node->core().list().empty());
}

BOOST_AUTO_TEST_CASE(OwnerCleanupRetainsChargeOnDirectoryFailureAndResumesAfterRestart)
{
  OwnedPacketFixture fixture;
  BOOST_REQUIRE_EQUAL(fixture.insert().state, "DONE");
  const auto charge = fixture.usage().committedBytes;
  {
    DirectoryFsyncFailureScope failure;
    BOOST_CHECK_THROW(fixture.erase("/owner/a", "restart-cleanup",
      fixture.request.storageBinding.ownerEpoch), std::runtime_error);
  }
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, charge);
  BOOST_CHECK_THROW(fixture.node->getManifest(fixture.request.reference.objectName),
    std::exception);
  fixture.provider.reset();
  fixture.node.reset();
  fixture.open();
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, 0U);
  BOOST_CHECK_EQUAL(fixture.usage().reservedBytes, 0U);
  BOOST_CHECK(fixture.node->core().list().empty());
}

BOOST_AUTO_TEST_CASE(OwnerCleanupLeavesUntrackedStagePendingAcrossRestart)
{
  OwnedPacketFixture fixture;
  BOOST_REQUIRE_EQUAL(fixture.insert().state, "DONE");
  const auto charge = fixture.usage().committedBytes;
  const auto stage = fixture.temp.path() / "objects" / "staging" /
    (fixture.request.reference.expectedSha256 + ".99.part");
  {
    std::ofstream output(stage);
    output << "untracked stage";
    BOOST_REQUIRE(output.good());
  }
  const auto epoch = fixture.request.storageBinding.ownerEpoch;
  const auto pending = fixture.erase("/owner/a", "stage-cleanup", epoch);
  BOOST_CHECK_EQUAL(pending.state, "PENDING");
  BOOST_CHECK_EQUAL(pending.releasedBytes, 0U);
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, charge);
  fixture.provider.reset();
  fixture.node.reset();
  fixture.open();
  BOOST_CHECK(std::filesystem::exists(stage));
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, charge);
  // This deliberately created fixture file has no production ownership proof.
  BOOST_REQUIRE(std::filesystem::remove(stage));
  const RepoUserStorageDeleteRequest cleanup{1, "stage-cleanup",
    fixture.request.reference.objectName, epoch};
  const auto erased = RepoUserStorageDeleteReply::fromJson(toString(
    fixture.node->handleUserStorageDelete(toBytes(cleanup.toJson()), "/owner/a")));
  BOOST_CHECK_EQUAL(erased.state, "DELETED");
  BOOST_CHECK_EQUAL(fixture.usage().committedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(PersistentOriginalReadUsesAcceptedHolderAndStandardInterestMatching)
{
  OwnedPacketFixture fixture;
  const auto keyName = fixture.request.reference.packetSetMetadata->packetNames.back();
  fixture.node->core().putDataPacket(keyName, fixture.packets.back());
  BOOST_REQUIRE_EQUAL(fixture.insert().state, "DONE");
  const auto& wire = fixture.packets.back();
  const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
  BOOST_CHECK(fixture.node->getDataPacket(ndn::Interest(packet.getFullName())) == wire);
  ndn::Name wrong = packet.getName();
  const std::vector<uint8_t> digest(32, 0);
  wrong.appendImplicitSha256Digest(ndn::span<const uint8_t>(digest.data(), digest.size()));
  BOOST_CHECK_THROW(fixture.node->getDataPacket(ndn::Interest(wrong)), std::runtime_error);
  ndn::Interest prefix(ndn::Name("/provider/a/kv"));
  prefix.setCanBePrefix(true);
  BOOST_CHECK(fixture.node->getDataPacket(prefix) == fixture.packets[1]);
  ndn::Interest directory(ndn::Name("/provider/a"));
  directory.setCanBePrefix(true);
  BOOST_CHECK_THROW(fixture.node->getDataPacket(directory), std::runtime_error);
  BOOST_CHECK_EQUAL(fixture.erase("/owner/a", "reader-cleanup",
    fixture.request.storageBinding.ownerEpoch).state, "DELETED");
  BOOST_CHECK(fixture.node->core().hasDataPacket(keyName));
  BOOST_CHECK_THROW(fixture.node->getDataPacket(ndn::Interest(ndn::Name(keyName))),
    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(PersistentOriginalFaceServesExactWireAndBoundsPendingWorkThroughStop)
{
  // The serving Face must outlive Node's worker and queued publications.
  ndn::DummyClientFace face(ndn::DummyClientFace::Options{true, true});
  OwnedPacketFixture fixture;
  BOOST_REQUIRE_EQUAL(fixture.insert().state, "DONE");
  const auto pumpUntil = [&](const std::function<bool()>& complete) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
      face.getIoContext().restart();
      (void)face.getIoContext().poll();
      if (complete()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return complete();
  };
  fixture.node->registerOriginalDataServing(face);
  BOOST_REQUIRE(pumpUntil([&] { return fixture.node->isOriginalDataServingReady(); }));
  const auto pending = RepoNodeTestAccess::pending(*fixture.node);
  auto received = std::make_shared<std::vector<std::vector<uint8_t>>>();
  auto connection = face.onSendData.connect([received](const ndn::Data& data) {
    const auto encoded = data.wireEncode();
    received->emplace_back(encoded.begin(), encoded.end());
  });
  ndn::Interest interest(ndn::Name(
    fixture.request.reference.packetSetMetadata->packetNames.back()));
  interest.setCanBePrefix(false);
  face.receive(interest);
  BOOST_REQUIRE(pumpUntil([&] { return received->size() == 1 && pending() == 0; }));
  BOOST_CHECK(received->front() == fixture.packets.back());
  BOOST_CHECK_EQUAL(fixture.erase("/owner/a", "face-cleanup",
    fixture.request.storageBinding.ownerEpoch).state, "DELETED");
  received->clear();
  face.receive(interest);
  BOOST_REQUIRE(pumpUntil([&] { return pending() == 0; }));
  BOOST_CHECK(received->empty());

  auto writeGuard = RepoNodeTestAccess::holdWrites(*fixture.node);
  for (unsigned i = 0; i < 65; ++i) {
    RepoNodeTestAccess::enqueue(*fixture.node, interest);
  }
  BOOST_CHECK_EQUAL(pending(), 64U);
  fixture.node->stopOriginalPacketOperations();
  BOOST_CHECK_LE(pending(), 1U);
  writeGuard.unlock();
  BOOST_REQUIRE(pumpUntil([&] { return pending() == 0; }));
  BOOST_CHECK(!fixture.node->isOriginalDataServingReady());
  fixture.provider.reset();
  fixture.node.reset();
  BOOST_CHECK(pumpUntil([&] { return pending() == 0; }));
  BOOST_CHECK(received->empty());
}

BOOST_AUTO_TEST_CASE(SelectedRepoRequestCancellationAbandonsQueuedDispatchBeforeItsDeadline)
{
  ndn::DummyClientFace face(ndn::DummyClientFace::Options{true, true});
  TempRoot temp;
  ndn::KeyChain keys("pib-memory:spec221-selected-cancel", "tpm-memory:spec221-selected-cancel");
  const auto owner = makeRsaIdentity(keys, ndn::Name("/owner/a"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/authority"));
  ndn_service_framework::ServiceUser user(
    ndn_service_framework::ServiceUser::LocalMockTag{}, face, ndn::Name("/group"),
    owner, authority, writeFixedKeychainTrust(temp.path(), keys));
  std::atomic<unsigned> probes{0};
  std::atomic<bool> closed{false};
  const auto emitted = std::make_shared<std::atomic<bool>>(false);
  const auto began = std::chrono::steady_clock::now();
  auto result = std::async(std::launch::async, [&] {
    return RepoClient::requestSelectedService(user, ndn::Name("/repo/provider"),
      ndn::Name("/SERVICE/repo/INSERT"), {1}, 10, 5000, emitted, [&] {
        ++probes;
        if (closed.load())
          throw std::runtime_error("test-source-closed");
      });
  });
  // Leave the Face paused so dispatch cannot complete or supply a response.
  const auto probeDeadline = began + std::chrono::seconds(1);
  while (probes.load() < 2 && std::chrono::steady_clock::now() < probeDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  BOOST_REQUIRE_GE(probes.load(), 2U);
  closed.store(true);
  BOOST_REQUIRE(result.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
  BOOST_CHECK_EXCEPTION(result.get(), std::runtime_error, [] (const auto& error) {
    return std::string(error.what()) == "test-source-closed";
  });
  BOOST_CHECK(!emitted->load());
  // A late start callback must observe abandonment and cannot publish Selection.
  face.getIoContext().restart();
  face.getIoContext().poll();
  BOOST_CHECK(!emitted->load());
  BOOST_TEST_MESSAGE("SPEC221_SELECTED_RPC_CANCEL wallSeconds=" <<
    std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
}

BOOST_AUTO_TEST_CASE(OwnerPacketClientUsesConfiguredFaceAndChecksWireSignerSizeAndDeadline)
{
  ndn::DummyClientFace ownerFace(ndn::DummyClientFace::Options{true, true});
  ndn::DummyClientFace repoFace(ndn::DummyClientFace::Options{true, true});
  OwnedPacketFixture fixture;
  BOOST_REQUIRE_EQUAL(fixture.insert().state, "DONE");
  const auto owner = makeRsaIdentity(fixture.keys, ndn::Name("/owner/a"));
  const auto authority = fixture.keys.getPib().getIdentity(ndn::Name("/authority"))
    .getDefaultKey().getDefaultCertificate();
  const auto producer = fixture.keys.getPib().getIdentity(ndn::Name("/provider/a"))
    .getDefaultKey().getDefaultCertificate();
  ndn_service_framework::ServiceUser user(
    ndn_service_framework::ServiceUser::LocalMockTag{}, ownerFace, ndn::Name("/group"),
    owner, authority, writeFixedKeychainTrust(fixture.temp.path(), fixture.keys));
  const RepoArtifactTarget target{"/repo/a", "/repo/provider", "/SERVICE/repo"};
  const ndn::Name name(fixture.request.reference.packetSetMetadata->packetNames.front());
  fixture.node->registerOriginalDataServing(repoFace);
  auto interestLink = ownerFace.onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getName() == name) {
      BOOST_CHECK(!interest.getCanBePrefix());
      BOOST_REQUIRE_EQUAL(interest.getForwardingHint().size(), 1U);
      BOOST_CHECK(interest.getForwardingHint().front() == ndn::Name(target.providerIdentity));
      repoFace.receive(interest);
    }
  });
  auto dataLink = repoFace.onSendData.connect([&](const ndn::Data& data) {
    ownerFace.receive(data);
  });
  const auto pump = [&](const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
      ownerFace.getIoContext().restart();
      ownerFace.getIoContext().poll();
      repoFace.getIoContext().restart();
      repoFace.getIoContext().poll();
      if (done()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return done();
  };
  BOOST_REQUIRE(pump([&] { return fixture.node->isOriginalDataServingReady(); }));
  const auto verify = [producer](const ndn::Data& data) {
    return ndn::security::verifySignature(data, producer);
  };
  const auto run = [&](uint64_t size, const std::function<bool(const ndn::Data&)>& validator,
                       int timeoutMs = 1000) {
    auto result = std::async(std::launch::async, [&] {
      return RepoClient::fetchOriginalDataPacketFromUserStorage(
        user, target, name, size, validator, timeoutMs);
    });
    BOOST_REQUIRE(pump([&] { return result.wait_for(std::chrono::milliseconds(0)) ==
      std::future_status::ready; }));
    return result.get();
  };
  BOOST_CHECK(run(fixture.packets.front().size(), verify) == fixture.packets.front());
  BOOST_CHECK_THROW(run(fixture.packets.front().size() + 1, verify), std::runtime_error);
  BOOST_CHECK_THROW(run(fixture.packets.front().size(),
    [](const ndn::Data&) { return false; }), std::runtime_error);
  BOOST_CHECK_THROW(run(fixture.packets.front().size(), [verify](const ndn::Data& data) {
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    return verify(data);
  }, 30), std::runtime_error);

  bool rejectedOnFace = false;
  user.postToIo([&] {
    BOOST_CHECK_THROW(RepoClient::fetchOriginalDataPacketFromUserStorage(
      user, target, name, fixture.packets.front().size(), verify, 100), std::logic_error);
    rejectedOnFace = true;
  });
  BOOST_REQUIRE(pump([&] { return rejectedOnFace; }));
}

BOOST_AUTO_TEST_CASE(OwnerPacketTimeoutKeepsAdmissionUntilPausedFaceCallbacksDrain)
{
  ndn::DummyClientFace face(ndn::DummyClientFace::Options{true, true});
  TempRoot temp;
  ndn::KeyChain keys("pib-memory:spec221-client", "tpm-memory:spec221-client");
  const auto owner = makeRsaIdentity(keys, ndn::Name("/owner/a"));
  const auto authority = makeRsaIdentity(keys, ndn::Name("/authority"));
  auto user = std::make_unique<ndn_service_framework::ServiceUser>(
    ndn_service_framework::ServiceUser::LocalMockTag{}, face, ndn::Name("/group"),
    owner, authority, writeFixedKeychainTrust(temp.path(), keys));
  const RepoArtifactTarget target{"/repo/a", "/repo/provider", "/SERVICE/repo"};
  for (size_t i = 0; i < 16; ++i) {
    BOOST_CHECK_THROW(RepoClient::fetchOriginalDataPacketFromUserStorage(
      *user, target, ndn::Name("/provider/a/missing"), 100,
      [](const ndn::Data&) { return true; }, 3), std::runtime_error);
  }
  BOOST_CHECK_EXCEPTION(RepoClient::fetchOriginalDataPacketFromUserStorage(
    *user, target, ndn::Name("/provider/a/missing"), 100,
    [](const ndn::Data&) { return true; }, 3), std::runtime_error,
    [](const std::runtime_error& error) {
      return std::string(error.what()) == "service-user-face-operation-capacity";
    });
  face.getIoContext().restart();
  face.getIoContext().poll();
  // An expired queued start must not emit a late original Interest.
  BOOST_CHECK(std::none_of(face.sentInterests.begin(), face.sentInterests.end(),
    [](const ndn::Interest& interest) { return interest.getName() ==
      ndn::Name("/provider/a/missing"); }));
  std::weak_ptr<void> lease;
  {
    auto admitted = user->postToIoWithFace([](ndn::Face&) {});
    lease = admitted;
  }
  BOOST_CHECK(!lease.expired());
  user.reset();
  face.getIoContext().restart();
  face.getIoContext().poll();
  BOOST_CHECK(lease.expired());
}

BOOST_AUTO_TEST_CASE(SourcePublicationAdmissionPrecedesPacketGenerationAndSpooling)
{
  ndn::DummyClientFace face(ndn::DummyClientFace::Options{true, true});
  OwnedPacketFixture fixture;
  const auto source = fixture.keys.getPib().getIdentity(ndn::Name("/provider/a"))
    .getDefaultKey().getDefaultCertificate();
  const auto authority = fixture.keys.getPib().getIdentity(ndn::Name("/authority"))
    .getDefaultKey().getDefaultCertificate();
  ndn_service_framework::ServiceUser writer(
    ndn_service_framework::ServiceUser::LocalMockTag{}, face, ndn::Name("/group"),
    source, authority, writeFixedKeychainTrust(fixture.temp.path(), fixture.keys));
  std::vector<std::shared_ptr<void>> leases;
  for (size_t i = 0; i < 16; ++i) {
    leases.push_back(writer.postToIoWithFace([](ndn::Face&) {}));
  }
  const RepoArtifactTarget target{"/repo/a", "/repo/provider", "/SERVICE/repo"};
  size_t produced = 0;
  BOOST_CHECK_EXCEPTION(RepoClient::commitUserStorageOriginalPackets(writer, target,
    fixture.request, [&]() -> std::optional<std::vector<uint8_t>> {
      if (produced == fixture.packets.size()) return std::nullopt;
      return fixture.packets[produced++];
    }, [source](const ndn::Data& data) {
      return ndn::security::verifySignature(data, source);
    }, 10, 1000), std::runtime_error, [](const std::runtime_error& error) {
      return std::string(error.what()) == "service-user-face-operation-capacity";
    });
  BOOST_CHECK_EQUAL(produced, 0U);
  leases.clear();
  // Draining queued leases restores capacity for a later publication.
  face.getIoContext().restart();
  face.getIoContext().poll();
  auto fresh = writer.postToIoWithFace([](ndn::Face&) {});
  BOOST_CHECK(fresh);
  face.getIoContext().restart();
  face.getIoContext().poll();
}

BOOST_AUTO_TEST_CASE(OwnerRecoveryPolicyPreservesAndChargesUnconfirmedFilesBeforeNodeStartup)
{
  TempRoot temp;
  const auto root = temp.path() / "policy";
  const auto stage = root / "staging" / (std::string(64, 'a') + ".9.part");
  const auto payload = root / "payloads" / "sha256" / "aa" / std::string(64, 'a');
  {
    auto backend = makeFilesystemRepoStore(root.string(), 4096, 1024, "policy",
      FilesystemRecoveryPolicy::KEEP_UNCONFIRMED);
    BOOST_CHECK(backend->supportsUserStorageRecovery());
    std::filesystem::create_directories(payload.parent_path());
    std::ofstream(stage) << "stage";
    std::ofstream(payload) << "orphan";
  }
  {
    auto backend = makeFilesystemRepoStore(root.string(), 4096, 1024, "policy",
      FilesystemRecoveryPolicy::KEEP_UNCONFIRMED);
    BOOST_CHECK(std::filesystem::exists(stage));
    BOOST_CHECK(std::filesystem::exists(payload));
    BOOST_CHECK_EQUAL(backend->usedBytes(), 11U);
    auto tiered = makeTieredRepoStore(backend, 4096, "filesystem", 1024);
    BOOST_CHECK(tiered->supportsUserStorageRecovery());
  }
  {
    auto legacy = makeFilesystemRepoStore(root.string(), 4096, 1024, "policy");
    BOOST_CHECK(!legacy->supportsUserStorageRecovery());
    BOOST_CHECK(!std::filesystem::exists(stage));
    BOOST_CHECK(!std::filesystem::exists(payload));
    BOOST_CHECK_EQUAL(legacy->usedBytes(), 0U);
  }
}

BOOST_AUTO_TEST_CASE(PhysicalNameNormalizationDoesNotRelaxOwnerJournalOrWire)
{
  TempRoot temp;
  const std::string logical = "/provider/a/kv/manifest";
  const RepoUserStorageObject physical{
    ndn::Name(logical).toUri(ndn::name::UriFormat::CANONICAL), std::string(64, 'a'), 1, 3, 1};
  BOOST_CHECK_NO_THROW(validateRepoUserStorageObject(physical));
  SqliteArtifactMetadataStore journal((temp.path() / "metadata.sqlite3").string());
  const auto grant = journal.grantUserStorage("/owner/a", "/repo/a", "grant", "/provider/a",
    logical, 1000, 100);
  journal.reconcileUserStorageInventory("/repo/a", {}, 100);
  BOOST_CHECK_THROW(journal.reserveUserStorage("/provider/a", "/repo/a", "/owner/a",
    grant.grantId, "write", physical, 101), std::invalid_argument);
  RepoUserStorageClaim claim;
  claim.repoNode = "/repo/a";
  claim.ownerIdentity = "/owner/a";
  claim.grantId = grant.grantId;
  claim.ownerEpoch = grant.ownerEpoch;
  claim.object = physical;
  claim.committedAtMs = 101;
  RepoUserStorageListReply reply{1, "/repo/a", "/owner/a", {claim}, {}};
  BOOST_CHECK_THROW(reply.toJson(), std::invalid_argument);
  reply.objects.front().object.objectName = logical;
  BOOST_CHECK_NO_THROW(reply.toJson());
}

BOOST_AUTO_TEST_CASE(OwnedInsertRejectsBackendWithoutPreservingStartupRecovery)
{
  OwnedPacketFixture fixture(FilesystemRecoveryPolicy::RECLAIM_ORPHANS);
  const auto failed = fixture.insert();
  BOOST_CHECK_EQUAL(failed.state, "FAILED");
  BOOST_CHECK_EQUAL(failed.message, "repo-user-storage-recovery-unsupported");
  BOOST_CHECK_EQUAL(fixture.fetches, 0U);
  BOOST_CHECK(fixture.node->core().list().empty());
  BOOST_CHECK_EQUAL(fixture.usage().reservedBytes, 0U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::tests
