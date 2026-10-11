#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"
#include "ndnsf-distributed-repo/ArtifactTypes.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <openssl/sha.h>
#include <sqlite3.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <thread>
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

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::tests
