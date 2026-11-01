#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackendTestAccess.hpp"
#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>
#include <openssl/sha.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <cerrno>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <iterator>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace ndnsf_distributed_repo::tests {

BOOST_AUTO_TEST_SUITE(DistributedRepoArtifactReceipt)

namespace {

class TempRoot
{
public:
  TempRoot()
  {
    static std::atomic<uint64_t> sequence{0};
    m_path = std::filesystem::temp_directory_path() /
      ("spec192-artifact-receipt-" + std::to_string(getpid()) + "-" +
       std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(m_path);
  }

  ~TempRoot()
  {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  const std::filesystem::path& path() const noexcept { return m_path; }

private:
  std::filesystem::path m_path;
};

std::string
digest(const std::vector<uint8_t>& bytes)
{
  unsigned char output[SHA256_DIGEST_LENGTH]{};
  SHA256(bytes.data(), bytes.size(), output);
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.reserve(SHA256_DIGEST_LENGTH * 2);
  for (const auto value : output) {
    result.push_back(HEX[value >> 4]);
    result.push_back(HEX[value & 0x0f]);
  }
  return result;
}

ArtifactReference
makeReference(const std::string& name, const std::vector<uint8_t>& bytes)
{
  ArtifactReference reference;
  reference.logicalName = "/spec192/user/model/" + name;
  reference.contentDigest = digest(bytes);
  reference.sizeBytes = bytes.size();
  reference.rootManifestName = "/spec192/user/manifest/" + name;
  reference.publisherIdentity = "/spec192/user";
  reference.policyEpoch = "repo-policy-v1";
  return reference.canonicalized();
}

ndn::security::Certificate
makeRsaIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  auto key = keyChain.createIdentity(identity, ndn::RsaKeyParams(2048));
  return key.getDefaultKey().getDefaultCertificate();
}

ndn_service_framework::ResponseMessage
dispatch(ndn_service_framework::ServiceProvider& provider,
         const ndn::Name& service, const std::vector<uint8_t>& payload,
         const std::string& requestId)
{
  ndn_service_framework::RequestMessage request;
  ndn::Buffer buffer;
  if (!payload.empty()) buffer = ndn::Buffer(payload.data(), payload.size());
  request.setPayload(buffer, buffer.size());
  return provider.dispatchRequest(
    ndn::Name("/spec192/user"), provider.getName(), service,
    ndn::Name("/spec192/request").append(requestId), request);
}

std::string
responseString(const ndn_service_framework::ResponseMessage& response);

std::string
reservePublishOperation(ndn_service_framework::ServiceProvider& provider,
                        const ndn::Name& storeService,
                        const RepoArtifactTarget& seed,
                        const ArtifactReference& reference,
                        uint64_t generation,
                        const std::string& idempotencyKey,
                        const RepoUserStorageBinding& storageBinding,
                        const std::vector<RepoArtifactTarget>& targets,
                        const std::string& requestId)
{
  PublishOperationIntent intent;
  intent.schemaVersion = 2;
  intent.idempotencyKeyDigest = digestPublishIdempotencyKey(idempotencyKey);
  intent.reference = reference;
  intent.generation = generation;
  intent.seed = seed;
  intent.consistency = "ONE";
  intent.targets = targets;
  intent.userStorageBindings = {{1, seed, storageBinding, "/spec192/user"}};
  intent.requestDigest = makePublishRequestDigest(
    reference, generation, seed, intent.consistency, targets,
    intent.userStorageBindings);
  const auto expectedOperationId = makePublishOperationId(
    "/spec192/user", seed, intent.idempotencyKeyDigest);
  const auto wire = intent.toJson();
  const std::vector<uint8_t> payload(wire.begin(), wire.end());
  const auto response = dispatch(provider, storeService, payload, requestId);
  if (!response.getStatus() || responseString(response) != expectedOperationId) {
    BOOST_ERROR("PUBLISH_INTENT reservation failed: " + response.getErrorInfo());
    return {};
  }
  return expectedOperationId;
}

std::vector<uint8_t>
responseBytes(const ndn_service_framework::ResponseMessage& response)
{
  const auto payload = response.getPayload();
  return std::vector<uint8_t>(payload.begin(), payload.end());
}

std::string
responseString(const ndn_service_framework::ResponseMessage& response)
{
  const auto payload = responseBytes(response);
  return std::string(payload.begin(), payload.end());
}

ArtifactReplicaCommit
committedReplica(const ArtifactReference& reference, const std::string& repo,
                 const std::string& provider, const std::string& receipt,
                 const std::string& operation)
{
  ArtifactReplicaCommit replica;
  replica.repoNode = repo;
  replica.providerIdentity = provider;
  replica.receiptId = receipt;
  replica.operationId = operation;
  replica.reference = reference;
  replica.generation = 1;
  replica.contentDigest = reference.contentDigest;
  replica.persistedBytes = reference.sizeBytes;
  replica.completedAtMs = 100;
  replica.locationVersion = 1;
  replica.routeState = "ROUTABLE";
  replica.state = "COMMITTED";
  replica.forwardingHint = "/NDNSF/DistributedRepo/Object/v1";
  return replica;
}

std::string
persistentReceiptId(const ArtifactCommitReceipt& receipt)
{
  std::string material;
  const auto appendNumber = [&] (uint64_t number) {
    for (size_t index = 0; index < 8; ++index) {
      const auto shift = static_cast<unsigned>((7 - index) * 8);
      material.push_back(static_cast<char>((number >> shift) & 0xff));
    }
  };
  const auto appendString = [&] (const std::string& value) {
    appendNumber(static_cast<uint64_t>(value.size()));
    material.append(value);
  };
  static constexpr char DOMAIN[] =
    "ndnsf-distributed-repo:persistent-receipt:v1";
  material.append(DOMAIN, sizeof(DOMAIN));
  appendNumber(receipt.schemaVersion);
  appendString(receipt.operationId);
  appendString(receipt.reference.toJson());
  appendNumber(receipt.generation);
  appendString(receipt.lifecycleMode);
  appendString(receipt.placementState);
  appendNumber(receipt.requestedReplicas);
  appendNumber(receipt.requiredAcks);
  appendNumber(receipt.achievedReplicas);
  appendString(receipt.writeConsistency);
  appendString(receipt.state);
  appendNumber(receipt.completedAtMs);
  appendNumber(static_cast<uint64_t>(receipt.replicaCommits.size()));
  for (const auto& replica : receipt.replicaCommits) {
    appendString(replica.repoNode);
    appendString(replica.providerIdentity);
    appendString(replica.receiptId);
    appendString(replica.operationId);
    appendString(replica.reference.toJson());
    appendNumber(replica.generation);
    appendString(replica.contentDigest);
    appendNumber(replica.persistedBytes);
    appendNumber(replica.completedAtMs);
    appendNumber(replica.locationVersion);
    appendString(replica.routeState);
    appendString(replica.state);
    appendString(replica.failureReason);
    appendString(replica.forwardingHint);
  }
  std::vector<uint8_t> bytes(material.begin(), material.end());
  return "artifact-commit-" + digest(bytes);
}

void
checkPersistentReceiptRow(const std::string& databasePath,
                          const ArtifactLocationRecord& location)
{
  sqlite3* rawDatabase = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_open_v2(databasePath.c_str(), &rawDatabase,
                                      SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(rawDatabase, sqlite3_close);
  const char* sql =
    "SELECT operation_id,repo_node,provider_identity,logical_name,digest_algorithm,"
    "content_digest,size_bytes,format_version,root_manifest_name,publisher_identity,"
    "policy_epoch,generation,persisted_bytes,state FROM artifact_persistent_receipts "
    "WHERE receipt_id=?";
  sqlite3_stmt* rawStatement = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_prepare_v2(database.get(), sql, -1, &rawStatement, nullptr), SQLITE_OK);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  BOOST_REQUIRE_EQUAL(sqlite3_bind_text(statement.get(), 1, location.receiptId.c_str(), -1,
                                        SQLITE_TRANSIENT), SQLITE_OK);
  BOOST_REQUIRE_EQUAL(sqlite3_step(statement.get()), SQLITE_ROW);
  const auto& reference = location.reference;
  const auto textColumn = [&] (int column) {
    const auto* value = sqlite3_column_text(statement.get(), column);
    return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
  };
  BOOST_CHECK_EQUAL(textColumn(0), location.operationId);
  BOOST_CHECK_EQUAL(textColumn(1), location.repoNode);
  BOOST_CHECK_EQUAL(textColumn(2), location.providerIdentity);
  BOOST_CHECK_EQUAL(textColumn(3), reference.logicalName);
  BOOST_CHECK_EQUAL(textColumn(4), reference.digestAlgorithm);
  BOOST_CHECK_EQUAL(textColumn(5), reference.contentDigest);
  BOOST_CHECK_EQUAL(sqlite3_column_int64(statement.get(), 6), reference.sizeBytes);
  BOOST_CHECK_EQUAL(textColumn(7), reference.formatVersion);
  BOOST_CHECK_EQUAL(textColumn(8), reference.rootManifestName);
  BOOST_CHECK_EQUAL(textColumn(9), reference.publisherIdentity);
  BOOST_CHECK_EQUAL(textColumn(10), reference.policyEpoch);
  BOOST_CHECK_EQUAL(sqlite3_column_int64(statement.get(), 11), location.generation);
  BOOST_CHECK_EQUAL(sqlite3_column_int64(statement.get(), 12), reference.sizeBytes);
  BOOST_CHECK_EQUAL(textColumn(13), "COMMITTED");
  BOOST_CHECK_EQUAL(sqlite3_step(statement.get()), SQLITE_DONE);
}

size_t
countPersistentReceipts(const std::string& databasePath,
                        const ArtifactReference& reference)
{
  sqlite3* rawDatabase = nullptr;
  if (sqlite3_open_v2(databasePath.c_str(), &rawDatabase,
                      SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (rawDatabase != nullptr) sqlite3_close(rawDatabase);
    throw std::runtime_error("cannot open Persistent receipt database for counting");
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(rawDatabase, sqlite3_close);
  sqlite3_stmt* rawStatement = nullptr;
  const char* sql =
    "SELECT COUNT(*) FROM artifact_persistent_receipts WHERE logical_name=? "
    "AND digest_algorithm=? AND content_digest=? AND size_bytes=? "
    "AND format_version=? AND root_manifest_name=? AND publisher_identity=? "
    "AND policy_epoch=?";
  if (sqlite3_prepare_v2(database.get(), sql, -1, &rawStatement, nullptr) != SQLITE_OK) {
    throw std::runtime_error("cannot prepare Persistent receipt count query");
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement,
                                                                       sqlite3_finalize);
  const auto bindTextValue = [&] (int index, const std::string& value) {
    if (sqlite3_bind_text(statement.get(), index, value.c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throw std::runtime_error("cannot bind Persistent receipt count query");
    }
  };
  bindTextValue(1, reference.logicalName);
  bindTextValue(2, reference.digestAlgorithm);
  bindTextValue(3, reference.contentDigest);
  if (sqlite3_bind_int64(statement.get(), 4,
                         static_cast<sqlite3_int64>(reference.sizeBytes)) != SQLITE_OK) {
    throw std::runtime_error("cannot bind Persistent receipt size");
  }
  bindTextValue(5, reference.formatVersion);
  bindTextValue(6, reference.rootManifestName);
  bindTextValue(7, reference.publisherIdentity);
  bindTextValue(8, reference.policyEpoch);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error("cannot read Persistent receipt count");
  }
  return static_cast<size_t>(sqlite3_column_int64(statement.get(), 0));
}

void
replaceCurrentLocationReceiptId(const std::string& databasePath,
                                const std::string& operationId,
                                const std::string& receiptId)
{
  sqlite3* rawDatabase = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_open_v2(databasePath.c_str(), &rawDatabase,
                                      SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(rawDatabase, sqlite3_close);
  sqlite3_stmt* rawStatement = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_prepare_v2(database.get(),
    "UPDATE artifact_locations SET receipt_id=? WHERE operation_id=?",
    -1, &rawStatement, nullptr), SQLITE_OK);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
    rawStatement, sqlite3_finalize);
  BOOST_REQUIRE_EQUAL(sqlite3_bind_text(statement.get(), 1, receiptId.c_str(), -1,
                                        SQLITE_TRANSIENT), SQLITE_OK);
  BOOST_REQUIRE_EQUAL(sqlite3_bind_text(statement.get(), 2, operationId.c_str(), -1,
                                        SQLITE_TRANSIENT), SQLITE_OK);
  BOOST_REQUIRE_EQUAL(sqlite3_step(statement.get()), SQLITE_DONE);
  BOOST_CHECK_EQUAL(sqlite3_changes(database.get()), 1);
}

} // namespace

BOOST_AUTO_TEST_CASE(PublishOperationDigestsMatchKnownAnswers)
{
  const std::string key = "spec192-known-key";
  const auto keyDigest = digestPublishIdempotencyKey(key);
  BOOST_CHECK_EQUAL(
    keyDigest,
    "d4c47c2e7196d0c8d2b6cf3db760d77c65831e43868322737cbae42d9d2f5f3e");

  const RepoArtifactTarget seed{
    "/logical/seed", "/provider/seed", "/NDNSF/DistributedRepo/Object/v1"};
  const RepoArtifactTarget replica{
    "/logical/replica", "/provider/replica", "/NDNSF/DistributedRepo/Object/v1"};
  BOOST_CHECK_EQUAL(
    makePublishOperationId("/spec192/user", seed, keyDigest),
    "a0aa24e8f9edd06bd0f7dc82e2f1b3bc3fdf3308b7bca05174c8586e0b7a326e");

  auto reference = makeReference("known-answer", {'A', 'B', 'C'});
  // Fixed legacy digest transcript: do not change its URI spelling or identity.
  reference.logicalName = "/known-answer";
  reference.rootManifestName = "/spec192/artifacts/known-answer";
  reference.publisherIdentity = "/spec192/publisher";
  const std::vector<RepoArtifactTarget> reversedTargets{seed, replica};
  const std::vector<RepoArtifactTarget> sortedTargets{replica, seed};
  const auto reversedDigest = makePublishRequestDigest(
    reference, 1, seed, "QUORUM", reversedTargets);
  BOOST_CHECK_EQUAL(
    reversedDigest,
    "8b21d1640a47a6654db1fa904f42fb88c27c051b838c189344b99b955e904258");
  BOOST_CHECK_EQUAL(reversedDigest,
                    makePublishRequestDigest(reference, 1, seed, "QUORUM",
                                             sortedTargets));

  PublishOperationIntent intent;
  intent.idempotencyKeyDigest = keyDigest;
  intent.requestDigest = reversedDigest;
  intent.reference = reference;
  intent.generation = 1;
  intent.seed = seed;
  intent.consistency = "QUORUM";
  intent.targets = reversedTargets;
  const auto wire = intent.toJson();
  BOOST_CHECK_EQUAL(wire.find(key), std::string::npos);
  BOOST_CHECK_EQUAL(PublishOperationIntent::fromJson(wire).toJson(), wire);
}

BOOST_AUTO_TEST_CASE(PublishOperationStatusRequestOptionallyBindsExactTarget)
{
  const RepoArtifactTarget target{
    "/logical/replica", "/provider/replica",
    "/NDNSF/DistributedRepo/Object/v1"};
  PublishOperationStatusRequest request;
  request.operationId = std::string(64, 'a');
  request.target = target;

  const auto wire = request.toJson();
  const auto decoded = PublishOperationStatusRequest::fromJson(wire);
  BOOST_REQUIRE(decoded.target);
  BOOST_CHECK_EQUAL(decoded.operationId, request.operationId);
  BOOST_CHECK(*decoded.target == target);

  const PublishOperationStatusRequest legacy{1, request.operationId};
  const auto legacyDecoded = PublishOperationStatusRequest::fromJson(legacy.toJson());
  BOOST_CHECK(!legacyDecoded.target);
  BOOST_CHECK_EQUAL(legacyDecoded.operationId, request.operationId);

  auto extraField = wire;
  extraField.insert(extraField.size() - 1, ",\"unexpected\":true");
  BOOST_CHECK_THROW(PublishOperationStatusRequest::fromJson(extraField),
                    std::exception);

  auto nonCanonicalTarget = request;
  nonCanonicalTarget.target->servicePrefix = "//NDNSF/DistributedRepo/Object/v1";
  BOOST_CHECK_THROW(nonCanonicalTarget.toJson(), std::exception);
}

BOOST_AUTO_TEST_CASE(PublishIntentRejectsAnUnrepresentableTerminalStatus)
{
  const auto quote = [] (const std::string& value) {
    return "\"" + value + "\"";
  };
  auto reference = makeReference("status-size", {'A'});
  reference.logicalName = "/" + std::string(4095, 'a');
  reference.rootManifestName = "/" + std::string(4095, 'b');
  reference.publisherIdentity = "/" + std::string(4095, 'c');

  std::vector<RepoArtifactTarget> targets;
  for (size_t i = 0; i < 90; ++i) {
    targets.push_back({"/repo/" + std::to_string(i),
                       "/provider/" + std::to_string(i),
                       "/NDNSF/DistributedRepo/Object/v1"});
  }
  const auto seed = targets.front();
  const auto requestDigest = makePublishRequestDigest(
    reference, 1, seed, "ALL", targets);

  const auto referenceWire = "{\"logicalName\":" + quote(reference.logicalName) +
    ",\"digestAlgorithm\":" + quote(reference.digestAlgorithm) +
    ",\"contentDigest\":" + quote(reference.contentDigest) +
    ",\"sizeBytes\":" + std::to_string(reference.sizeBytes) +
    ",\"formatVersion\":" + quote(reference.formatVersion) +
    ",\"rootManifestName\":" + quote(reference.rootManifestName) +
    ",\"publisherIdentity\":" + quote(reference.publisherIdentity) +
    ",\"policyEpoch\":" + quote(reference.policyEpoch) + "}";
  const auto targetWire = [&quote] (const RepoArtifactTarget& target) {
    return "{\"repoNode\":" + quote(target.repoNode) +
      ",\"providerIdentity\":" + quote(target.providerIdentity) +
      ",\"servicePrefix\":" + quote(target.servicePrefix) + "}";
  };
  std::string targetsWire = "[";
  for (size_t i = 0; i < targets.size(); ++i) {
    if (i != 0) targetsWire.push_back(',');
    targetsWire += targetWire(targets[i]);
  }
  targetsWire.push_back(']');
  const auto intentWire = "{\"schemaVersion\":1,\"operation\":\"PUBLISH_INTENT\","
    "\"idempotencyKeyDigest\":" + quote(std::string(64, 'a')) +
    ",\"requestDigest\":" + quote(requestDigest) +
    ",\"artifact\":" + referenceWire + ",\"generation\":1,\"seed\":" +
    targetWire(seed) + ",\"consistency\":\"ALL\",\"targets\":" +
    targetsWire + "}";
  BOOST_REQUIRE_LT(intentWire.size(), 256U * 1024U);
  BOOST_CHECK_EXCEPTION(PublishOperationIntent::fromJson(intentWire),
    ArtifactValidationError,
    [] (const ArtifactValidationError& error) {
      return error.code() == artifact_error::LimitExceeded;
    });
}

BOOST_AUTO_TEST_CASE(FullReferenceAndReceiptRoundTripKeepReplicaIdentity)
{
  const std::vector<uint8_t> bytes{1, 7, 9, 11};
  const auto reference = makeReference("round-trip", bytes);
  const auto checkDifferent = [&] (const std::function<void(ArtifactReference&)>& mutate) {
    auto changed = reference;
    mutate(changed);
    BOOST_CHECK(reference != changed);
  };
  checkDifferent([] (auto& value) { value.logicalName += "-different"; });
  checkDifferent([] (auto& value) { value.digestAlgorithm = "sha512"; });
  checkDifferent([] (auto& value) { value.contentDigest[0] = value.contentDigest[0] == '0' ? '1' : '0'; });
  checkDifferent([] (auto& value) { ++value.sizeBytes; });
  checkDifferent([] (auto& value) { value.formatVersion = "exact-packet-v1"; });
  checkDifferent([] (auto& value) { value.rootManifestName += "/different"; });
  checkDifferent([] (auto& value) { value.publisherIdentity += "/different"; });
  checkDifferent([] (auto& value) { value.policyEpoch += "-different"; });
  BOOST_CHECK(ArtifactReference::fromJson(reference.toJson()) == reference);

  ArtifactCommitReceipt receipt;
  receipt.operationId = "operation-1";
  receipt.reference = reference;
  receipt.generation = 1;
  receipt.lifecycleMode = "PERSISTENT";
  receipt.placementState = "SATISFIED";
  receipt.requestedReplicas = 2;
  receipt.requiredAcks = 2;
  receipt.achievedReplicas = 2;
  receipt.writeConsistency = "ALL";
  receipt.state = "COMMITTED";
  receipt.completedAtMs = 100;
  receipt.replicaCommits.push_back(committedReplica(
    reference, "/logical/repo-a", "/provider/a", "repo-receipt-a", "operation-1"));
  receipt.replicaCommits.push_back(committedReplica(
    reference, "/logical/repo-b", "/provider/b", "repo-receipt-b", "operation-1"));
  receipt.receiptId = persistentReceiptId(receipt);
  const auto receiptWire = receipt.toJson();
  auto decodedReceipt = ArtifactCommitReceipt::fromJson(receiptWire);
  BOOST_CHECK_EQUAL(decodedReceipt.toJson(), receiptWire);
  auto changedCompletion = receipt;
  ++changedCompletion.completedAtMs;
  BOOST_CHECK_THROW(changedCompletion.validate(), ArtifactValidationError);
  auto tamperedWire = receiptWire;
  const auto completedAtField = tamperedWire.rfind("\"completedAtMs\":100");
  BOOST_REQUIRE_NE(completedAtField, std::string::npos);
  tamperedWire.replace(completedAtField,
                       std::string("\"completedAtMs\":100").size(),
                       "\"completedAtMs\":101");
  BOOST_CHECK_THROW(ArtifactCommitReceipt::fromJson(tamperedWire),
                    ArtifactValidationError);

  ArtifactLocationRecord expired;
  expired.repoNode = "/logical/repo-a";
  expired.providerIdentity = "/provider/a";
  expired.operationId = "operation-1";
  expired.receiptId = "repo-receipt-a";
  expired.reference = reference;
  expired.generation = 1;
  expired.locationVersion = 1;
  expired.committedAtMs = 100;
  expired.expiresAtMs = 200;
  expired.forwardingHint = "/NDNSF/DistributedRepo/Object/v1";
  BOOST_CHECK_THROW(expired.validate(201), ArtifactValidationError);

  auto badBinding = receipt;
  badBinding.replicaCommits[1].reference.publisherIdentity += "/wrong";
  BOOST_CHECK_THROW(badBinding.validate(), ArtifactValidationError);
  auto duplicatePrincipal = receipt;
  duplicatePrincipal.replicaCommits[1].providerIdentity =
    duplicatePrincipal.replicaCommits[0].providerIdentity;
  BOOST_CHECK_THROW(duplicatePrincipal.validate(), ArtifactValidationError);

  ArtifactHandle handle;
  handle.reference = reference;
  handle.commitReceiptIds = {receipt.receiptId, "repo-receipt-a", "repo-receipt-b"};
  handle.selectedTargets = {
    {"/logical/repo-a", ndn::Name("/provider/a"), ndn::Name("/NDNSF/DistributedRepo/Object/v1")},
    {"/logical/repo-b", ndn::Name("/provider/b"), ndn::Name("/NDNSF/DistributedRepo/Object/v1")},
  };
  handle.replicaCommits = receipt.replicaCommits;
  handle.seedRepoId = "/logical/repo-a";
  handle.writeConsistency = "ALL";
  handle.lifecycleMode = "PERSISTENT";
  BOOST_CHECK_EQUAL(ArtifactHandle::fromJson(handle.toJson()).toJson(), handle.toJson());

  ArtifactPublishResult result;
  result.reference = reference;
  result.operationId = receipt.operationId;
  result.requestedReplicas = 2;
  result.achievedReplicas = 2;
  result.replicas = {
    {"/logical/repo-a", "/provider/a", "COMMITTED", "repo-receipt-a", ""},
    {"/logical/repo-b", "/provider/b", "COMMITTED", "repo-receipt-b", ""},
  };
  result.handle = handle;
  result.receipt = receipt;
  BOOST_CHECK_NO_THROW(result.validate());
  result.replicas[1].providerIdentity = "/provider/forged";
  BOOST_CHECK_THROW(result.validate(), ArtifactValidationError);
}

BOOST_AUTO_TEST_CASE(OneConsistencyRecordsSkippedTargetsWithoutCountingThem)
{
  const std::vector<uint8_t> bytes{3, 5, 8};
  const auto reference = makeReference("one-consistency", bytes);
  ArtifactCommitReceipt receipt;
  receipt.operationId = "one-operation";
  receipt.reference = reference;
  receipt.generation = 1;
  receipt.lifecycleMode = "PERSISTENT";
  receipt.placementState = "SATISFIED";
  receipt.requestedReplicas = 3;
  receipt.requiredAcks = 1;
  receipt.achievedReplicas = 1;
  receipt.writeConsistency = "ONE";
  receipt.state = "COMMITTED";
  receipt.completedAtMs = 100;
  receipt.replicaCommits.push_back(committedReplica(
    reference, "/logical/seed", "/provider/seed", "seed-receipt", "one-operation"));
  ArtifactReplicaCommit skipped;
  skipped.repoNode = "/logical/second";
  skipped.providerIdentity = "/provider/second";
  skipped.operationId = "one-operation";
  skipped.reference = reference;
  skipped.completedAtMs = 100;
  skipped.routeState = "UNKNOWN";
  skipped.state = "SKIPPED";
  receipt.replicaCommits.push_back(skipped);
  skipped.repoNode = "/logical/third";
  skipped.providerIdentity = "/provider/third";
  skipped.completedAtMs = 100;
  receipt.replicaCommits.push_back(skipped);
  receipt.receiptId = persistentReceiptId(receipt);
  BOOST_CHECK_NO_THROW(receipt.validate());

  receipt.achievedReplicas = 2;
  BOOST_CHECK_THROW(receipt.validate(), ArtifactValidationError);
}

BOOST_AUTO_TEST_CASE(StoreAndLocationHandlersBindLocalBytesReceiptAndIdentities)
{
  using namespace std::chrono_literals;
  TempRoot temporary;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec192-artifact-receipt",
                                   "tpm-memory:spec192-artifact-receipt");
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/provider"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/authority"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
    ndn::Name("/spec192/group"), providerCert, authorityCert,
    std::filesystem::absolute("examples/trust-any.conf").string());

  const ndn::Name servicePrefix(RepoClient::DEFAULT_SERVICE_NAME);
  const std::string logicalRepo = "/logical/repo-a";
  const auto metadataPath = (temporary.path() / "repo.artifact-v2.sqlite").string();
  StorageCapability capability;
  capability.repoNode = logicalRepo;
  capability.repoMode = "persistent";
  capability.freeBytes = 1024 * 1024;
  auto store = std::make_shared<FilesystemRepoStoreBackend>(
    (temporary.path() / "repo-payloads").string(), 4096, 1024,
    "spec192-persistent-receipt-owner");
  auto node = std::make_shared<RepoNode>(
    servicePrefix, capability, store, logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE),
    metadataPath);
  node->registerServices(provider);
  SqliteArtifactMetadataStore durableCatalog(metadataPath, false, 15);
  BOOST_CHECK_EQUAL(durableCatalog.schemaGeneration(), 16U);

  const std::vector<uint8_t> directBytes{0x10, 0x20, 0x30, 0x40};
  const auto directReference = makeReference("PUBLISH_STATUS-direct-store", directBytes);
  const auto callerIdentity = std::string("/spec192/user");
  const RepoArtifactTarget localSeed{
    logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE)};
  ArtifactStoreRequest direct;
  direct.generation = 1;
  direct.reference = directReference;
  direct.bytes = directBytes;
  const auto storeName = makeRepoServiceName(servicePrefix, "STORE");

  const auto unjournaledReference = makeReference(
    "PUBLISH_STATUS-unjournaled-store", directBytes);
  ArtifactStoreRequest unjournaled;
  unjournaled.operationId = "operation-without-intent";
  unjournaled.generation = 1;
  unjournaled.reference = unjournaledReference;
  unjournaled.bytes = directBytes;
  auto response = dispatch(provider, storeName, unjournaled.toWire(),
                           "unjournaled-seed-store");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!node->core().has(artifactStorageObjectName(unjournaledReference)));
  BOOST_CHECK(!durableCatalog.lookupPersistentLocation(
    unjournaledReference, logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), 0));
  BOOST_CHECK_EQUAL(countPersistentReceipts(metadataPath, unjournaledReference), 0U);

  const auto forgedOwnerReference = makeReference(
    "PUBLISH_STATUS-forged-replica-owner", directBytes);
  ArtifactStoreRequest forgedOwner = unjournaled;
  forgedOwner.operationId = "operation-forged-replica-owner";
  forgedOwner.reference = forgedOwnerReference;
  forgedOwner.operationOwner = RepoArtifactTarget{
    "/logical/remote-seed", "/spec192/seed-provider", servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE)};
  response = dispatch(provider, storeName, forgedOwner.toWire(),
                      "forged-replica-owner-store");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!node->core().has(artifactStorageObjectName(forgedOwnerReference)));
  BOOST_CHECK(!durableCatalog.lookupPersistentLocation(
    forgedOwnerReference, logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), 0));
  BOOST_CHECK_EQUAL(countPersistentReceipts(metadataPath, forgedOwnerReference), 0U);

  PublishOperationIntent directIntent;
  directIntent.idempotencyKeyDigest = digestPublishIdempotencyKey(
    "spec192-direct-store-operation-key");
  directIntent.reference = directReference;
  directIntent.generation = direct.generation;
  directIntent.seed = localSeed;
  directIntent.consistency = "ONE";
  directIntent.targets = {localSeed};
  directIntent.schemaVersion = 2;
  const auto grantTime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  SqliteArtifactMetadataStore ownerJournal(metadataPath, true, 16, false);
  const auto directGrant = ownerJournal.grantUserStorage(callerIdentity, logicalRepo,
    "direct-store-grant", callerIdentity, artifactStorageObjectName(directReference),
    grantTime + 60000, grantTime);
  const RepoUserStorageBinding directBinding{1, callerIdentity, directGrant.grantId,
    directGrant.ownerEpoch};
  directIntent.userStorageBindings = {{1, localSeed, directBinding, callerIdentity}};
  directIntent.requestDigest = makePublishRequestDigest(
    directReference, directIntent.generation, localSeed,
    directIntent.consistency, directIntent.targets,
    directIntent.userStorageBindings);
  direct.operationId = makePublishOperationId(
    callerIdentity, localSeed, directIntent.idempotencyKeyDigest);
  direct.schemaVersion = 3;
  direct.storageBinding = directBinding;
  const auto directIntentJson = directIntent.toJson();
  const std::vector<uint8_t> directIntentWire(
    directIntentJson.begin(), directIntentJson.end());
  response = dispatch(provider, storeName, directIntentWire, "direct-store-intent");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), direct.operationId);

  response = dispatch(provider, storeName, direct.toWire(), "direct-store");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto directReceipt = RepoArtifactCommitReceipt::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(directReceipt.repoNode, logicalRepo);
  BOOST_CHECK_EQUAL(directReceipt.providerIdentity, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE));
  BOOST_CHECK(directReceipt.reference == directReference);
  BOOST_CHECK_EQUAL(directReceipt.operationId, direct.operationId);
  BOOST_CHECK_EQUAL(directReceipt.generation, direct.generation);
  const auto committedDirectStatusName = makeRepoServiceName(servicePrefix, "STATUS");
  ArtifactLocationRequest committedDirectQuery;
  committedDirectQuery.reference = directReference;
  const auto committedDirectQueryJson = committedDirectQuery.toJson();
  response = dispatch(provider, committedDirectStatusName,
    std::vector<uint8_t>(committedDirectQueryJson.begin(),
                         committedDirectQueryJson.end()), "direct-location-after-commit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto directLocation = ArtifactLocationRecord::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(directLocation.repoNode, logicalRepo);
  BOOST_CHECK_EQUAL(directLocation.providerIdentity, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE));
  BOOST_CHECK(directLocation.reference == directReference);
  BOOST_CHECK_EQUAL(directLocation.operationId, direct.operationId);
  BOOST_CHECK_EQUAL(directLocation.generation, direct.generation);
  BOOST_CHECK_EQUAL(directLocation.locationVersion, 1U);
  BOOST_CHECK_EQUAL(directLocation.routeState, "ROUTABLE");
  BOOST_CHECK(durableCatalog.lookupPersistentLocation(
    directReference, logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), 0).has_value());
  BOOST_CHECK(durableCatalog.hasPersistentCommitReceipt(
    directReference, logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE),
    direct.operationId, directReceipt.receiptId, direct.generation));
  BOOST_CHECK(!durableCatalog.hasPersistentCommitReceipt(
    directReference, logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE),
    direct.operationId, "receipt-from-another-operation", direct.generation));
  checkPersistentReceiptRow(metadataPath, directLocation);

  response = dispatch(provider, storeName, direct.toWire(), "direct-store-retry");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto retriedDirectReceipt = RepoArtifactCommitReceipt::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(retriedDirectReceipt.receiptId, directReceipt.receiptId);
  checkPersistentReceiptRow(metadataPath, directLocation);
  BOOST_CHECK(RepoClient::getRange(
    *node, artifactStorageObjectName(directReference),
    RepoByteRange{0, directBytes.size()}) == directBytes);

  auto conflictingDirect = direct;
  conflictingDirect.generation = 2;
  PublishOperationIntent conflictingIntent = directIntent;
  conflictingIntent.idempotencyKeyDigest = digestPublishIdempotencyKey(
    "spec192-direct-store-conflict-key");
  conflictingIntent.generation = conflictingDirect.generation;
  conflictingIntent.requestDigest = makePublishRequestDigest(
    directReference, conflictingIntent.generation, localSeed,
    conflictingIntent.consistency, conflictingIntent.targets,
    conflictingIntent.userStorageBindings);
  conflictingDirect.operationId = makePublishOperationId(
    callerIdentity, localSeed, conflictingIntent.idempotencyKeyDigest);
  const auto conflictingIntentJson = conflictingIntent.toJson();
  const std::vector<uint8_t> conflictingIntentWire(
    conflictingIntentJson.begin(), conflictingIntentJson.end());
  response = dispatch(provider, storeName, conflictingIntentWire,
                      "direct-store-conflict-intent");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  response = dispatch(provider, storeName, conflictingDirect.toWire(),
                      "direct-store-conflict");
  BOOST_CHECK(!response.getStatus());
  const auto preservedDirectManifest = node->core().getManifest(
    artifactStorageObjectName(directReference));
  BOOST_CHECK_EQUAL(preservedDirectManifest.operationId, direct.operationId);
  BOOST_CHECK_EQUAL(preservedDirectManifest.generation, direct.generation);
  BOOST_CHECK(RepoClient::getRange(
    *node, artifactStorageObjectName(directReference),
    RepoByteRange{0, directBytes.size()}) == directBytes);
  ArtifactLocationRequest directQuery;
  directQuery.reference = directReference;
  const auto directStatusName = makeRepoServiceName(servicePrefix, "STATUS");
  const auto directQueryJson = directQuery.toJson();
  response = dispatch(provider, directStatusName,
                      std::vector<uint8_t>(directQueryJson.begin(),
                                           directQueryJson.end()),
                      "direct-location-after-conflict");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(ArtifactLocationRecord::fromJson(responseString(response)).toJson(),
                    directLocation.toJson());
  checkPersistentReceiptRow(metadataPath, directLocation);

  const std::vector<uint8_t> rangedBytes{1, 2, 3, 4, 5, 6};
  const auto rangedReference = makeReference("ranged-store", rangedBytes);
  const auto rangedGrantTime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  const auto rangedGrant = ownerJournal.grantUserStorage(callerIdentity, logicalRepo,
    "ranged-store-grant", callerIdentity, artifactStorageObjectName(rangedReference),
    rangedGrantTime + 60000, rangedGrantTime);
  const RepoUserStorageBinding rangedBinding{1, callerIdentity, rangedGrant.grantId,
    rangedGrant.ownerEpoch};
  PublishOperationIntent rangedIntent;
  rangedIntent.schemaVersion = 2;
  rangedIntent.idempotencyKeyDigest = digestPublishIdempotencyKey(
    "spec192-ranged-store-operation-key");
  rangedIntent.reference = rangedReference;
  rangedIntent.generation = 1;
  rangedIntent.seed = localSeed;
  rangedIntent.consistency = "ONE";
  rangedIntent.targets = {localSeed};
  rangedIntent.userStorageBindings = {{1, localSeed, rangedBinding, callerIdentity}};
  rangedIntent.requestDigest = makePublishRequestDigest(
    rangedReference, rangedIntent.generation, localSeed,
    rangedIntent.consistency, rangedIntent.targets,
    rangedIntent.userStorageBindings);
  const auto operation = makePublishOperationId(
    callerIdentity, localSeed, rangedIntent.idempotencyKeyDigest);
  const auto rangedIntentJson = rangedIntent.toJson();
  const std::vector<uint8_t> rangedIntentWire(
    rangedIntentJson.begin(), rangedIntentJson.end());
  response = dispatch(provider, storeName, rangedIntentWire, "ranged-store-intent");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), operation);

  ArtifactRangeWriteRequest first;
  first.schemaVersion = 3;
  first.operationId = operation;
  first.generation = 1;
  first.reference = rangedReference;
  first.storageBinding = rangedBinding;
  first.offsetBytes = 0;
  first.lengthBytes = 3;
  first.bytes = {1, 2, 3};
  const auto storeRangeName = makeRepoServiceName(servicePrefix, "STORE_RANGE");
  response = dispatch(provider, storeRangeName, first.toWire(), "ranged-first");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK(!durableCatalog.lookupPersistentLocation(
    rangedReference, logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), 0));

  ArtifactRangeWriteRequest second = first;
  second.offsetBytes = 3;
  second.bytes = {4, 5, 6};
  response = dispatch(provider, storeRangeName, second.toWire(), "ranged-second");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK(!durableCatalog.lookupPersistentLocation(
    rangedReference, logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), 0));

  ArtifactRangeCommitRequest commit;
  commit.schemaVersion = 3;
  commit.operationId = operation;
  commit.generation = 1;
  commit.reference = rangedReference;
  commit.storageBinding = rangedBinding;
  const auto commitName = makeRepoServiceName(servicePrefix, "COMMIT_RANGES");
  response = dispatch(provider, commitName, commit.toWire(), "ranged-commit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto rangedReceipt = RepoArtifactCommitReceipt::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(rangedReceipt.repoNode, logicalRepo);
  BOOST_CHECK_EQUAL(rangedReceipt.providerIdentity, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE));
  BOOST_CHECK(rangedReceipt.reference == rangedReference);
  BOOST_CHECK_EQUAL(rangedReceipt.operationId, operation);
  BOOST_CHECK_EQUAL(rangedReceipt.generation, 1U);
  ArtifactLocationRequest committedRangeQuery;
  committedRangeQuery.reference = rangedReference;
  const auto committedRangeQueryJson = committedRangeQuery.toJson();
  response = dispatch(provider, committedDirectStatusName,
                      std::vector<uint8_t>(committedRangeQueryJson.begin(),
                                           committedRangeQueryJson.end()),
                      "ranged-location-after-commit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto rangedLocation = ArtifactLocationRecord::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(rangedLocation.repoNode, logicalRepo);
  BOOST_CHECK_EQUAL(rangedLocation.providerIdentity, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE));
  BOOST_CHECK(rangedLocation.reference == rangedReference);
  BOOST_CHECK_EQUAL(rangedLocation.operationId, operation);
  BOOST_CHECK_EQUAL(rangedLocation.generation, 1U);
  BOOST_CHECK_EQUAL(rangedLocation.routeState, "ROUTABLE");
  const auto localBytes = RepoClient::getRange(
    *node, artifactStorageObjectName(rangedReference), RepoByteRange{0, rangedBytes.size()});
  BOOST_CHECK(localBytes == rangedBytes);
  checkPersistentReceiptRow(metadataPath, rangedLocation);

  replaceCurrentLocationReceiptId(
    metadataPath, direct.operationId, rangedReceipt.receiptId);
  response = dispatch(provider, directStatusName,
    std::vector<uint8_t>(directQueryJson.begin(), directQueryJson.end()),
    "direct-location-rejects-mismatched-receipt-id");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(response.getErrorInfo().find(
    "repo-artifact-location-invalid: current catalog receipt does not bind") != std::string::npos);
  replaceCurrentLocationReceiptId(
    metadataPath, direct.operationId, directReceipt.receiptId);
  response = dispatch(provider, directStatusName,
    std::vector<uint8_t>(directQueryJson.begin(), directQueryJson.end()),
    "direct-location-after-mismatch-restore");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(ArtifactLocationRecord::fromJson(responseString(response)).toJson(),
                    directLocation.toJson());

  BOOST_CHECK_EQUAL(node->core().reapExpiredRangeReservations(0ms), 0U);
  response = dispatch(provider, storeRangeName, first.toWire(), "ranged-retry");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  response = dispatch(provider, commitName, commit.toWire(), "ranged-recommit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto retriedRangedReceipt = RepoArtifactCommitReceipt::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(retriedRangedReceipt.receiptId, rangedReceipt.receiptId);
  BOOST_CHECK_EQUAL(node->core().reapExpiredRangeReservations(0ms), 0U);

  auto conflictingRange = first;
  conflictingRange.generation = 2;
  PublishOperationIntent conflictingRangeIntent = rangedIntent;
  conflictingRangeIntent.idempotencyKeyDigest = digestPublishIdempotencyKey(
    "spec192-ranged-store-conflict-key");
  conflictingRangeIntent.generation = conflictingRange.generation;
  conflictingRangeIntent.requestDigest = makePublishRequestDigest(
    rangedReference, conflictingRange.generation, localSeed,
    conflictingRangeIntent.consistency, conflictingRangeIntent.targets,
    conflictingRangeIntent.userStorageBindings);
  conflictingRange.operationId = makePublishOperationId(
    callerIdentity, localSeed, conflictingRangeIntent.idempotencyKeyDigest);
  const auto conflictingRangeIntentJson = conflictingRangeIntent.toJson();
  const std::vector<uint8_t> conflictingRangeIntentWire(
    conflictingRangeIntentJson.begin(), conflictingRangeIntentJson.end());
  response = dispatch(provider, storeName, conflictingRangeIntentWire,
                      "ranged-store-conflict-intent");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  response = dispatch(provider, storeRangeName, conflictingRange.toWire(),
                      "ranged-store-conflict");
  BOOST_CHECK(!response.getStatus());
  auto conflictingCommit = commit;
  conflictingCommit.operationId = conflictingRange.operationId;
  conflictingCommit.generation = conflictingRange.generation;
  response = dispatch(provider, commitName, conflictingCommit.toWire(),
                      "ranged-commit-conflict");
  BOOST_CHECK(!response.getStatus());
  const auto preservedRangedManifest = node->core().getManifest(
    artifactStorageObjectName(rangedReference));
  BOOST_CHECK_EQUAL(preservedRangedManifest.operationId, operation);
  BOOST_CHECK_EQUAL(preservedRangedManifest.generation, 1U);
  BOOST_CHECK(RepoClient::getRange(
    *node, artifactStorageObjectName(rangedReference),
    RepoByteRange{0, rangedBytes.size()}) == rangedBytes);
  ArtifactLocationRequest rangedQuery;
  rangedQuery.reference = rangedReference;
  const auto rangedQueryJson = rangedQuery.toJson();
  const auto rangedStatusName = makeRepoServiceName(servicePrefix, "STATUS");
  response = dispatch(provider, rangedStatusName,
                      std::vector<uint8_t>(rangedQueryJson.begin(),
                                           rangedQueryJson.end()),
                      "ranged-location-after-conflict");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(ArtifactLocationRecord::fromJson(responseString(response)).toJson(),
                    rangedLocation.toJson());
  checkPersistentReceiptRow(metadataPath, rangedLocation);
  BOOST_CHECK_EQUAL(node->core().reapExpiredRangeReservations(0ms), 0U);

  ArtifactLocationRequest query;
  query.reference = rangedReference;
  const auto statusName = makeRepoServiceName(servicePrefix, "STATUS");
  const auto queryJson = query.toJson();
  response = dispatch(provider, statusName,
                      std::vector<uint8_t>(queryJson.begin(), queryJson.end()),
                      "ranged-location");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto returnedLocation = ArtifactLocationRecord::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(returnedLocation.toJson(), rangedLocation.toJson());
}

BOOST_AUTO_TEST_CASE(RangeAbortIsOwnerBoundAndExpiredStagingReleasesCapacity)
{
  using namespace std::chrono_literals;
  TempRoot temporary;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec192-range-abort",
                                   "tpm-memory:spec192-range-abort");
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/provider-abort"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/authority-abort"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
    ndn::Name("/spec192/group-abort"), providerCert, authorityCert,
    std::filesystem::absolute("examples/trust-any.conf").string());

  const ndn::Name servicePrefix(RepoClient::DEFAULT_SERVICE_NAME);
  const std::string logicalRepo = "/logical/range-abort";
  const auto metadataPath = (temporary.path() / "artifact-v2.sqlite").string();
  StorageCapability capability;
  capability.repoNode = logicalRepo;
  capability.repoMode = "persistent";
  capability.freeBytes = 1024 * 1024;
  auto store = std::make_shared<FilesystemRepoStoreBackend>(
    (temporary.path() / "objects").string(), 4096, 1024,
    "spec192-range-abort-owner");
  auto node = std::make_shared<RepoNode>(
    servicePrefix, capability, store, logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE),
    metadataPath);
  node->registerServices(provider);

  SqliteArtifactMetadataStore ownerJournal(metadataPath, true, 16, false);
  const auto makeStorageBinding = [&] (const ArtifactReference& reference,
                                       const std::string& grantId) {
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
    const auto grant = ownerJournal.grantUserStorage("/spec192/user", logicalRepo,
      grantId, "/spec192/user", artifactStorageObjectName(reference), now + 60000, now);
    return RepoUserStorageBinding{1, "/spec192/user", grant.grantId, grant.ownerEpoch};
  };

  const auto service = makeRepoServiceName(servicePrefix, "STORE_RANGE");
  const auto publishService = makeRepoServiceName(servicePrefix, "STORE");
  const auto abortService = makeRepoServiceName(servicePrefix, "ABORT_RANGES");
  const auto commitService = makeRepoServiceName(servicePrefix, "COMMIT_RANGES");
  const RepoArtifactTarget seed{
    logicalRepo, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE)};
  const std::vector<uint8_t> ownedBytes{0x10, 0x20, 0x30, 0x40};
  const auto ownedReference = makeReference("owner-bound", ownedBytes);
  const auto ownedBinding = makeStorageBinding(ownedReference, "grant-owner-bound");
  ArtifactRangeWriteRequest write;
  write.operationId = reservePublishOperation(
    provider, publishService, seed, ownedReference, 1,
    "spec192-range-owner-key", ownedBinding, {seed}, "range-owner-intent");
  write.schemaVersion = 3;
  write.generation = 1;
  write.reference = ownedReference;
  write.storageBinding = ownedBinding;
  write.offsetBytes = 0;
  write.lengthBytes = ownedBytes.size();
  write.bytes = ownedBytes;
  auto response = dispatch(provider, service, write.toWire(), "range-owner-write");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());

  ArtifactRangeAbortRequest wrongOwner;
  wrongOwner.schemaVersion = 2;
  wrongOwner.operationId = "different-operation";
  wrongOwner.generation = 1;
  wrongOwner.reference = ownedReference;
  wrongOwner.storageBinding = ownedBinding;
  const auto wrongWire = wrongOwner.toWire();
  BOOST_CHECK(ArtifactRangeAbortRequest::fromWire(wrongWire).operationId ==
              wrongOwner.operationId);
  response = dispatch(provider, abortService, wrongWire, "range-wrong-abort");
  BOOST_CHECK(!response.getStatus());

  ArtifactRangeCommitRequest commit;
  commit.schemaVersion = 3;
  commit.operationId = write.operationId;
  commit.generation = 1;
  commit.reference = ownedReference;
  commit.storageBinding = ownedBinding;
  response = dispatch(provider, commitService, commit.toWire(), "range-owned-commit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK(node->core().has(artifactStorageObjectName(ownedReference)));
  BOOST_CHECK(RepoClient::getRange(
    *node, artifactStorageObjectName(ownedReference), {0, ownedBytes.size()}) == ownedBytes);

  const std::vector<uint8_t> abortedBytes{0x51, 0x52, 0x53};
  const auto abortedReference = makeReference("exact-abort", abortedBytes);
  const auto abortedBinding = makeStorageBinding(abortedReference, "grant-exact-abort");
  write.operationId = reservePublishOperation(
    provider, publishService, seed, abortedReference, 1,
    "spec192-range-exact-abort-key", abortedBinding, {seed}, "range-exact-intent");
  write.storageBinding = abortedBinding;
  write.reference = abortedReference;
  write.lengthBytes = abortedBytes.size();
  write.bytes = abortedBytes;
  response = dispatch(provider, service, write.toWire(), "range-exact-write");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  ArtifactRangeAbortRequest exactAbort;
  exactAbort.schemaVersion = 2;
  exactAbort.operationId = write.operationId;
  exactAbort.generation = 1;
  exactAbort.reference = abortedReference;
  exactAbort.storageBinding = abortedBinding;
  response = dispatch(provider, abortService, exactAbort.toWire(), "range-exact-abort");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  commit.operationId = write.operationId;
  commit.reference = abortedReference;
  commit.storageBinding = abortedBinding;
  response = dispatch(provider, commitService, commit.toWire(), "range-aborted-commit");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!node->core().has(artifactStorageObjectName(abortedReference)));

  const std::vector<uint8_t> expiredBytes{0x61, 0x62, 0x63, 0x64, 0x65};
  const auto expiredReference = makeReference("expired-range", expiredBytes);
  const auto expiredBinding = makeStorageBinding(expiredReference, "grant-expired-range");
  write.operationId = reservePublishOperation(
    provider, publishService, seed, expiredReference, 1,
    "spec192-range-expired-key", expiredBinding, {seed}, "range-expired-intent");
  write.storageBinding = expiredBinding;
  write.reference = expiredReference;
  write.lengthBytes = expiredBytes.size();
  write.bytes = expiredBytes;
  response = dispatch(provider, service, write.toWire(), "range-expired-write");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(node->core().reapExpiredRangeReservations(0ms), 1U);
  commit.operationId = write.operationId;
  commit.reference = expiredReference;
  commit.storageBinding = expiredBinding;
  response = dispatch(provider, commitService, commit.toWire(), "range-expired-commit");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!node->core().has(artifactStorageObjectName(expiredReference)));
}

BOOST_AUTO_TEST_CASE(RepoRestartRemovesAbandonedRangeStaging)
{
  TempRoot temporary;
  const auto root = temporary.path() / "orphaned-repo";
  const std::vector<uint8_t> bytes{0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  RepoObjectManifest manifest;
  manifest.objectName = "/repo/spec192/abandoned-range";
  manifest.sha256 = digest(bytes);
  manifest.size = bytes.size();
  manifest.generation = 1;
  manifest.operationId = "abandoned-client-operation";
  const std::vector<uint8_t> firstRange(bytes.begin(), bytes.begin() + 3);
  {
    FilesystemRepoStoreBackend backend(
      root.string(), 4096, 1024, "spec192-abandoned-range-owner");
    backend.putRange(manifest, {0, 3}, firstRange);
    BOOST_CHECK_GT(std::distance(
      std::filesystem::directory_iterator(root / "staging"),
      std::filesystem::directory_iterator{}), 0);
  }

  {
    FilesystemRepoStoreBackend restarted(
      root.string(), 4096, 1024, "spec192-abandoned-range-owner");
    BOOST_CHECK_EQUAL(restarted.size(), 0U);
    BOOST_CHECK_EQUAL(std::distance(
      std::filesystem::directory_iterator(root / "staging"),
      std::filesystem::directory_iterator{}), 0);
  }
}

BOOST_AUTO_TEST_CASE(OwnedAvailabilityCodecsRejectOwnerEpochAndTupleSubstitution)
{
  RepoAvailabilityProbeRequest request;
  request.schemaVersion = 2; request.ownerEpoch = 7;
  request.reference = makeReference("owned-availability-codec", {1, 2, 3});
  request.expectedLocation = {"/repo/local", "/provider/local", "op-7", "receipt-7", 1, 2};
  request.probeKind = "verify_content";
  const auto wire = request.toJson();
  BOOST_CHECK_EQUAL(RepoAvailabilityProbeRequest::fromJson(wire).toJson(), wire);
  auto invalidRequest = request;
  invalidRequest.ownerEpoch = 0;
  BOOST_CHECK_THROW(invalidRequest.toJson(), ArtifactValidationError);
  invalidRequest.ownerEpoch = std::numeric_limits<uint64_t>::max();
  BOOST_CHECK_THROW(invalidRequest.toJson(), ArtifactValidationError);
  invalidRequest = request; invalidRequest.schemaVersion = 1;
  BOOST_CHECK_THROW(invalidRequest.toJson(), ArtifactValidationError);
  auto extraOwner = wire; extraOwner.insert(extraOwner.size() - 1, ",\"ownerIdentity\":\"/other\"");
  BOOST_CHECK_THROW(RepoAvailabilityProbeRequest::fromJson(extraOwner), std::invalid_argument);

  RepoAvailabilityProbeResponse response;
  response.schemaVersion = 2; response.ownerIdentity = "/user/owner"; response.ownerEpoch = 7;
  response.reference = request.reference; response.expectedLocation = request.expectedLocation;
  response.report.locationKind = "PERSISTENT";
  response.report.repoNode = request.expectedLocation.repoNode;
  response.report.providerIdentity = request.expectedLocation.providerIdentity;
  response.report.generation = 1; response.report.reachability = "REACHABLE";
  response.report.probeKind = "verify_content";
  response.report.observedAtMs = 100; response.report.validUntilMs = 100;
  BOOST_CHECK_EQUAL(RepoAvailabilityProbeResponse::fromJson(response.toJson()).toJson(), response.toJson());
  BOOST_CHECK_NO_THROW(response.validateExpected(request, "/user/owner"));
  BOOST_CHECK_THROW(response.validateExpected(request, "/user/other"), ArtifactValidationError);
  auto bad = response; bad.ownerEpoch = 8;
  BOOST_CHECK_THROW(bad.validateExpected(request, "/user/owner"), ArtifactValidationError);
  bad = response; bad.expectedLocation.receiptId = "substituted";
  BOOST_CHECK_THROW(bad.validateExpected(request, "/user/owner"), ArtifactValidationError);
  bad = response; bad.reference = makeReference("another-object", {1, 2, 3});
  BOOST_CHECK_THROW(bad.validateExpected(request, "/user/owner"), ArtifactValidationError);
  bad = response; bad.report.probeKind = "metadata";
  BOOST_CHECK_THROW(bad.validateExpected(request, "/user/owner"), ArtifactValidationError);
  bad = response; bad.schemaVersion = 1; bad.ownerIdentity.clear(); bad.ownerEpoch = 0;
  BOOST_CHECK_THROW(bad.validateExpected(request, "/user/owner"), ArtifactValidationError);
  BOOST_CHECK_NO_THROW(bad.toJson());
  bad = response; bad.ownerIdentity = "/";
  BOOST_CHECK_THROW(bad.toJson(), ArtifactValidationError);
  bad = response; bad.ownerIdentity = "/8=user/8=owner";
  BOOST_CHECK_THROW(bad.toJson(), ArtifactValidationError);
}

BOOST_AUTO_TEST_CASE(AvailabilityCodecsAndInAppReceiptRemainTruthful)
{
  const std::vector<uint8_t> bytes{0x41, 0x50, 0x49, 0x31};
  const auto reference = makeReference("availability-codec", bytes);
  RepoReceiptLookupRequest receiptQuery;
  receiptQuery.receiptId = "in-app-local-7";
  receiptQuery.repoNode = "/repo/local";
  receiptQuery.providerIdentity = "/provider/local";
  receiptQuery.reference = reference;
  const auto parsedReceiptQuery = RepoReceiptLookupRequest::fromJson(
    receiptQuery.toJson());
  BOOST_CHECK_EQUAL(parsedReceiptQuery.toJson(), receiptQuery.toJson());

  ArtifactExpectedLocation expected{
    "/repo/local", "/provider/local", "operation-7", "receipt-7", 3, 9};
  RepoAvailabilityProbeRequest probeRequest;
  probeRequest.reference = reference;
  probeRequest.expectedLocation = expected;
  probeRequest.probeKind = "verify_content";
  const auto parsedProbe = RepoAvailabilityProbeRequest::fromJson(
    probeRequest.toJson());
  BOOST_CHECK_EQUAL(parsedProbe.toJson(), probeRequest.toJson());

  AvailabilityLocationReport location;
  location.locationKind = "PERSISTENT";
  location.repoNode = expected.repoNode;
  location.providerIdentity = expected.providerIdentity;
  location.generation = expected.generation;
  location.operationId = expected.operationId;
  location.receiptId = expected.receiptId;
  location.locationVersion = expected.locationVersion;
  location.reachability = "REACHABLE";
  location.objectPresence = "PRESENT";
  location.integrity = "VERIFIED";
  location.probeKind = "verify_content";
  location.observedAtMs = 100;
  location.validUntilMs = 200;
  RepoAvailabilityProbeResponse response;
  response.reference = reference;
  response.expectedLocation = expected;
  response.report = location;
  BOOST_CHECK_EQUAL(
    AvailabilityLocationReport::fromJson(location.toJson()).toJson(),
    location.toJson());
  const auto parsedResponse = RepoAvailabilityProbeResponse::fromJson(
    response.toJson());
  BOOST_CHECK_EQUAL(parsedResponse.toJson(), response.toJson());

  AvailabilityReport publicReport;
  publicReport.reference = reference;
  publicReport.probeKind = "verify_content";
  publicReport.locations.push_back(location);
  BOOST_CHECK_EQUAL(AvailabilityReport::fromJson(publicReport.toJson()).toJson(),
                    publicReport.toJson());
  AvailabilityLocationReport inAppLocation;
  inAppLocation.locationKind = "IN_APP";
  inAppLocation.generation = 1;
  inAppLocation.reachability = "REACHABLE";
  inAppLocation.objectPresence = "PRESENT";
  inAppLocation.observedAtMs = 100;
  inAppLocation.validUntilMs = 200;
  BOOST_CHECK_EQUAL(
    AvailabilityLocationReport::fromJson(inAppLocation.toJson()).toJson(),
    inAppLocation.toJson());
  auto inAppWithPartialTuple = inAppLocation.toJson();
  const auto inAppReachabilityField = inAppWithPartialTuple.find(",\"reachability\":");
  BOOST_REQUIRE_NE(inAppReachabilityField, std::string::npos);
  inAppWithPartialTuple.insert(inAppReachabilityField,
    ",\"operationId\":\"operation-7\"");
  BOOST_CHECK_THROW(AvailabilityLocationReport::fromJson(inAppWithPartialTuple),
                    ArtifactValidationError);
  auto inAppWithEmptyTuple = inAppLocation.toJson();
  const auto emptyInAppReachabilityField = inAppWithEmptyTuple.find(",\"reachability\":");
  BOOST_REQUIRE_NE(emptyInAppReachabilityField, std::string::npos);
  inAppWithEmptyTuple.insert(emptyInAppReachabilityField,
    ",\"operationId\":\"\",\"receiptId\":\"\",\"locationVersion\":0");
  BOOST_CHECK_THROW(AvailabilityLocationReport::fromJson(inAppWithEmptyTuple),
                    ArtifactValidationError);
  auto inAppWithCurrentTuple = inAppLocation.toJson();
  const auto reachabilityField = inAppWithCurrentTuple.find(",\"reachability\":");
  BOOST_REQUIRE_NE(reachabilityField, std::string::npos);
  inAppWithCurrentTuple.insert(reachabilityField,
    ",\"operationId\":\"operation-7\",\"receiptId\":\"receipt-7\",\"locationVersion\":9");
  BOOST_CHECK_THROW(AvailabilityLocationReport::fromJson(inAppWithCurrentTuple),
                    ArtifactValidationError);
  AvailabilityReport inAppPublicReport;
  inAppPublicReport.reference = reference;
  inAppPublicReport.probeKind = "metadata";
  inAppPublicReport.locations.push_back(inAppLocation);
  BOOST_CHECK_EQUAL(AvailabilityReport::fromJson(inAppPublicReport.toJson()).toJson(),
                    inAppPublicReport.toJson());
  auto inAppWithCurrentPublicTuple = inAppPublicReport.toJson();
  const auto currentPublicReachabilityField =
    inAppWithCurrentPublicTuple.find(",\"reachability\":");
  BOOST_REQUIRE_NE(currentPublicReachabilityField, std::string::npos);
  inAppWithCurrentPublicTuple.insert(currentPublicReachabilityField,
    ",\"operationId\":\"operation-7\",\"receiptId\":\"receipt-7\",\"locationVersion\":9");
  BOOST_CHECK_THROW(AvailabilityReport::fromJson(inAppWithCurrentPublicTuple),
                    ArtifactValidationError);
  auto inAppWithEmptyCurrentTuple = inAppPublicReport.toJson();
  const auto publicReachabilityField = inAppWithEmptyCurrentTuple.find(",\"reachability\":");
  BOOST_REQUIRE_NE(publicReachabilityField, std::string::npos);
  inAppWithEmptyCurrentTuple.insert(publicReachabilityField,
    ",\"operationId\":\"\",\"receiptId\":\"\",\"locationVersion\":0");
  BOOST_CHECK_THROW(AvailabilityReport::fromJson(inAppWithEmptyCurrentTuple),
                    ArtifactValidationError);
  auto inAppWithPartialPublicTuple = inAppPublicReport.toJson();
  const auto partialPublicReachabilityField =
    inAppWithPartialPublicTuple.find(",\"reachability\":");
  BOOST_REQUIRE_NE(partialPublicReachabilityField, std::string::npos);
  inAppWithPartialPublicTuple.insert(partialPublicReachabilityField,
    ",\"operationId\":\"operation-7\"");
  BOOST_CHECK_THROW(AvailabilityReport::fromJson(inAppWithPartialPublicTuple),
                    ArtifactValidationError);

  AvailabilityLocationReport persistentWithoutLocation;
  persistentWithoutLocation.locationKind = "PERSISTENT";
  persistentWithoutLocation.repoNode = expected.repoNode;
  persistentWithoutLocation.providerIdentity = expected.providerIdentity;
  persistentWithoutLocation.generation = expected.generation;
  persistentWithoutLocation.reachability = "UNKNOWN";
  persistentWithoutLocation.objectPresence = "UNKNOWN";
  persistentWithoutLocation.integrity = "NOT_CHECKED";
  persistentWithoutLocation.observedAtMs = 100;
  persistentWithoutLocation.validUntilMs = 200;
  BOOST_CHECK_EQUAL(
    AvailabilityLocationReport::fromJson(persistentWithoutLocation.toJson()).toJson(),
    persistentWithoutLocation.toJson());
  AvailabilityReport persistentUnknownReport;
  persistentUnknownReport.reference = reference;
  persistentUnknownReport.probeKind = "metadata";
  persistentUnknownReport.locations.push_back(persistentWithoutLocation);
  BOOST_CHECK_EQUAL(
    AvailabilityReport::fromJson(persistentUnknownReport.toJson()).toJson(),
    persistentUnknownReport.toJson());
  auto persistentUncommitted = persistentUnknownReport;
  persistentUncommitted.locations.front().generation = 0;
  BOOST_CHECK_EQUAL(
    AvailabilityReport::fromJson(persistentUncommitted.toJson()).toJson(),
    persistentUncommitted.toJson());
  auto persistentUncommittedWithKnownPresence = persistentUncommitted;
  persistentUncommittedWithKnownPresence.locations.front().objectPresence = "ABSENT";
  BOOST_CHECK_THROW(persistentUncommittedWithKnownPresence.toJson(),
                    ArtifactValidationError);
  auto persistentUncommittedReachable = persistentUncommitted;
  persistentUncommittedReachable.locations.front().reachability = "REACHABLE";
  BOOST_CHECK_THROW(persistentUncommittedReachable.toJson(), ArtifactValidationError);

  RepoAvailabilityProbeResponse responseWithoutCurrentLocation;
  responseWithoutCurrentLocation.reference = reference;
  responseWithoutCurrentLocation.expectedLocation = expected;
  responseWithoutCurrentLocation.report = persistentWithoutLocation;
  responseWithoutCurrentLocation.report.reachability = "REACHABLE";
  const auto responseWithoutCurrentTupleJson = responseWithoutCurrentLocation.toJson();
  BOOST_CHECK_EQUAL(
    RepoAvailabilityProbeResponse::fromJson(responseWithoutCurrentTupleJson).toJson(),
    responseWithoutCurrentTupleJson);
  auto responseWithPartialCurrentTuple = responseWithoutCurrentTupleJson;
  const auto partialResponseReportObject =
    responseWithPartialCurrentTuple.find(",\"report\":{");
  BOOST_REQUIRE_NE(partialResponseReportObject, std::string::npos);
  const auto partialResponseReachabilityField =
    responseWithPartialCurrentTuple.find(",\"reachability\":", partialResponseReportObject);
  BOOST_REQUIRE_NE(partialResponseReachabilityField, std::string::npos);
  responseWithPartialCurrentTuple.insert(partialResponseReachabilityField,
    ",\"operationId\":\"operation-7\"");
  BOOST_CHECK_THROW(RepoAvailabilityProbeResponse::fromJson(responseWithPartialCurrentTuple),
                    ArtifactValidationError);
  auto responseWithEmptyCurrentTuple = responseWithoutCurrentTupleJson;
  const auto responseReportObject = responseWithEmptyCurrentTuple.find(",\"report\":{");
  BOOST_REQUIRE_NE(responseReportObject, std::string::npos);
  const auto responseReachabilityField =
    responseWithEmptyCurrentTuple.find(",\"reachability\":", responseReportObject);
  BOOST_REQUIRE_NE(responseReachabilityField, std::string::npos);
  responseWithEmptyCurrentTuple.insert(responseReachabilityField,
    ",\"operationId\":\"\",\"receiptId\":\"\",\"locationVersion\":0");
  BOOST_CHECK_THROW(RepoAvailabilityProbeResponse::fromJson(responseWithEmptyCurrentTuple),
                    ArtifactValidationError);

  auto invalidReason = location;
  invalidReason.reason = std::string(1, static_cast<char>(0xff));
  BOOST_CHECK_THROW(invalidReason.toJson(), ArtifactValidationError);
  auto oversizedReason = location;
  oversizedReason.reason.assign(257, 'x');
  BOOST_CHECK_THROW(oversizedReason.toJson(), ArtifactValidationError);
  location.integrity = "FAILED";
  location.objectPresence = "UNKNOWN";
  BOOST_CHECK_THROW(location.toJson(), ArtifactValidationError);

  TempRoot temporary;
  InAppRepoConfig config;
  config.durableRoot = temporary.path() / "in-app";
  config.ownerId = "spec192-in-app-availability";
  const auto sourcePath = temporary.path() / "source.bin";
  {
    std::ofstream output(sourcePath, std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE(output.good());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    BOOST_REQUIRE(output.good());
  }
  auto api = ArtifactRepositoryApi::makeInApp(config);
  const auto published = api.publishFile(sourcePath.string(), reference);
  BOOST_REQUIRE(published.receipt);
  BOOST_REQUIRE(published.handle);
  const auto historical = api.getReceipt(published.receipt->receiptId,
                                         published.handle);
  BOOST_CHECK_EQUAL(historical.toJson(), published.receipt->toJson());

  const auto metadata = api.checkAvailability(*published.handle, "metadata");
  BOOST_REQUIRE_EQUAL(metadata.locations.size(), 1U);
  BOOST_CHECK_EQUAL(metadata.locations[0].locationKind, "IN_APP");
  BOOST_CHECK(metadata.locations[0].repoNode.empty());
  BOOST_CHECK(metadata.locations[0].providerIdentity.empty());
  BOOST_CHECK_EQUAL(metadata.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(metadata.locations[0].objectPresence, "PRESENT");
  BOOST_CHECK_EQUAL(metadata.locations[0].integrity, "NOT_CHECKED");

  const auto verified = api.checkAvailability(*published.handle, "verify_content");
  BOOST_REQUIRE_EQUAL(verified.locations.size(), 1U);
  BOOST_CHECK_EQUAL(verified.locations[0].integrity, "VERIFIED");

  const auto committedPayload = config.durableRoot / "payloads" /
    reference.digestAlgorithm / reference.contentDigest.substr(0, 2) /
    reference.contentDigest;
  BOOST_REQUIRE(std::filesystem::exists(committedPayload));
  std::filesystem::remove(committedPayload);
  const auto missingObject = api.checkAvailability(*published.handle, "metadata");
  BOOST_REQUIRE_EQUAL(missingObject.locations.size(), 1U);
  BOOST_CHECK_EQUAL(missingObject.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(missingObject.locations[0].objectPresence, "PARTIAL");
  BOOST_CHECK_EQUAL(missingObject.locations[0].integrity, "NOT_CHECKED");

  const auto payloadShard = committedPayload.parent_path();
  std::filesystem::remove(payloadShard);
  {
    std::ofstream shardBlocker(payloadShard, std::ios::binary | std::ios::trunc);
    BOOST_REQUIRE(shardBlocker.good());
    shardBlocker << "not-a-directory";
    BOOST_REQUIRE(shardBlocker.good());
  }
  const auto unobservableObject = api.checkAvailability(*published.handle, "metadata");
  BOOST_REQUIRE_EQUAL(unobservableObject.locations.size(), 1U);
  BOOST_CHECK_EQUAL(unobservableObject.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(unobservableObject.locations[0].objectPresence, "UNKNOWN");
  BOOST_CHECK_EQUAL(unobservableObject.locations[0].integrity, "NOT_CHECKED");
  std::filesystem::remove(payloadShard);

  std::filesystem::remove_all(config.durableRoot);
  const auto missingRoot = api.checkAvailability(*published.handle, "metadata");
  BOOST_REQUIRE_EQUAL(missingRoot.locations.size(), 1U);
  BOOST_CHECK_EQUAL(missingRoot.locations[0].reachability, "UNREACHABLE");
  BOOST_CHECK_EQUAL(missingRoot.locations[0].objectPresence, "UNKNOWN");
  const auto stillHistorical = api.getReceipt(published.receipt->receiptId,
                                              published.handle);
  BOOST_CHECK_EQUAL(stillHistorical.toJson(), published.receipt->toJson());
}

BOOST_AUTO_TEST_CASE(ServerAndClientDefaultsAndOverrideKeepRegisteredStatusPrefix)
{
  BOOST_CHECK_EQUAL(RepoClient::DEFAULT_SERVICE_NAME,
                    "/NDNSF/DistributedRepo/Object/v1");
  PersistentRepoServerConfig serverConfig;
  BOOST_CHECK_EQUAL(serverConfig.servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE), RepoClient::DEFAULT_SERVICE_NAME);
  PersistentRepoClientConfig clientConfig;
  BOOST_CHECK_EQUAL(clientConfig.servicePrefix.toUri(ndn::name::UriFormat::ALTERNATE), RepoClient::DEFAULT_SERVICE_NAME);

  TempRoot temporary;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec192-prefix-override",
                                   "tpm-memory:spec192-prefix-override");
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/provider"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/authority"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
    ndn::Name("/spec192/group"), providerCert, authorityCert,
    std::filesystem::absolute("examples/trust-any.conf").string());

  serverConfig.servicePrefix = ndn::Name("/spec192/custom-repo/v2");
  clientConfig.servicePrefix = serverConfig.servicePrefix;
  StorageCapability capability;
  capability.repoNode = "/logical/custom";
  capability.repoMode = "persistent";
  capability.freeBytes = 4096;
  auto node = std::make_shared<RepoNode>(
    serverConfig.servicePrefix, capability,
    makeTieredRepoStore((temporary.path() / "custom.sqlite3").string(), 4096),
    capability.repoNode, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE));
  node->registerServices(provider);

  const auto customStatus = makeRepoServiceName(clientConfig.servicePrefix, "STATUS");
  BOOST_CHECK_EQUAL(customStatus.toUri(ndn::name::UriFormat::ALTERNATE), "/spec192/custom-repo/v2/STATUS");
  auto response = dispatch(provider, customStatus, {}, "custom-status");
  BOOST_CHECK(response.getStatus());
  const auto defaultStatus = makeRepoServiceName(
    ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), "STATUS");
  response = dispatch(provider, defaultStatus, {}, "wrong-default-status");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo(),
                    "No handler registered for " + defaultStatus.toUri());
}

namespace {

struct OwnedPayloadFixture
{
  TempRoot temporary;
  std::unique_ptr<ndn::Face> ownedFace;
  ndn::Face& face; // Supplied face must outlive the fixture and all callback drains.
  ndn::security::KeyChain keyChain{"pib-memory:spec204-payload", "tpm-memory:spec204-payload"};
  std::unique_ptr<ndn_service_framework::ServiceProvider> provider;
  std::shared_ptr<FilesystemRepoStoreBackend> backend;
  std::shared_ptr<RepoNode> node;
  std::unique_ptr<SqliteArtifactMetadataStore> journal;
  StorageCapability capability;
  std::string metadataPath;

  explicit OwnedPayloadFixture(ndn::Face* suppliedFace = nullptr)
    : ownedFace(suppliedFace ? std::unique_ptr<ndn::Face>{} : std::make_unique<ndn::Face>())
    , face(suppliedFace ? *suppliedFace : *ownedFace)
  {
    (void)makeRsaIdentity(keyChain, ndn::Name("/spec192/provider"));
    (void)makeRsaIdentity(keyChain, ndn::Name("/spec192/authority"));
    capability.repoNode = "/repo/owned-payload"; capability.repoMode = "persistent";
    capability.freeBytes = uint64_t{2} * REPO_USER_STORAGE_LIMIT_BYTES;
    metadataPath = (temporary.path() / "metadata.sqlite3").string();
    reopen();
  }

  static uint64_t now()
  {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  }

  void reopen()
  {
    journal.reset(); node.reset(); backend.reset(); provider.reset();
    provider = std::make_unique<ndn_service_framework::ServiceProvider>(
      ndn_service_framework::ServiceProvider::LocalMockTag{}, face, ndn::Name("/spec192/group"),
      keyChain.getPib().getIdentity(ndn::Name("/spec192/provider")).getDefaultKey().getDefaultCertificate(),
      keyChain.getPib().getIdentity(ndn::Name("/spec192/authority")).getDefaultKey().getDefaultCertificate(),
      std::filesystem::absolute("examples/trust-any.conf").string());
    backend = std::make_shared<FilesystemRepoStoreBackend>(
      (temporary.path() / "payloads").string(), 16 * 1024 * 1024, 1024 * 1024, "owned-payload");
    node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability, backend,
      capability.repoNode, "/spec192/provider", metadataPath);
    node->registerServices(*provider);
    journal = std::make_unique<SqliteArtifactMetadataStore>(metadataPath, true, 16, false);
  }

  ndn_service_framework::ResponseMessage call(const std::string& verb,
                                             const std::vector<uint8_t>& wire)
  {
    return dispatch(*provider, makeRepoServiceName(node->servicePrefix(), verb), wire, verb);
  }

  ArtifactStoreRequest publication(const ArtifactReference& ref, const std::string& owner,
                                    const std::string& key, const std::string& consistency = "ONE",
                                    size_t remoteTargets = 0)
  {
    const auto grant = journal->grantUserStorage(owner, capability.repoNode, "grant-" + key,
      "/spec192/user", artifactStorageObjectName(ref), now() + 60000, now());
    PublishOperationIntent intent;
    intent.schemaVersion = 2; intent.reference = ref; intent.generation = 1;
    intent.idempotencyKeyDigest = digestPublishIdempotencyKey(key);
    intent.seed = {capability.repoNode, "/spec192/provider", "/service/repo"};
    intent.consistency = consistency; intent.targets = {intent.seed};
    const RepoUserStorageBinding binding{1, owner, grant.grantId, grant.ownerEpoch};
    intent.userStorageBindings = {{1, intent.seed, binding, "/spec192/user"}};
    for (size_t i = 0; i < remoteTargets; ++i) {
      const auto suffix = std::to_string(i);
      const RepoArtifactTarget target{"/repo/remote-" + suffix,
        "/provider/remote-" + suffix, "/service/remote-" + suffix};
      intent.targets.push_back(target);
      // Remote bindings define selected intent only; this fixture does not
      // authenticate a remote grant or perform network replica writes.
      intent.userStorageBindings.push_back({1, target,
        {1, owner, "remote-grant-" + suffix, 1}, "/spec192/provider"});
    }
    intent.requestDigest = makePublishRequestDigest(ref, 1, intent.seed, consistency,
      intent.targets, intent.userStorageBindings);
    const auto json = intent.toJson();
    const auto response = call("STORE", std::vector<uint8_t>(json.begin(), json.end()));
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    ArtifactStoreRequest write;
    write.schemaVersion = 3; write.operationId = responseString(response);
    write.generation = 1; write.reference = ref; write.storageBinding = binding;
    return write;
  }

  static ArtifactRangeWriteRequest range(const ArtifactStoreRequest& write,
                                         uint64_t offset, std::vector<uint8_t> bytes)
  {
    ArtifactRangeWriteRequest result;
    result.schemaVersion = 3; result.operationId = write.operationId;
    result.generation = write.generation; result.reference = write.reference;
    result.storageBinding = write.storageBinding; result.offsetBytes = offset;
    result.lengthBytes = bytes.size(); result.bytes = std::move(bytes);
    return result;
  }

  static ArtifactRangeCommitRequest commit(const ArtifactStoreRequest& write)
  {
    ArtifactRangeCommitRequest result;
    result.schemaVersion = 3; result.operationId = write.operationId;
    result.generation = write.generation; result.reference = write.reference;
    result.storageBinding = write.storageBinding;
    return result;
  }

  static ArtifactRangeAbortRequest abort(const ArtifactStoreRequest& write)
  {
    ArtifactRangeAbortRequest result;
    result.schemaVersion = 2; result.operationId = write.operationId;
    result.generation = write.generation; result.reference = write.reference;
    result.storageBinding = write.storageBinding;
    return result;
  }

  std::filesystem::path onlyManifest() const
  {
    const auto root = temporary.path() / "payloads" / "manifests";
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(root))
      if (entry.is_regular_file() && entry.path().extension() == ".json") paths.push_back(entry.path());
    if (paths.size() != 1) throw std::runtime_error("fixture requires exactly one physical manifest");
    return paths.front();
  }

  static std::string read(const std::filesystem::path& path)
  {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  uint64_t sqlCharge(const std::string& owner, bool reserved) const
  {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(metadataPath.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
      if (raw) sqlite3_close(raw);
      throw std::runtime_error("fixture quota SQL open failed");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    const char* sql = reserved
      ? "SELECT COALESCE(SUM(reserved_bytes),0) FROM repo_user_storage_writes WHERE repo_node=? AND owner_identity=? AND state='Reserved'"
      : "SELECT COALESCE(SUM(payload_wire_bytes+manifest_bytes),0) FROM repo_user_storage_claims WHERE repo_node=? AND owner_identity=? AND state<>'Deleted'";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
      throw std::runtime_error("fixture quota SQL prepare failed");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
    if (sqlite3_bind_text(query.get(), 1, capability.repoNode.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(query.get(), 2, owner.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(query.get()) != SQLITE_ROW)
      throw std::runtime_error("fixture quota SQL read failed");
    return static_cast<uint64_t>(sqlite3_column_int64(query.get(), 0));
  }

  void replacePublishSnapshot(const std::string& operationId, const PublishOperationStatus& value)
  {
    sqlite3* raw = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_open_v2(metadataPath.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    sqlite3_stmt* statement = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_prepare_v2(database.get(),
      "UPDATE artifact_publish_operations SET status_json=? WHERE operation_id=?", -1,
      &statement, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
    const auto json = value.toJson();
    BOOST_REQUIRE_EQUAL(sqlite3_bind_text(query.get(), 1, json.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    BOOST_REQUIRE_EQUAL(sqlite3_bind_text(query.get(), 2, operationId.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    BOOST_REQUIRE_EQUAL(sqlite3_step(query.get()), SQLITE_DONE);
    BOOST_REQUIRE_EQUAL(sqlite3_changes(database.get()), 1);
  }

  bool persistedReady() const
  {
    sqlite3* raw = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_open_v2(metadataPath.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    sqlite3_stmt* statement = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_prepare_v2(database.get(),
      "SELECT ready FROM repo_user_storage_state WHERE repo_node=?", -1, &statement, nullptr), SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
    BOOST_REQUIRE_EQUAL(sqlite3_bind_text(query.get(), 1, capability.repoNode.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    BOOST_REQUIRE_EQUAL(sqlite3_step(query.get()), SQLITE_ROW);
    return sqlite3_column_int(query.get(), 0) != 0;
  }
};

} // namespace

BOOST_AUTO_TEST_CASE(OwnerDeleteCleansUnacceptedIntentStageWithoutInventingAcceptance)
{
  OwnedPayloadFixture f;
  const auto write = f.publication(makeReference("delete-accepted-stage", {1, 2, 3, 4}),
    "/spec192/user", "delete-accepted-stage");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charge = f.sqlCharge("/spec192/user", true);
  BOOST_REQUIRE_GT(charge, 0U);
  const auto request = toBytes(RepoUserStorageDeleteRequest{1, "cleanup",
    artifactStorageObjectName(write.reference), 1}.toJson());
  response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto deleted = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(deleted.state, "DELETED"); BOOST_CHECK_EQUAL(deleted.releasedBytes, charge);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), 0U);
  BOOST_CHECK(std::filesystem::is_empty(f.temporary.path() / "payloads" / "staging"));
  const auto terminal = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
  BOOST_REQUIRE(terminal); BOOST_CHECK_EQUAL(terminal->state, "PREPARING");
  BOOST_CHECK(terminal->aggregateReceiptJson.empty());
  BOOST_CHECK_EQUAL(terminal->acceptedAtMs, 0U);
  BOOST_CHECK(f.journal->recoverablePublishOperations().empty());
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, {1, 2}).toWire());
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
  BOOST_CHECK(std::filesystem::is_empty(f.temporary.path() / "payloads" / "staging"));
  response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(responseString(response)).releasedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(OwnerDeletePreservesActualSeedReceiptAndUnknownTargetsForEveryPolicy)
{
  for (const std::string policy : {"ONE", "QUORUM", "ALL"}) {
    OwnedPayloadFixture f;
    const std::vector<uint8_t> bytes{1, 2, 3, 4};
    auto write = f.publication(makeReference("delete-policy-" + policy, bytes),
      "/spec192/user", "delete-policy-" + policy, policy, 2);
    write.bytes = bytes;
    auto response = f.call("STORE", write.toWire());
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    const auto seedReceipt = RepoArtifactCommitReceipt::fromJson(responseString(response));
    auto accepted = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
    BOOST_REQUIRE(accepted);
    auto attempted = accepted->status.targets.at(1);
    attempted.state = "IN_FLIGHT"; attempted.attemptCount = 2;
    attempted.updatedAtMs = OwnedPayloadFixture::now();
    f.journal->updatePublishOperationTarget(write.operationId, attempted, attempted.updatedAtMs);
    const auto charge = f.sqlCharge("/spec192/user", false);
    response = f.call("USER_STORAGE_DELETE", toBytes(RepoUserStorageDeleteRequest{1, "cleanup",
      artifactStorageObjectName(write.reference), 1}.toJson()));
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    const auto deleted = RepoUserStorageDeleteReply::fromJson(responseString(response));
    BOOST_CHECK_EQUAL(deleted.state, "DELETED"); BOOST_CHECK_EQUAL(deleted.releasedBytes, charge);
    const auto terminal = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
    BOOST_REQUIRE(terminal);
    BOOST_CHECK_EQUAL(terminal->state, policy == "ONE" ? "COMMITTED" : "PARTIAL");
    const auto receipt = ArtifactCommitReceipt::fromJson(terminal->aggregateReceiptJson);
    BOOST_CHECK_EQUAL(receipt.requiredAcks, policy == "ONE" ? 1U : policy == "QUORUM" ? 2U : 3U);
    BOOST_CHECK_EQUAL(receipt.achievedReplicas, 1U);
    BOOST_REQUIRE_EQUAL(receipt.replicaCommits.size(), 3U);
    BOOST_CHECK_EQUAL(receipt.replicaCommits[0].state, "COMMITTED");
    BOOST_CHECK_EQUAL(receipt.replicaCommits[0].receiptId, seedReceipt.receiptId);
    BOOST_REQUIRE(terminal->status.targets[0].receipt);
    BOOST_CHECK_EQUAL(terminal->status.targets[0].receipt->toJson(), seedReceipt.toJson());
    BOOST_CHECK_EQUAL(terminal->status.targets[1].state, "UNKNOWN");
    BOOST_CHECK_EQUAL(receipt.replicaCommits[1].failureReason, "repo-target-outcome-unknown");
    BOOST_CHECK_EQUAL(terminal->status.targets[2].state, "NOT_ATTEMPTED");
    BOOST_CHECK_EQUAL(receipt.replicaCommits[2].state, "SKIPPED");
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
    BOOST_CHECK(!f.node->core().has(artifactStorageObjectName(write.reference)));
    BOOST_CHECK(f.journal->recoverablePublishOperations().empty());
    // Trusted retry demonstrates journal terminal immutability; no remote work
    // or remote committed receipt is fabricated by this native fixture.
    BOOST_CHECK_THROW(f.journal->updatePublishOperationTarget(write.operationId,
      attempted, OwnedPayloadFixture::now()), std::runtime_error);
    BOOST_CHECK_EQUAL(f.journal->lookupPublishOperation(write.operationId,
      "/spec192/user")->aggregateReceiptJson, terminal->aggregateReceiptJson);
  }
}

BOOST_AUTO_TEST_CASE(OwnerDeleteLeavesAnotherOwnersAcceptedPublicationAndBytesUntouched)
{
  OwnedPayloadFixture f;
  const auto reference = makeReference("delete-peer-publication", {1, 2, 3, 4});
  auto peer = f.publication(reference, "/user/v", "peer-publication", "ALL", 2);
  peer.bytes = {1, 2, 3, 4};
  auto response = f.call("STORE", peer.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto peerCharge = f.sqlCharge("/user/v", false);
  const auto before = f.journal->lookupPublishOperation(peer.operationId, "/spec192/user");
  BOOST_REQUIRE(before); BOOST_REQUIRE_EQUAL(before->state, "ACCEPTED");
  const auto own = f.publication(reference, "/spec192/user", "own-publication");
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(own, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charge = f.sqlCharge("/spec192/user", true);
  const auto objectName = artifactStorageObjectName(reference);
  const auto physical = f.node->core().getRange(objectName, {0, 4});
  response = f.call("USER_STORAGE_DELETE", toBytes(RepoUserStorageDeleteRequest{1, "cleanup",
    artifactStorageObjectName(reference), 1}.toJson()));
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto pending = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(pending.state, "PENDING"); BOOST_CHECK_EQUAL(pending.releasedBytes, 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charge);
  BOOST_CHECK(f.node->core().getRange(objectName, {0, 4}) == physical);
  BOOST_CHECK_EQUAL(f.journal->lookupPublishOperation(peer.operationId,
    "/spec192/user")->status.toJson(), before->status.toJson());
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/v", false), peerCharge);
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/v", true), 0U);
}

BOOST_AUTO_TEST_CASE(OwnedPublishWorkerKeepsProvenSeedReceiptWhenCleanupFencesReads)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  auto write = f.publication(makeReference("fenced-worker", bytes), "/spec192/user",
    "fenced-worker", "ALL", 2);
  write.bytes = bytes;
  auto response = f.call("STORE", write.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto seedReceipt = RepoArtifactCommitReceipt::fromJson(responseString(response));
  const auto objectName = artifactStorageObjectName(write.reference);
  const auto charge = f.sqlCharge("/spec192/user", false);
  // Trusted setup fences before the real worker starts, so its initial seed
  // validation must fail without sending or inventing remote replica outcomes.
  (void)f.journal->prepareUserStorageCleanup("/spec192/user", f.capability.repoNode,
    "cleanup", objectName, 1, OwnedPayloadFixture::now());
  auto publisher = std::make_shared<ndn_service_framework::ServiceUser>(
    ndn_service_framework::ServiceUser::LocalMockTag{}, f.face, ndn::Name("/spec192/group"),
    f.keyChain.getPib().getIdentity(ndn::Name("/spec192/provider")).getDefaultKey().getDefaultCertificate(),
    f.keyChain.getPib().getIdentity(ndn::Name("/spec192/authority")).getDefaultKey().getDefaultCertificate(),
    std::filesystem::absolute("examples/trust-any.conf").string());
  f.node->registerServices(*f.provider, publisher, 1, 100);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  auto terminal = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
  while (terminal && terminal->state != "PARTIAL" && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    terminal = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
  }
  BOOST_REQUIRE(terminal); BOOST_REQUIRE_EQUAL(terminal->state, "PARTIAL");
  const auto receipt = ArtifactCommitReceipt::fromJson(terminal->aggregateReceiptJson);
  BOOST_CHECK_EQUAL(receipt.achievedReplicas, 1U); BOOST_CHECK_EQUAL(receipt.requiredAcks, 3U);
  BOOST_REQUIRE(terminal->status.targets.front().receipt);
  BOOST_CHECK_EQUAL(terminal->status.targets.front().receipt->toJson(), seedReceipt.toJson());
  for (size_t i = 1; i < terminal->status.targets.size(); ++i) {
    BOOST_CHECK_EQUAL(terminal->status.targets[i].state, "NOT_ATTEMPTED");
    BOOST_CHECK_EQUAL(terminal->status.targets[i].attemptCount, 0U);
    BOOST_CHECK_EQUAL(receipt.replicaCommits[i].state, "SKIPPED");
  }
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charge);
  BOOST_CHECK(f.node->core().getRange(objectName, {0, bytes.size()}) == bytes);
  // Explicit join before local publisher/Face dependencies can be destroyed.
  f.provider.reset();
  f.node.reset();
}

BOOST_AUTO_TEST_CASE(OwnedAvailabilityRequiresLiveOwnerEpochAcrossSharedCleanupAndRestart)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{4, 3, 2, 1};
  const auto reference = makeReference("owned-availability", bytes);
  const auto objectName = artifactStorageObjectName(reference);
  auto own = f.publication(reference, "/spec192/user", "availability-u");
  own.bytes = bytes;
  auto response = f.call("STORE", own.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  auto peer = f.publication(reference, "/spec192/other", "availability-v");
  peer.bytes = bytes;
  response = f.call("STORE", peer.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());

  // Actual ONE seed-only worker finalization avoids synthetic terminal receipts.
  auto publisher = std::make_shared<ndn_service_framework::ServiceUser>(
    ndn_service_framework::ServiceUser::LocalMockTag{}, f.face, ndn::Name("/spec192/group"),
    f.keyChain.getPib().getIdentity(ndn::Name("/spec192/provider")).getDefaultKey().getDefaultCertificate(),
    f.keyChain.getPib().getIdentity(ndn::Name("/spec192/authority")).getDefaultKey().getDefaultCertificate(),
    std::filesystem::absolute("examples/trust-any.conf").string());
  struct JoinOwners {
    OwnedPayloadFixture& fixture;
    std::shared_ptr<ndn_service_framework::ServiceUser>& publisher;
    ~JoinOwners() { fixture.provider.reset(); fixture.node.reset(); publisher.reset(); }
  } join{f, publisher};
  f.node->registerServices(*f.provider, publisher, 1, 100);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool completed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto u = f.journal->lookupPublishOperation(own.operationId, "/spec192/user");
    const auto v = f.journal->lookupPublishOperation(peer.operationId, "/spec192/user");
    if (u && v && u->state == "COMMITTED" && v->state == "COMMITTED") { completed = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  BOOST_REQUIRE(completed);
  const auto ownCharge = f.sqlCharge("/spec192/user", false);
  const auto peerCharge = f.sqlCharge("/spec192/other", false);
  BOOST_CHECK_GT(ownCharge, bytes.size()); BOOST_CHECK_EQUAL(peerCharge, ownCharge);
  auto currentRequest = [&](uint64_t epoch) {
    const auto locationReply = f.call("STATUS", toBytes(ArtifactLocationRequest{1, reference}.toJson()));
    BOOST_REQUIRE_MESSAGE(locationReply.getStatus(), locationReply.getErrorInfo());
    const auto location = ArtifactLocationRecord::fromJson(responseString(locationReply));
    RepoAvailabilityProbeRequest query;
    query.schemaVersion = 2; query.ownerEpoch = epoch; query.reference = reference;
    query.expectedLocation = {location.repoNode, location.providerIdentity, location.operationId,
      location.receiptId, location.generation, location.locationVersion};
    query.probeKind = "verify_content";
    return query;
  };
  auto observe = [&](const RepoAvailabilityProbeRequest& query, const std::string& actor) {
    const auto payload = toBytes(query.toJson());
    ndn_service_framework::RequestMessage request;
    ndn::Buffer buffer(payload.data(), payload.size()); request.setPayload(buffer, buffer.size());
    const auto reply = f.provider->dispatchRequest(ndn::Name(actor), f.provider->getName(),
      makeRepoServiceName(f.node->servicePrefix(), "STATUS"), ndn::Name("/spec192/request/owned-probe"), request);
    BOOST_REQUIRE_MESSAGE(reply.getStatus(), reply.getErrorInfo());
    auto result = RepoAvailabilityProbeResponse::fromJson(responseString(reply));
    result.validateExpected(query, actor);
    return result;
  };
  auto assertUnavailable = [](const RepoAvailabilityProbeResponse& result) {
    BOOST_CHECK_EQUAL(result.report.objectPresence, "UNKNOWN");
    BOOST_CHECK_EQUAL(result.report.integrity, "NOT_CHECKED");
    BOOST_CHECK(result.report.operationId.empty());
    BOOST_CHECK_EQUAL(result.report.validUntilMs, result.report.observedAtMs);
  };
  auto query = currentRequest(own.storageBinding->ownerEpoch);
  auto available = observe(query, "/spec192/user");
  BOOST_CHECK_EQUAL(available.report.objectPresence, "PRESENT");
  BOOST_CHECK_EQUAL(available.report.integrity, "VERIFIED");
  auto oldQuery = query; ++oldQuery.ownerEpoch;
  assertUnavailable(observe(oldQuery, "/spec192/user"));
  assertUnavailable(observe(query, "/spec192/unrelated"));
  BOOST_CHECK_THROW(f.node->handleStatus(toBytes(query.toJson())), std::runtime_error);
  const auto cleanup = f.journal->prepareUserStorageCleanup("/spec192/user", f.capability.repoNode,
    "availability-delete-u", objectName, own.storageBinding->ownerEpoch, f.now());
  BOOST_CHECK(cleanup.state == RepoUserStorageClaimState::CleanupPrepared);
  assertUnavailable(observe(query, "/spec192/user"));
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), ownCharge);
  auto peerQuery = currentRequest(peer.storageBinding->ownerEpoch);
  BOOST_CHECK_EQUAL(observe(peerQuery, "/spec192/other").report.integrity, "VERIFIED");
  response = f.call("USER_STORAGE_DELETE", toBytes(RepoUserStorageDeleteRequest{1,
    "availability-delete-u", objectName, own.storageBinding->ownerEpoch}.toJson()));
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(responseString(response)).state, "DELETED");
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/other", false), peerCharge);
  BOOST_CHECK(f.node->core().getRange(objectName, {0, bytes.size()}) == bytes);
  f.reopen();
  query = currentRequest(own.storageBinding->ownerEpoch);
  assertUnavailable(observe(query, "/spec192/user"));
  BOOST_CHECK_EQUAL(observe(currentRequest(peer.storageBinding->ownerEpoch), "/spec192/other").report.integrity, "VERIFIED");
  auto fresh = f.publication(reference, "/spec192/user", "availability-u-new"); fresh.bytes = bytes;
  BOOST_CHECK_GT(fresh.storageBinding->ownerEpoch, own.storageBinding->ownerEpoch);
  response = f.call("STORE", fresh.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(observe(currentRequest(fresh.storageBinding->ownerEpoch), "/spec192/user").report.integrity, "VERIFIED");
  assertUnavailable(observe(currentRequest(own.storageBinding->ownerEpoch), "/spec192/user"));
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/other", false), peerCharge);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), ownCharge);
}

BOOST_AUTO_TEST_CASE(OwnerDeleteFencesBusyWorkerAndDrainsAbandonedSelectedRequest)
{
  boost::asio::io_context ioContext;
  ndn::security::KeyChain faceKeys("pib-memory:spec204-busy-face", "tpm-memory:spec204-busy-face");
  ndn::DummyClientFace face(ioContext, faceKeys);
  OwnedPayloadFixture f(&face);
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  auto write = f.publication(makeReference("busy-cleanup-worker", bytes),
    "/spec192/user", "busy-cleanup-worker", "ALL", 2);
  write.bytes = bytes;
  auto response = f.call("STORE", write.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto seedReceipt = RepoArtifactCommitReceipt::fromJson(responseString(response));
  const auto objectName = artifactStorageObjectName(write.reference);
  const auto charge = f.sqlCharge("/spec192/user", false);
  auto publications = std::make_shared<std::atomic<size_t>>(0);
  auto publisher = std::make_shared<ndn_service_framework::ServiceUser>(
    ndn_service_framework::ServiceUser::LocalMockTag{}, f.face, ndn::Name("/spec192/group"),
    f.keyChain.getPib().getIdentity(ndn::Name("/spec192/provider")).getDefaultKey().getDefaultCertificate(),
    f.keyChain.getPib().getIdentity(ndn::Name("/spec192/authority")).getDefaultKey().getDefaultCertificate(),
    std::filesystem::absolute("examples/trust-any.conf").string());
  publisher->useSigningKeyChainForSigningOnlyForTest(f.keyChain);
  publisher->setLocalPublicationHandler([publications] (const ndn::Name&, const ndn::Buffer&) {
    publications->fetch_add(1, std::memory_order_relaxed);
  });
  struct DrainPublisher {
    OwnedPayloadFixture& fixture;
    std::shared_ptr<ndn_service_framework::ServiceUser>& publisher;
    bool joined = false;
    void joinAndDrain() {
      if (joined) return;
      fixture.provider.reset(); fixture.node.reset();
      auto& io = fixture.face.getIoContext();
      io.restart(); io.poll();
      joined = true;
    }
    ~DrainPublisher() {
      try {
        joinAndDrain(); publisher.reset();
        auto& io = fixture.face.getIoContext();
        io.restart(); io.poll();
      }
      catch (const std::exception& error) {
        BOOST_ERROR("worker fixture drain failed: " << error.what());
      }
    }
  } drain{f, publisher};
  // Face/keychain are fixture members; counters and publisher outlive Node join
  // and the delayed-start drain. Do not run I/O until request timeout below.
  const auto postsBefore = publisher->getIoPostCountForTest();
  f.node->registerServices(*f.provider, publisher, 1, 2000);
  const auto activeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (publisher->getIoPostCountForTest() == postsBefore &&
         std::chrono::steady_clock::now() < activeDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  BOOST_REQUIRE_EQUAL(publisher->getIoPostCountForTest(), postsBefore + 1);
  auto active = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
  BOOST_REQUIRE(active); BOOST_REQUIRE_EQUAL(active->status.targets[1].state, "IN_FLIGHT");
  const auto request = toBytes(RepoUserStorageDeleteRequest{1, "cleanup", objectName, 1}.toJson());
  response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto pending = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(pending.state, "PENDING"); BOOST_CHECK_EQUAL(pending.releasedBytes, 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charge);
  BOOST_CHECK(f.node->core().getRange(objectName, {0, 4}) == bytes);
  BOOST_CHECK_THROW(f.journal->validateUserStorageGrant("/spec192/user", f.capability.repoNode,
    "/spec192/user", write.storageBinding->grantId, objectName, 1, OwnedPayloadFixture::now()), std::runtime_error);
  const auto terminalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto terminal = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
  while (terminal && terminal->state != "PARTIAL" && std::chrono::steady_clock::now() < terminalDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    terminal = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
  }
  BOOST_REQUIRE(terminal); BOOST_REQUIRE_EQUAL(terminal->state, "PARTIAL");
  BOOST_REQUIRE(terminal->status.targets.front().receipt);
  BOOST_CHECK_EQUAL(terminal->status.targets.front().receipt->toJson(), seedReceipt.toJson());
  BOOST_CHECK_EQUAL(terminal->status.targets[1].state, "UNKNOWN");
  BOOST_CHECK_EQUAL(terminal->status.targets[1].failureCode, "repo-target-outcome-unknown");
  BOOST_CHECK_EQUAL(terminal->status.targets[2].state, "NOT_ATTEMPTED");
  BOOST_CHECK_EQUAL(terminal->status.targets[2].attemptCount, 0U);
  RepoUserStorageDeleteReply deleted;
  do {
    response = f.call("USER_STORAGE_DELETE", request);
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    deleted = RepoUserStorageDeleteReply::fromJson(responseString(response));
    if (deleted.state == "DELETED") break;
    BOOST_CHECK_EQUAL(deleted.releasedBytes, 0U);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < terminalDeadline);
  BOOST_REQUIRE_EQUAL(deleted.state, "DELETED"); BOOST_CHECK_EQUAL(deleted.releasedBytes, charge);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
  BOOST_CHECK(!f.node->core().has(objectName));
  response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(responseString(response)).releasedBytes, 0U);
  drain.joinAndDrain(); // Also guaranteed on any fatal fixture assertion.
  auto& io = f.face.getIoContext();
  BOOST_CHECK_EQUAL(publications->load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EQUAL(publisher.use_count(), 1L);
  publisher.reset();
  io.restart(); io.poll(); // Face and keychain remain alive through destruction work.
}

BOOST_AUTO_TEST_CASE(OwnerDeleteRejectsMismatchedPublicationSnapshotBeforePhysicalMutation)
{
  OwnedPayloadFixture f;
  auto write = f.publication(makeReference("corrupt-cleanup-publish", {1, 2, 3, 4}),
    "/spec192/user", "corrupt-cleanup-publish");
  write.bytes = {1, 2, 3, 4};
  auto response = f.call("STORE", write.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto accepted = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
  BOOST_REQUIRE(accepted); BOOST_REQUIRE_EQUAL(accepted->state, "ACCEPTED");
  const auto charge = f.sqlCharge("/spec192/user", false);
  const auto objectName = artifactStorageObjectName(write.reference);
  const auto physical = f.node->core().getRange(objectName, {0, 4});
  const auto replaceSnapshot = [&f, &write] (const PublishOperationStatus& value) {
    f.replacePublishSnapshot(write.operationId, value);
  };
  const auto request = toBytes(RepoUserStorageDeleteRequest{1, "cleanup",
    artifactStorageObjectName(write.reference), 1}.toJson());
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto corrupt = accepted->status;
    if (mutation == 0) corrupt.reference.logicalName += "/other";
    if (mutation == 1) ++corrupt.generation;
    if (mutation == 2) corrupt.consistency = "ALL";
    // Deliberate coherent snapshot corruption, not another committed write:
    // keep the copied receipt parseable so the intent cross-check is exercised.
    for (auto& target : corrupt.targets) {
      if (target.receipt) {
        target.receipt->reference = corrupt.reference;
        target.receipt->generation = corrupt.generation;
      }
    }
    replaceSnapshot(corrupt);
    response = f.call("USER_STORAGE_DELETE", request);
    BOOST_CHECK(!response.getStatus());
    BOOST_CHECK_NE(response.getErrorInfo().find("publication identity mismatch"), std::string::npos);
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charge);
    BOOST_CHECK(f.node->core().getRange(objectName, {0, 4}) == physical);
    replaceSnapshot(accepted->status);
  }
  response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(responseString(response)).state, "DELETED");
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
}

BOOST_AUTO_TEST_CASE(BoundPayloadQuotaAccountsAndRejectionBeforeMutation)
{
  OwnedPayloadFixture f;
  auto largeRef = makeReference("large-pending", {1});
  largeRef.sizeBytes = REPO_USER_STORAGE_LIMIT_BYTES - 4096;
  auto large = f.publication(largeRef, "/user/u", "large");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(large, 0, {1}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto usage = f.journal->userStorageUsage("/user/u", f.capability.repoNode);
  BOOST_CHECK_GT(usage.reservedBytes, largeRef.sizeBytes);
  BOOST_CHECK_LE(usage.reservedBytes, REPO_USER_STORAGE_LIMIT_BYTES);
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), usage.reservedBytes);
  const std::vector<uint8_t> bytes(8192, 2);
  const auto smallRef = makeReference("small", bytes);
  auto denied = f.publication(smallRef, "/user/u", "denied"); denied.bytes = bytes;
  response = f.call("STORE", denied.toWire());
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("QUOTA_EXCEEDED"), 0U);
  BOOST_CHECK(!f.node->core().has(artifactStorageObjectName(smallRef)));
  BOOST_CHECK(!f.journal->lookupUserStorageWrite(f.capability.repoNode, "/user/u", denied.operationId));
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes, usage.reservedBytes);
  BOOST_CHECK_EQUAL(countPersistentReceipts(f.metadataPath, smallRef), 0U);
  auto permitted = f.publication(smallRef, "/user/v", "other-user"); permitted.bytes = bytes;
  response = f.call("STORE", permitted.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charged = bytes.size() + OwnedPayloadFixture::read(f.onlyManifest()).size();
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/v", f.capability.repoNode).committedBytes, charged);
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/v", false), charged);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/v", f.capability.repoNode).reservedBytes, 0U);
  const auto accepted = f.journal->lookupPublishOperation(permitted.operationId, "/spec192/user");
  BOOST_REQUIRE(accepted);
  BOOST_CHECK_EQUAL(accepted->status.schemaVersion, 2U);
  BOOST_REQUIRE_EQUAL(accepted->status.userStorageBindings.size(), 1U);
  BOOST_CHECK_EQUAL(accepted->status.userStorageBindings.front().binding.toJson(), permitted.storageBinding->toJson());
  response = f.call("STORE", permitted.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/v", f.capability.repoNode).committedBytes, charged);
  auto spoof = permitted; spoof.storageBinding = denied.storageBinding;
  response = f.call("STORE", spoof.toWire());
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-user-storage-intent-mismatch"), 0U);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(large).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes, 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/v", f.capability.repoNode).committedBytes, charged);
}

namespace {
std::atomic<int> ownedCommitFsyncFailures{0};
std::atomic<size_t> ownedCommitFsyncAttempts{0};
int ownedCommitFsync(int fd)
{
  struct stat metadata{};
  if (::fstat(fd, &metadata) == 0 && S_ISDIR(metadata.st_mode)) {
    ++ownedCommitFsyncAttempts;
    const auto failures = ownedCommitFsyncFailures.load();
    if (failures != 0) {
      if (failures > 0) --ownedCommitFsyncFailures;
      errno = EIO;
      return -1;
    }
  }
  return ::fsync(fd);
}
struct OwnedCommitFsyncScope
{
  detail::FilesystemRepoStoreIoHooks previous;
  explicit OwnedCommitFsyncScope(int failures)
  {
    ownedCommitFsyncFailures = failures;
    ownedCommitFsyncAttempts = 0;
    previous = detail::installFilesystemRepoStoreIoHooks({ownedCommitFsync, nullptr, nullptr});
  }
  ~OwnedCommitFsyncScope() { detail::installFilesystemRepoStoreIoHooks(previous); }
};
struct OwnedRollbackPermissions
{
  std::filesystem::path path;
  std::filesystem::perms original;
  explicit OwnedRollbackPermissions(std::filesystem::path value)
    : path(std::move(value)), original(std::filesystem::status(path).permissions()) {}
  ~OwnedRollbackPermissions() {
    std::error_code error;
    std::filesystem::permissions(path, original, error);
  }
};
}

BOOST_AUTO_TEST_CASE(BoundCommitTransientFsyncRecoverySettlesBothReservations)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  const auto write = f.publication(makeReference("transient-owned-sync", bytes),
    "/spec192/user", "transient-owned-sync");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, bytes).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  {
    OwnedCommitFsyncScope faults(1);
    response = f.call("COMMIT_RANGES", OwnedPayloadFixture::commit(write).toWire());
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    BOOST_CHECK_EQUAL(ownedCommitFsyncAttempts.load(), 2U);
  }
  const auto objectName = artifactStorageObjectName(write.reference);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), bytes.size() + OwnedPayloadFixture::read(f.onlyManifest()).size());
  auto replacement = f.node->core().getManifest(objectName);
  replacement.operationId = "settled-replacement";
  ++replacement.generation;
  const std::vector<uint8_t> next{5, 6, 7, 8};
  replacement.sha256 = sha256Hex(next);
  BOOST_CHECK_NO_THROW(f.node->core().putRange(replacement, {0, next.size()}, next));
  BOOST_CHECK(f.node->core().abortFailedRangesIfOwned(replacement));
  BOOST_CHECK(f.node->core().getRange(objectName, {0, bytes.size()}) == bytes);
}

BOOST_AUTO_TEST_CASE(BoundCommitReplayRejectsStoredByteDriftBeforeRewritingMetadata)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  auto write = f.publication(makeReference("owned-commit-byte-drift", bytes),
    "/spec192/user", "owned-commit-byte-drift");
  write.bytes = bytes;
  auto response = f.call("STORE", write.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charged = f.sqlCharge("/spec192/user", false);
  const auto path = f.onlyManifest();
  { std::ofstream output(path, std::ios::app); output << '\n'; BOOST_REQUIRE(output.good()); }
  const auto grown = OwnedPayloadFixture::read(path);
  response = f.call("COMMIT_RANGES", OwnedPayloadFixture::commit(write).toWire());
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-user-storage-inventory-conflict"), 0U);
  BOOST_CHECK(OwnedPayloadFixture::read(path) == grown);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charged);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), 0U);
}

BOOST_AUTO_TEST_CASE(BoundCommitPersistentFsyncKeepsQuotaUntilActualCommitRetry)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  const auto write = f.publication(makeReference("persistent-owned-sync", bytes),
    "/spec192/user", "persistent-owned-sync");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, bytes).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto reserved = f.sqlCharge("/spec192/user", true);
  {
    OwnedCommitFsyncScope faults(-1);
    response = f.call("COMMIT_RANGES", OwnedPayloadFixture::commit(write).toWire());
    BOOST_CHECK(!response.getStatus());
    BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-file-ambiguous-commit"), 0U);
    BOOST_CHECK_GE(ownedCommitFsyncAttempts.load(), 2U);
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), reserved);
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
    response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    BOOST_CHECK_EQUAL(responseString(response), "pending");
    response = f.call("COMMIT_RANGES", OwnedPayloadFixture::commit(write).toWire());
    BOOST_CHECK(!response.getStatus());
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), reserved);
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
  }
  response = f.call("COMMIT_RANGES", OwnedPayloadFixture::commit(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charged = bytes.size() + OwnedPayloadFixture::read(f.onlyManifest()).size();
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charged);
  response = f.call("COMMIT_RANGES", OwnedPayloadFixture::commit(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charged);
  BOOST_CHECK(f.node->core().getRange(artifactStorageObjectName(write.reference), {0, bytes.size()}) == bytes);
}

BOOST_AUTO_TEST_CASE(BoundFinalizedResidueRetainsChargeUntilOwnerDeleteConfirmation)
{
  BOOST_REQUIRE_NE(::geteuid(), 0U);
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  const auto write = f.publication(makeReference("owned-finalized-residue", bytes),
    "/spec192/user", "owned-finalized-residue");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, bytes).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto reserved = f.sqlCharge("/spec192/user", true);
  const auto objectName = artifactStorageObjectName(write.reference);
  const auto payload = f.temporary.path() / "payloads" / "payloads" / "sha256" /
    write.reference.contentDigest.substr(0, 2) / write.reference.contentDigest;
  {
    OwnedRollbackPermissions restore(f.temporary.path() / "payloads" / "manifests");
    std::filesystem::permissions(restore.path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    response = f.call("COMMIT_RANGES", OwnedPayloadFixture::commit(write).toWire());
    BOOST_CHECK(!response.getStatus());
    BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-file-metadata-write-failed"), 0U);
    BOOST_REQUIRE(std::filesystem::is_regular_file(payload));
    BOOST_CHECK_EQUAL(std::filesystem::file_size(payload), bytes.size());
    BOOST_CHECK(!f.node->core().has(objectName));
    response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    BOOST_CHECK_EQUAL(responseString(response), "pending");
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), reserved);
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
    BOOST_CHECK_EQUAL(f.node->core().reapExpiredRangeReservations(std::chrono::steady_clock::duration::zero()), 0U);
  }
  const auto request = toBytes(RepoUserStorageDeleteRequest{1, "delete-residue", objectName,
    write.storageBinding->ownerEpoch}.toJson());
  response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  auto reply = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(reply.state, "DELETED");
  BOOST_CHECK_EQUAL(reply.releasedBytes, reserved);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
  BOOST_CHECK(!std::filesystem::exists(payload));
  response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  reply = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(reply.state, "DELETED");
  BOOST_CHECK_EQUAL(reply.releasedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(BoundRangeWriteFailureRetainsQuotaUntilConfirmedRollback)
{
  BOOST_REQUIRE_NE(::geteuid(), 0U);
  OwnedPayloadFixture f;
  const auto write = f.publication(makeReference("owned-range-io-failure", {1, 2, 3, 4}),
    "/spec192/user", "owned-range-io-failure");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto reserved = f.sqlCharge("/spec192/user", true);
  const auto directory = f.temporary.path() / "payloads" / "staging";
  const auto stage = directory / (write.reference.contentDigest + ".1.part");
  const auto original = OwnedPayloadFixture::read(stage);
  {
    OwnedRollbackPermissions restoreDirectory(directory), restoreFile(stage);
    std::filesystem::permissions(stage, std::filesystem::perms::owner_read);
    std::filesystem::permissions(directory,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 2, {3, 4}).toWire());
    BOOST_CHECK(!response.getStatus());
    BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-artifact-begin-failed"), 0U);
    BOOST_CHECK(OwnedPayloadFixture::read(stage) == original);
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), reserved);
    response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    BOOST_CHECK_EQUAL(responseString(response), "pending");
  }
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), 0U);
  BOOST_CHECK(!std::filesystem::exists(stage));
}

BOOST_AUTO_TEST_CASE(BoundRangeInternalCleanupCanBeConfirmedWithoutSurvivingReservations)
{
  BOOST_REQUIRE_NE(::geteuid(), 0U);
  OwnedPayloadFixture f;
  const auto write = f.publication(makeReference("owned-range-cleaned-failure", {1, 2, 3, 4}),
    "/spec192/user", "owned-range-cleaned-failure");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto reserved = f.sqlCharge("/spec192/user", true);
  const auto stage = f.temporary.path() / "payloads" / "staging" / (write.reference.contentDigest + ".1.part");
  {
    OwnedRollbackPermissions restore(stage);
    std::filesystem::permissions(stage, std::filesystem::perms::owner_read);
    response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 2, {3, 4}).toWire());
    BOOST_CHECK(!response.getStatus());
    BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-artifact-begin-failed"), 0U);
  }
  BOOST_CHECK(!std::filesystem::exists(stage));
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), reserved);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), 0U);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
}

BOOST_AUTO_TEST_CASE(BoundRangesCommitAndAbortConserveCharge)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  auto write = f.publication(makeReference("ranges", bytes), "/user/u", "ranges");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto reserved = f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes;
  BOOST_CHECK_GT(reserved, bytes.size());
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes, reserved);
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 2, {3, 4}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  response = f.call("COMMIT_RANGES", OwnedPayloadFixture::commit(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charged = bytes.size() + OwnedPayloadFixture::read(f.onlyManifest()).size();
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).committedBytes, charged);
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", false), charged);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes, 0U);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(f.node->core().getRange(artifactStorageObjectName(write.reference), {0, 4}) == bytes);
  auto abandoned = f.publication(makeReference("abort", bytes), "/user/u", "abort");
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(abandoned, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(abandoned).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(abandoned).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes, 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).committedBytes, charged);
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(abandoned, 0, {1, 2}).toWire());
  BOOST_CHECK(!response.getStatus());
}

BOOST_AUTO_TEST_CASE(BoundRangeAbortRetainsSharedStageChargesUntilPeerCommit)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  const auto a = f.publication(makeReference("shared-abort-a", bytes), "/user/u", "shared-abort-a");
  const auto b = f.publication(makeReference("shared-abort-b", bytes), "/user/v", "shared-abort-b");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(a, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(b, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto chargeA = f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes;
  const auto chargeB = f.journal->userStorageUsage("/user/v", f.capability.repoNode).reservedBytes;
  BOOST_CHECK_GT(chargeA, bytes.size()); BOOST_CHECK_GT(chargeB, bytes.size());
  const auto stage = f.temporary.path() / "payloads" / "staging" / (a.reference.contentDigest + ".1.part");
  const auto staged = OwnedPayloadFixture::read(stage);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(a).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "pending");
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), chargeA);
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/v", true), chargeB);
  BOOST_CHECK(OwnedPayloadFixture::read(stage) == staged);
  const auto pending = f.journal->lookupUserStorageWrite(f.capability.repoNode, "/user/u", a.operationId);
  BOOST_REQUIRE(pending); BOOST_CHECK(pending->state == RepoUserStorageWriteState::Reserved);
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(b, 2, {3, 4}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  response = f.call("COMMIT_RANGES", OwnedPayloadFixture::commit(b).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto peerCharge = f.journal->userStorageUsage("/user/v", f.capability.repoNode).committedBytes;
  BOOST_CHECK_GT(peerCharge, bytes.size());
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/v", true), 0U);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(a).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/v", false), peerCharge);
  BOOST_CHECK(f.node->core().getRange(artifactStorageObjectName(b.reference), {0, 4}) == bytes);
  const auto cancelled = f.journal->lookupUserStorageWrite(f.capability.repoNode, "/user/u", a.operationId);
  BOOST_REQUIRE(cancelled); BOOST_CHECK(cancelled->state == RepoUserStorageWriteState::Cancelled);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(a).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(a, 0, {1, 2}).toWire());
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/v", false), peerCharge);
  BOOST_CHECK(f.node->core().getRange(artifactStorageObjectName(b.reference), {0, 4}) == bytes);
}

BOOST_AUTO_TEST_CASE(BoundRangeAbortRetainsQuotaForUnknownAndLinkedStaging)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  const auto write = f.publication(makeReference("guarded-bound-abort", bytes), "/user/u", "guarded-bound-abort");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charge = f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes;
  BOOST_CHECK_GT(charge, bytes.size());
  const auto directory = f.temporary.path() / "payloads" / "staging";
  const auto stage = directory / (write.reference.contentDigest + ".1.part");
  const auto staged = OwnedPayloadFixture::read(stage);
  const auto unknown = directory / (write.reference.contentDigest + ".99.part");
  { std::ofstream output(unknown); output << "unresolved"; BOOST_REQUIRE(output.good()); }
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "pending");
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), charge);
  BOOST_CHECK(OwnedPayloadFixture::read(stage) == staged);
  BOOST_CHECK_EQUAL(OwnedPayloadFixture::read(unknown), "unresolved");
  std::filesystem::remove(unknown); // Fixture explicitly settles the unknown holder.
  const std::filesystem::path ranges = stage.string() + ".ranges";
  const auto rangeBytes = OwnedPayloadFixture::read(ranges);
  const auto external = f.temporary.path() / "external-ranges";
  std::filesystem::rename(ranges, external); std::filesystem::create_symlink(external, ranges);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "pending");
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), charge);
  BOOST_CHECK(OwnedPayloadFixture::read(stage) == staged);
  BOOST_CHECK(OwnedPayloadFixture::read(external) == rangeBytes);
  BOOST_CHECK(std::filesystem::is_symlink(ranges));
  std::filesystem::remove(ranges); std::filesystem::rename(external, ranges);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), 0U);
  BOOST_CHECK(!std::filesystem::exists(stage)); BOOST_CHECK(!std::filesystem::exists(ranges));
  BOOST_CHECK_EQUAL(countPersistentReceipts(f.metadataPath, write.reference), 0U);
}

BOOST_AUTO_TEST_CASE(BoundRangeAbortKeepsReservationAcrossRealUnlinkFailure)
{
  BOOST_REQUIRE_MESSAGE(::geteuid() != 0, "permission failure oracle requires unprivileged native execution");
  OwnedPayloadFixture f;
  const auto write = f.publication(makeReference("unlink-bound-abort", {1, 2, 3, 4}), "/user/u", "unlink-bound-abort");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charge = f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes;
  const auto directory = f.temporary.path() / "payloads" / "staging";
  const auto stage = directory / (write.reference.contentDigest + ".1.part");
  const auto staged = OwnedPayloadFixture::read(stage);
  const auto ranges = OwnedPayloadFixture::read(stage.string() + ".ranges");
  {
    struct RestorePermissions {
      std::filesystem::path path;
      std::filesystem::perms original;
      ~RestorePermissions() {
        std::error_code error; std::filesystem::permissions(path, original, error);
      }
    } restore{directory, std::filesystem::status(directory).permissions()};
    std::filesystem::permissions(directory, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  }
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "pending");
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), charge);
  BOOST_CHECK(OwnedPayloadFixture::read(stage) == staged);
  BOOST_CHECK(OwnedPayloadFixture::read(stage.string() + ".ranges") == ranges);
  const auto retained = f.journal->lookupUserStorageWrite(f.capability.repoNode, "/user/u", write.operationId);
  BOOST_REQUIRE(retained); BOOST_CHECK(retained->state == RepoUserStorageWriteState::Reserved);
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), 0U);
  BOOST_CHECK(!std::filesystem::exists(stage));
  BOOST_CHECK(!std::filesystem::exists(stage.string() + ".ranges"));
}

// Fund a trusted Core staging fixture through the actual registered owner grant;
// this does not create a seed publication or emulate a network replica write.
static RepoUserStorageWrite
fundedCleanupRange(OwnedPayloadFixture& f, const ArtifactReference& reference,
                   const std::string& operationId, const std::string& physicalOperationId = {})
{
  const auto objectName = artifactStorageObjectName(reference);
  const auto request = RepoUserStorageGrantRequest{1, "grant-" + operationId,
    "/spec192/user", objectName, f.now() + 60000};
  const auto response = f.call("USER_STORAGE_GRANT", toBytes(request.toJson()));
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto grant = RepoUserStorageGrantReply::fromJson(responseString(response)).grant;
  BOOST_REQUIRE(f.journal->reconcileUserStorageInventory(f.capability.repoNode, {}, f.now()).ready);
  RepoObjectManifest manifest;
  manifest.objectName = objectName; manifest.objectType = "artifact-v2-bytes";
  manifest.sha256 = reference.contentDigest; manifest.size = reference.sizeBytes;
  manifest.policyEpoch = reference.policyEpoch; manifest.generation = 1;
  manifest.operationId = physicalOperationId.empty() ? operationId : physicalOperationId;
  const RepoUserStorageObject object{objectName, manifest.sha256, 1, manifest.size, manifest.toJson().size()};
  auto source = f.journal->reserveUserStorage("/spec192/user", f.capability.repoNode,
    grant.ownerIdentity, grant.grantId, operationId, object, f.now());
  f.node->core().putRange(manifest, {0, 2}, {1, 2});
  return source;
}

BOOST_AUTO_TEST_CASE(OwnerDeleteBindsCancelledWriteToFundedCoreStageBeforeRelease)
{
  OwnedPayloadFixture f;
  const auto reference = makeReference("cleanup-source", {1, 2, 3, 4});
  const auto source = fundedCleanupRange(f, reference, "cleanup-source");
  const auto charge = source.object.payloadWireBytes + source.object.manifestBytes;
  BOOST_CHECK_EQUAL(source.grant.ownerIdentity, "/spec192/user");
  BOOST_CHECK(!f.node->core().abortUserStorageWriteRangesIfOwned(source));
  const auto stage = f.temporary.path() / "payloads" / "staging" / (reference.contentDigest + ".1.part");
  BOOST_REQUIRE(std::filesystem::exists(stage));
  const auto request = toBytes(RepoUserStorageDeleteRequest{1, "cleanup", source.object.objectName, source.grant.ownerEpoch}.toJson());
  auto response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  auto reply = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(reply.state, "DELETED"); BOOST_CHECK_EQUAL(reply.releasedBytes, charge);
  BOOST_CHECK_EQUAL(reply.ownerIdentity, "/spec192/user"); BOOST_CHECK_EQUAL(reply.repoNode, f.capability.repoNode);
  BOOST_CHECK(!std::filesystem::exists(stage));
  BOOST_CHECK(!std::filesystem::exists(stage.string() + ".ranges"));
  BOOST_CHECK_EQUAL(f.sqlCharge(source.grant.ownerIdentity, true), 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge(source.grant.ownerIdentity, false), 0U);
  BOOST_CHECK(f.journal->listUserStorage(source.grant.ownerIdentity, f.capability.repoNode, {}).empty());
  const auto cancelled = f.journal->lookupUserStorageWrite(f.capability.repoNode, source.grant.ownerIdentity, source.operationId);
  BOOST_REQUIRE(cancelled); BOOST_CHECK(cancelled->state == RepoUserStorageWriteState::Cancelled);
  BOOST_CHECK(!f.node->core().abortUserStorageWriteRangesIfOwned(*cancelled));
  BOOST_CHECK_THROW(f.journal->validateUserStorageGrant("/spec192/user", f.capability.repoNode,
    source.grant.ownerIdentity, source.grant.grantId, source.object.objectName, source.grant.ownerEpoch, f.now()), std::runtime_error);
  response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  reply = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(reply.state, "DELETED"); BOOST_CHECK_EQUAL(reply.releasedBytes, 0U);
  const auto fresh = RepoUserStorageGrantRequest{1, "fresh", "/spec192/user", source.object.objectName, f.now() + 60000};
  response = f.call("USER_STORAGE_GRANT", toBytes(fresh.toJson()));
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(RepoUserStorageGrantReply::fromJson(responseString(response)).grant.ownerEpoch, 2U);
}

BOOST_AUTO_TEST_CASE(OwnerDeleteRefusesAStageFromAnotherOperationEvenWithMatchingTuple)
{
  OwnedPayloadFixture f;
  const auto reference = makeReference("cleanup-source-mismatch", {1, 2, 3, 4});
  const auto source = fundedCleanupRange(f, reference, "ledger-op", "actual-op");
  const auto charge = source.object.payloadWireBytes + source.object.manifestBytes;
  const auto stage = f.temporary.path() / "payloads" / "staging" / (reference.contentDigest + ".1.part");
  const auto staged = OwnedPayloadFixture::read(stage);
  const auto request = toBytes(RepoUserStorageDeleteRequest{1, "cleanup", source.object.objectName, source.grant.ownerEpoch}.toJson());
  for (int retry = 0; retry < 2; ++retry) {
    const auto response = f.call("USER_STORAGE_DELETE", request);
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    const auto reply = RepoUserStorageDeleteReply::fromJson(responseString(response));
    BOOST_CHECK_EQUAL(reply.state, "PENDING"); BOOST_CHECK_EQUAL(reply.releasedBytes, 0U);
    BOOST_CHECK_EQUAL(f.sqlCharge(source.grant.ownerIdentity, false), charge);
    BOOST_CHECK_EQUAL(f.sqlCharge(source.grant.ownerIdentity, true), 0U);
    BOOST_CHECK(OwnedPayloadFixture::read(stage) == staged);
    BOOST_CHECK(std::filesystem::exists(stage.string() + ".ranges"));
  }
  const auto peers = f.journal->userStorageCleanupPeers(f.capability.repoNode, source.grant.ownerIdentity, "cleanup");
  BOOST_REQUIRE_EQUAL(peers.cancelledWrites.size(), 1U);
  BOOST_CHECK_EQUAL(peers.cancelledWrites.front().operationId, "ledger-op");
  BOOST_CHECK(peers.cleanup.state == RepoUserStorageClaimState::CleanupPrepared);
}

BOOST_AUTO_TEST_CASE(OwnerDeleteRetainsPreparedChargeAcrossOwnedStageUnlinkFailure)
{
  BOOST_REQUIRE_MESSAGE(::geteuid() != 0, "permission failure oracle requires unprivileged native execution");
  OwnedPayloadFixture f;
  const auto reference = makeReference("cleanup-source-unlink", {1, 2, 3, 4});
  const auto source = fundedCleanupRange(f, reference, "cleanup-unlink");
  const auto charge = source.object.payloadWireBytes + source.object.manifestBytes;
  const auto directory = f.temporary.path() / "payloads" / "staging";
  const auto stage = directory / (reference.contentDigest + ".1.part");
  const auto staged = OwnedPayloadFixture::read(stage);
  const auto ranges = OwnedPayloadFixture::read(stage.string() + ".ranges");
  const auto request = toBytes(RepoUserStorageDeleteRequest{1, "cleanup", source.object.objectName, source.grant.ownerEpoch}.toJson());
  ndn_service_framework::ResponseMessage response;
  {
    struct RestorePermissions {
      std::filesystem::path path;
      std::filesystem::perms original;
      ~RestorePermissions() {
        std::error_code error; std::filesystem::permissions(path, original, error);
      }
    } restore{directory, std::filesystem::status(directory).permissions()};
    std::filesystem::permissions(directory, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
    response = f.call("USER_STORAGE_DELETE", request);
  }
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-artifact-remove-failed"), 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge(source.grant.ownerIdentity, false), charge);
  BOOST_CHECK_EQUAL(f.sqlCharge(source.grant.ownerIdentity, true), 0U);
  BOOST_CHECK(OwnedPayloadFixture::read(stage) == staged);
  BOOST_CHECK(OwnedPayloadFixture::read(stage.string() + ".ranges") == ranges);
  const auto peers = f.journal->userStorageCleanupPeers(f.capability.repoNode, source.grant.ownerIdentity, "cleanup");
  BOOST_REQUIRE_EQUAL(peers.cancelledWrites.size(), 1U);
  BOOST_CHECK(peers.cleanup.state == RepoUserStorageClaimState::CleanupPrepared);
  response = f.call("USER_STORAGE_DELETE", request);
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto reply = RepoUserStorageDeleteReply::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(reply.state, "DELETED"); BOOST_CHECK_EQUAL(reply.releasedBytes, charge);
  BOOST_CHECK_EQUAL(f.sqlCharge(source.grant.ownerIdentity, false), 0U);
  BOOST_CHECK(!std::filesystem::exists(stage)); BOOST_CHECK(!std::filesystem::exists(stage.string() + ".ranges"));
}

BOOST_AUTO_TEST_CASE(StartupCompletesPreparedOwnedPublicationCleanupBeforeAdmission)
{
  for (const std::string policy : {"ONE", "ALL"}) {
    OwnedPayloadFixture f;
    const std::vector<uint8_t> bytes{1, 2, 3, 4};
    auto write = f.publication(makeReference("restart-owned-publish-" + policy, bytes),
      "/spec192/user", "restart-owned-publish-" + policy, policy, 2);
    write.bytes = bytes;
    auto response = f.call("STORE", write.toWire());
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    const auto seedReceipt = RepoArtifactCommitReceipt::fromJson(responseString(response));
    const auto accepted = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
    BOOST_REQUIRE(accepted); BOOST_REQUIRE_EQUAL(accepted->state, "ACCEPTED");
    auto attempted = accepted->status.targets.at(1);
    attempted.state = "IN_FLIGHT"; attempted.attemptCount = 1;
    attempted.updatedAtMs = OwnedPayloadFixture::now();
    f.journal->updatePublishOperationTarget(write.operationId, attempted, attempted.updatedAtMs);
    const auto objectName = artifactStorageObjectName(write.reference);
    const auto charge = f.sqlCharge("/spec192/user", false);
    const auto prepared = f.journal->prepareUserStorageCleanup("/spec192/user",
      f.capability.repoNode, "cleanup", objectName, 1, OwnedPayloadFixture::now());
    BOOST_CHECK(prepared.state == RepoUserStorageClaimState::CleanupPrepared);
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charge);
    f.reopen(); // Actual constructor continues cleanup before service registration.
    BOOST_CHECK(f.persistedReady());
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), 0U);
    BOOST_CHECK(!f.node->core().has(objectName));
    BOOST_CHECK(f.journal->pendingUserStorageCleanups(f.capability.repoNode).empty());
    BOOST_CHECK(f.journal->listUserStorage("/spec192/user", f.capability.repoNode, "", 10).empty());
    const auto terminal = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
    BOOST_REQUIRE(terminal);
    BOOST_CHECK_EQUAL(terminal->state, policy == "ONE" ? "COMMITTED" : "PARTIAL");
    const auto receipt = ArtifactCommitReceipt::fromJson(terminal->aggregateReceiptJson);
    BOOST_CHECK_EQUAL(receipt.achievedReplicas, 1U);
    BOOST_REQUIRE(terminal->status.targets.front().receipt);
    BOOST_CHECK_EQUAL(terminal->status.targets.front().receipt->toJson(), seedReceipt.toJson());
    BOOST_CHECK_EQUAL(terminal->status.targets[1].state, "UNKNOWN");
    BOOST_CHECK_EQUAL(terminal->status.targets[2].state, "NOT_ATTEMPTED");
    f.reopen();
    BOOST_CHECK(f.persistedReady());
    BOOST_CHECK_EQUAL(f.journal->lookupPublishOperation(write.operationId,
      "/spec192/user")->aggregateReceiptJson, terminal->aggregateReceiptJson);
    response = f.call("USER_STORAGE_DELETE", toBytes(RepoUserStorageDeleteRequest{1,
      "cleanup", objectName, 1}.toJson()));
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(responseString(response)).releasedBytes, 0U);
    response = f.call("STORE", write.toWire());
    BOOST_CHECK(!response.getStatus()); BOOST_CHECK(!f.node->core().has(objectName));
    BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
    auto probe = f.publication(makeReference("restart-admission-" + policy, {5, 6}),
      "/user/probe", "restart-admission-" + policy);
    probe.bytes = {5, 6};
    response = f.call("STORE", probe.toWire());
    BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
    BOOST_CHECK_GT(f.sqlCharge("/user/probe", false), 0U);
  }
}

BOOST_AUTO_TEST_CASE(StartupRetainsPreparedChargeForAnotherOwnersAcceptedPublication)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  const auto reference = makeReference("restart-peer-publish", bytes);
  auto peer = f.publication(reference, "/user/v", "restart-peer-publish", "ALL", 2);
  peer.bytes = bytes;
  auto response = f.call("STORE", peer.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto accepted = f.journal->lookupPublishOperation(peer.operationId, "/spec192/user");
  BOOST_REQUIRE(accepted); BOOST_REQUIRE_EQUAL(accepted->state, "ACCEPTED");
  const auto peerCharge = f.sqlCharge("/user/v", false);
  const auto own = f.publication(reference, "/spec192/user", "restart-own-pending");
  response = f.call("STORE_RANGE", OwnedPayloadFixture::range(own, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charge = f.sqlCharge("/spec192/user", true);
  BOOST_REQUIRE_GT(charge, 0U);
  const auto objectName = artifactStorageObjectName(reference);
  (void)f.journal->prepareUserStorageCleanup("/spec192/user", f.capability.repoNode,
    "cleanup", objectName, 1, OwnedPayloadFixture::now());
  f.reopen();
  BOOST_CHECK(!f.persistedReady());
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charge);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", true), 0U);
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/v", false), peerCharge);
  BOOST_CHECK(f.node->core().getRange(objectName, {0, 4}) == bytes);
  const auto pending = f.journal->pendingUserStorageCleanups(f.capability.repoNode);
  BOOST_REQUIRE_EQUAL(pending.size(), 1U);
  BOOST_CHECK_EQUAL(pending.front().ownerIdentity, "/spec192/user");
  BOOST_CHECK_EQUAL(f.journal->lookupPublishOperation(peer.operationId,
    "/spec192/user")->status.toJson(), accepted->status.toJson());
  response = f.call("USER_STORAGE_DELETE", toBytes(RepoUserStorageDeleteRequest{1,
    "cleanup", objectName, 1}.toJson()));
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteReply::fromJson(responseString(response)).state, "PENDING");
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charge);
  auto probe = f.publication(makeReference("restart-blocked-admission", {5, 6}),
    "/user/probe", "restart-blocked-admission");
  probe.bytes = {5, 6};
  response = f.call("STORE", probe.toWire());
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-user-storage-reconciliation-required"), 0U);
  BOOST_CHECK(!f.node->core().has(artifactStorageObjectName(probe.reference)));
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/probe", false), 0U);
}

BOOST_AUTO_TEST_CASE(StartupRefusesCorruptPublicationIdentityAndResumesAfterExactRestoration)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  auto write = f.publication(makeReference("restart-corrupt-publish", bytes),
    "/spec192/user", "restart-corrupt-publish");
  write.bytes = bytes;
  auto response = f.call("STORE", write.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto accepted = f.journal->lookupPublishOperation(write.operationId, "/spec192/user");
  BOOST_REQUIRE(accepted); BOOST_REQUIRE_EQUAL(accepted->state, "ACCEPTED");
  const auto objectName = artifactStorageObjectName(write.reference);
  const auto charge = f.sqlCharge("/spec192/user", false);
  (void)f.journal->prepareUserStorageCleanup("/spec192/user", f.capability.repoNode,
    "cleanup", objectName, 1, OwnedPayloadFixture::now());
  auto corrupt = accepted->status; corrupt.consistency = "ALL";
  f.replacePublishSnapshot(write.operationId, corrupt);
  BOOST_CHECK_THROW(f.reopen(), std::runtime_error);
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), charge);
  {
    RepoCore physical(f.capability, f.backend);
    BOOST_CHECK(physical.getRange(objectName, {0, 4}) == bytes);
    SqliteArtifactMetadataStore journal(f.metadataPath, true, 16, false);
    const auto pending = journal.pendingUserStorageCleanups(f.capability.repoNode);
    BOOST_REQUIRE_EQUAL(pending.size(), 1U);
    BOOST_CHECK(pending.front().state == RepoUserStorageClaimState::CleanupPrepared);
  }
  f.replacePublishSnapshot(write.operationId, accepted->status);
  f.reopen();
  BOOST_CHECK(f.persistedReady());
  BOOST_CHECK_EQUAL(f.sqlCharge("/spec192/user", false), 0U);
  BOOST_CHECK(!f.node->core().has(objectName));
  BOOST_CHECK_EQUAL(f.journal->lookupPublishOperation(write.operationId,
    "/spec192/user")->state, "COMMITTED");
  auto probe = f.publication(makeReference("restored-restart-admission", {5, 6}),
    "/user/probe", "restored-restart-admission");
  probe.bytes = {5, 6};
  response = f.call("STORE", probe.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_GT(f.sqlCharge("/user/probe", false), 0U);
}

BOOST_AUTO_TEST_CASE(StartupSettlesCompletePendingPublicationBeforeReady)
{
  OwnedPayloadFixture f;
  const std::vector<uint8_t> bytes{1, 2, 3, 4};
  auto write = f.publication(makeReference("recovery", bytes), "/user/u", "recovery");
  BOOST_REQUIRE(f.journal->reconcileUserStorageInventory(f.capability.repoNode, {}, f.now()).ready);
  const auto objectName = artifactStorageObjectName(write.reference);
  (void)f.journal->reserveUserStorage("/spec192/user", f.capability.repoNode, "/user/u",
    write.storageBinding->grantId, write.operationId, {objectName, write.reference.contentDigest, 1, 4, 1}, f.now());
  RepoObjectManifest manifest;
  manifest.objectName = objectName; manifest.objectType = "artifact-v2-bytes";
  manifest.sha256 = write.reference.contentDigest; manifest.size = 4;
  manifest.generation = 1; manifest.operationId = write.operationId;
  manifest.policyEpoch = write.reference.policyEpoch; manifest.segmentCount = 1;
  manifest.replicationFactor = 1; manifest.writeConsistency = "ONE"; manifest.requiredWriteAcks = 1;
  (void)f.node->core().handleStore(encodeStoreRequest(manifest, bytes));
  const auto charged = bytes.size() + OwnedPayloadFixture::read(f.onlyManifest()).size();
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes, 5U);
  BOOST_CHECK_EQUAL(countPersistentReceipts(f.metadataPath, write.reference), 0U);
  f.reopen();
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes, 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).committedBytes, charged);
  BOOST_CHECK_EQUAL(countPersistentReceipts(f.metadataPath, write.reference), 0U);
  write.bytes = bytes;
  const auto response = f.call("STORE", write.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(countPersistentReceipts(f.metadataPath, write.reference), 1U);
  f.reopen();
  BOOST_CHECK_EQUAL(f.sqlCharge("/user/u", true), 0U);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).committedBytes, charged);
}

BOOST_AUTO_TEST_CASE(UnknownRestartStagingRemainsChargedAndClosesAdmission)
{
  OwnedPayloadFixture f;
  auto write = f.publication(makeReference("unknown-staging", {1, 2, 3, 4}), "/user/u", "pending");
  auto response = f.call("STORE_RANGE", OwnedPayloadFixture::range(write, 0, {1, 2}).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto charge = f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes;
  f.reopen();
  BOOST_CHECK(!f.journal->userStorageReconciliation(f.capability.repoNode).ready);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes, charge);
  auto other = f.publication(makeReference("blocked", {1, 2}), "/user/v", "blocked"); other.bytes = {1, 2};
  response = f.call("STORE", other.toWire());
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-user-storage-reconciliation-required"), 0U);
  BOOST_CHECK(!f.node->core().has(artifactStorageObjectName(other.reference)));
  response = f.call("ABORT_RANGES", OwnedPayloadFixture::abort(write).toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "pending");
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/u", f.capability.repoNode).reservedBytes, charge);
  BOOST_CHECK_EQUAL(f.journal->userStorageUsage("/user/v", f.capability.repoNode).committedBytes, 0U);
}

BOOST_AUTO_TEST_CASE(ActualManifestWireMeasurementDetectsUnaccountedBytesOnRestart)
{
  OwnedPayloadFixture f;
  auto write = f.publication(makeReference("manifest-measurement", {1, 2}), "/user/u", "measurement"); write.bytes = {1, 2};
  const auto response = f.call("STORE", write.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto path = f.onlyManifest();
  const auto original = OwnedPayloadFixture::read(path);
  const auto probe = f.node->core().probeObjectMetadata(artifactStorageObjectName(write.reference));
  BOOST_CHECK(probe.manifestMeasurementAvailable);
  BOOST_CHECK_EQUAL(probe.manifestWireBytes, original.size());
  { std::ofstream output(path, std::ios::binary | std::ios::app); output << "\n  "; BOOST_REQUIRE(output.good()); }
  BOOST_CHECK_EQUAL(OwnedPayloadFixture::read(path).size(), original.size() + 3);
  BOOST_CHECK_EQUAL(f.backend->probeObjectMetadata(artifactStorageObjectName(write.reference)).manifestWireBytes, original.size() + 3);
  BOOST_CHECK_THROW(f.reopen(), std::runtime_error);
  SqliteArtifactMetadataStore retained(f.metadataPath, false, 16, false);
  BOOST_CHECK_EQUAL(retained.userStorageUsage("/user/u", f.capability.repoNode).committedBytes, original.size() + 2);
}

BOOST_AUTO_TEST_CASE(IncompletePhysicalMetadataCannotUpgradeThroughBoundWrite)
{
  for (const bool missingGeneration : {true, false}) {
    OwnedPayloadFixture f;
    auto write = f.publication(makeReference("legacy", {1, 2}), "/user/u", "legacy");
    RepoObjectManifest manifest;
    manifest.objectName = artifactStorageObjectName(write.reference);
    manifest.objectType = "artifact-v2-bytes"; manifest.sha256 = write.reference.contentDigest;
    manifest.size = 2; manifest.generation = 1; manifest.operationId = write.operationId;
    manifest.policyEpoch = write.reference.policyEpoch;
    manifest.writeConsistency = "ONE"; manifest.requiredWriteAcks = 1;
    (void)f.node->core().handleStore(encodeStoreRequest(manifest, {1, 2}));
    const auto path = f.onlyManifest();
    auto legacyWire = OwnedPayloadFixture::read(path);
    const auto field = missingGeneration ? std::string("\"generation\":1") :
      std::string("\"operationId\":\"") + write.operationId + "\"";
    const auto position = legacyWire.find(field);
    BOOST_REQUIRE(position != std::string::npos);
    legacyWire.replace(position, field.size(), missingGeneration ?
      "\"generation\":0" : "\"operationId\":\"\"");
    { std::ofstream output(path, std::ios::binary | std::ios::trunc); output << legacyWire; BOOST_REQUIRE(output.good()); }
    write.bytes = {1, 2};
    const auto response = f.call("STORE", write.toWire());
    BOOST_CHECK(!response.getStatus());
    BOOST_CHECK_EQUAL(response.getErrorInfo().find(missingGeneration ?
      "repo-artifact-invalid-generation" : "repo-user-storage-reconciliation-required"), 0U);
    BOOST_CHECK_EQUAL(OwnedPayloadFixture::read(path), legacyWire);
    BOOST_CHECK(!f.journal->lookupUserStorageWrite(f.capability.repoNode, "/user/u", write.operationId));
    BOOST_CHECK_EQUAL(countPersistentReceipts(f.metadataPath, write.reference), 0U);
  }
}

BOOST_AUTO_TEST_CASE(OwnedGenerationCannotBeRecoveredFromLegacyInventory)
{
  OwnedPayloadFixture f;
  auto write = f.publication(makeReference("generation", {1, 2}), "/user/u", "generation");
  write.bytes = {1, 2};
  const auto response = f.call("STORE", write.toWire());
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto path = f.onlyManifest();
  const auto original = OwnedPayloadFixture::read(path);
  auto legacyWire = original;
  const auto position = legacyWire.find("\"generation\":1");
  BOOST_REQUIRE(position != std::string::npos);
  legacyWire.replace(position, std::string("\"generation\":1").size(), "\"generation\":0");
  BOOST_REQUIRE_EQUAL(legacyWire.size(), original.size());
  { std::ofstream output(path, std::ios::binary | std::ios::trunc); output << legacyWire; BOOST_REQUIRE(output.good()); }
  BOOST_CHECK_THROW(f.reopen(), std::exception);
  SqliteArtifactMetadataStore retained(f.metadataPath, false, 16, false);
  BOOST_CHECK_EQUAL(retained.userStorageUsage("/user/u", f.capability.repoNode).committedBytes, original.size() + 2);
}

BOOST_AUTO_TEST_CASE(OwnedPublishIntentChecksLiveSeedGrantBeforeJournalAdmission)
{
  TempRoot temporary;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-owned-intent", "tpm-memory:spec204-owned-intent");
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/provider"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/authority"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
    ndn::Name("/spec192/group"), providerCert, authorityCert,
    std::filesystem::absolute("examples/trust-any.conf").string());
  StorageCapability capability;
  capability.repoNode = "/repo/owned-intent"; capability.repoMode = "persistent";
  capability.freeBytes = 1024 * 1024;
  const auto metadataPath = (temporary.path() / "metadata.sqlite3").string();
  auto node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>((temporary.path() / "payloads").string(), 4096, 1024, "owned-intent"),
    capability.repoNode, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), metadataPath);
  node->registerServices(provider);
  SqliteArtifactMetadataStore journal(metadataPath, true, 16, true);
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  PublishOperationIntent intent;
  intent.schemaVersion = 2; intent.reference = makeReference("owned-intent", {1, 2});
  intent.generation = 1; intent.consistency = "ONE";
  intent.seed = {capability.repoNode, "/spec192/provider", "/service/repo"};
  intent.targets = {intent.seed};
  intent.userStorageBindings = {{1, intent.seed, {1, "/user/u", "grant", 1}, "/spec192/user"}};
  intent.idempotencyKeyDigest = digestPublishIdempotencyKey("owned-intent-key");
  const auto digestFor = [](const auto& value) {
    return makePublishRequestDigest(value.reference, value.generation, value.seed,
      value.consistency, value.targets, value.userStorageBindings);
  };
  const auto operationId = makePublishOperationId("/spec192/user", intent.seed, intent.idempotencyKeyDigest);
  const auto submit = [&](PublishOperationIntent value, const std::string& requestId) {
    value.requestDigest = digestFor(value);
    const auto wire = value.toJson();
    return dispatch(provider, makeRepoServiceName(node->servicePrefix(), "STORE"),
      std::vector<uint8_t>(wire.begin(), wire.end()), requestId);
  };
  auto response = submit(intent, "absent-grant");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-user-storage-unauthorized"), 0U);
  BOOST_CHECK(!journal.lookupPublishOperation(operationId, "/spec192/user"));
  const auto objectName = artifactStorageObjectName(intent.reference);
  (void)journal.grantUserStorage("/user/u", capability.repoNode, "grant", "/spec192/user", objectName, now + 60000, now);
  auto wrong = intent; wrong.userStorageBindings[0].binding.ownerEpoch = 2;
  response = submit(wrong, "wrong-epoch");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!journal.lookupPublishOperation(operationId, "/spec192/user"));
  wrong = intent; wrong.userStorageBindings[0].binding.ownerIdentity = "/user/v";
  response = submit(wrong, "wrong-owner");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!journal.lookupPublishOperation(operationId, "/spec192/user"));
  (void)journal.grantUserStorage("/user/u", capability.repoNode, "expired", "/spec192/user", objectName, now - 1000, now - 2000);
  wrong = intent; wrong.userStorageBindings[0].binding.grantId = "expired";
  response = submit(wrong, "expired-grant");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-user-storage-grant-expired"), 0U);
  BOOST_CHECK(!journal.lookupPublishOperation(operationId, "/spec192/user"));
  response = submit(intent, "valid-grant");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), operationId);
  const auto accepted = journal.lookupPublishOperation(operationId, "/spec192/user");
  BOOST_REQUIRE(accepted);
  BOOST_CHECK_EQUAL(accepted->intent.schemaVersion, 2U);
  BOOST_CHECK_EQUAL(accepted->intent.userStorageBindings[0].binding.ownerIdentity, "/user/u");
  BOOST_CHECK_EQUAL(accepted->state, "PREPARING");
  response = submit(intent, "exact-retry");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), operationId);
  (void)journal.grantUserStorage("/user/u", capability.repoNode, "other-grant", "/spec192/user", objectName, now + 60000, now);
  wrong = intent; wrong.userStorageBindings[0].binding.grantId = "other-grant";
  response = submit(wrong, "changed-grant-same-key");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-publish-idempotency-conflict"), 0U);
  ArtifactStoreRequest unbound;
  unbound.operationId = operationId; unbound.generation = 1;
  unbound.reference = intent.reference; unbound.bytes = {1, 2};
  response = dispatch(provider, makeRepoServiceName(node->servicePrefix(), "STORE"),
    unbound.toWire(), "unbound-owned-operation");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-user-storage-binding-required"), 0U);
  BOOST_CHECK(node->core().list().empty());
  BOOST_CHECK(!node->core().has(objectName));
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).reservedBytes, 0U);
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).committedBytes, 0U);
  BOOST_CHECK(journal.pendingUserStorageWrites(capability.repoNode).empty());
  BOOST_CHECK_EQUAL(countPersistentReceipts(metadataPath, intent.reference), 0U);
  // Core LocalMock actor propagation is covered; real network auth remains open.
}

BOOST_AUTO_TEST_CASE(BoundStorageFramesWithoutOwnedIntentCannotMutateStorage)
{
  TempRoot temporary;
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:spec204-bound-frame", "tpm-memory:spec204-bound-frame");
  const auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/provider"));
  const auto authorityCert = makeRsaIdentity(keyChain, ndn::Name("/spec192/authority"));
  ndn_service_framework::ServiceProvider provider(
    ndn_service_framework::ServiceProvider::LocalMockTag{}, face,
    ndn::Name("/spec192/group"), providerCert, authorityCert,
    std::filesystem::absolute("examples/trust-any.conf").string());
  StorageCapability capability;
  capability.repoNode = "/repo/bound-frame"; capability.repoMode = "persistent";
  capability.freeBytes = 1024 * 1024;
  const auto metadataPath = (temporary.path() / "metadata.sqlite3").string();
  auto node = std::make_shared<RepoNode>(ndn::Name("/service/repo"), capability,
    std::make_shared<FilesystemRepoStoreBackend>((temporary.path() / "payloads").string(), 4096, 1024, "bound-frame"),
    capability.repoNode, provider.getName().toUri(ndn::name::UriFormat::ALTERNATE), metadataPath);
  node->registerServices(provider);
  const auto ref = makeReference("bound-frame", {1, 2});
  const RepoUserStorageBinding binding{1, "/user/u", "grant", 1};
  ArtifactStoreRequest store;
  store.schemaVersion = 3; store.operationId = "bound-operation"; store.generation = 1;
  store.reference = ref; store.bytes = {1, 2}; store.storageBinding = binding;
  ArtifactRangeWriteRequest range;
  range.schemaVersion = 3; range.operationId = store.operationId; range.generation = 1;
  range.reference = ref; range.storageBinding = binding;
  range.offsetBytes = 0; range.lengthBytes = 2; range.bytes = {1, 2};
  ArtifactRangeCommitRequest commit;
  commit.schemaVersion = 3; commit.operationId = store.operationId; commit.generation = 1;
  commit.reference = ref; commit.storageBinding = binding;
  ArtifactRangeAbortRequest abort;
  abort.schemaVersion = 2; abort.operationId = store.operationId; abort.generation = 1;
  abort.reference = ref; abort.storageBinding = binding;
  for (const auto& entry : {
      std::pair<std::string, std::vector<uint8_t>>{"STORE", store.toWire()},
      {"STORE_RANGE", range.toWire()}, {"COMMIT_RANGES", commit.toWire()}, {"ABORT_RANGES", abort.toWire()}}) {
    const auto response = dispatch(provider, makeRepoServiceName(node->servicePrefix(), entry.first), entry.second, entry.first);
    BOOST_CHECK(!response.getStatus());
    BOOST_CHECK_EQUAL(response.getErrorInfo().find("repo-publish-intent-required"), 0U);
    BOOST_CHECK(!node->core().has(artifactStorageObjectName(ref)));
    BOOST_CHECK(node->core().list().empty());
  }
  SqliteArtifactMetadataStore journal(metadataPath, false, 16, true);
  BOOST_CHECK(journal.pendingUserStorageWrites(capability.repoNode).empty());
  BOOST_CHECK_EQUAL(journal.userStorageUsage("/user/u", capability.repoNode).reservedBytes, 0U);
  sqlite3* raw = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_open_v2(metadataPath.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
  sqlite3_stmt* statement = nullptr;
  BOOST_REQUIRE_EQUAL(sqlite3_prepare_v2(db.get(), "SELECT COUNT(*) FROM artifact_persistent_receipts", -1, &statement, nullptr), SQLITE_OK);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
  BOOST_REQUIRE_EQUAL(sqlite3_step(query.get()), SQLITE_ROW);
  BOOST_CHECK_EQUAL(sqlite3_column_int64(query.get(), 0), 0);
  // LocalMock checks admission before mutation; network qualification is separate.
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::tests
