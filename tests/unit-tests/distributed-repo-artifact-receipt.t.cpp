#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/PersistentRepoServerConfig.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <openssl/sha.h>
#include <sqlite3.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <iterator>
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
  reference.logicalName = "/" + name;
  reference.contentDigest = digest(bytes);
  reference.sizeBytes = bytes.size();
  reference.rootManifestName = "/spec192/artifacts/" + name;
  reference.publisherIdentity = "/spec192/publisher";
  reference.policyEpoch = "repo-policy-v1";
  return reference;
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

} // namespace

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
    servicePrefix, capability, store, logicalRepo, provider.getName().toUri(),
    metadataPath);
  node->registerServices(provider);
  SqliteArtifactMetadataStore durableCatalog(metadataPath, false, 14);
  BOOST_CHECK_EQUAL(durableCatalog.schemaGeneration(), 14U);

  const std::vector<uint8_t> directBytes{0x10, 0x20, 0x30, 0x40};
  const auto directReference = makeReference("direct-store", directBytes);
  ArtifactStoreRequest direct;
  direct.operationId = "operation-direct";
  direct.generation = 1;
  direct.reference = directReference;
  direct.bytes = directBytes;
  const auto storeName = makeRepoServiceName(servicePrefix, "STORE");
  auto response = dispatch(provider, storeName, direct.toWire(), "direct-store");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto directLocation = ArtifactLocationRecord::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(directLocation.repoNode, logicalRepo);
  BOOST_CHECK_EQUAL(directLocation.providerIdentity, provider.getName().toUri());
  BOOST_CHECK(directLocation.reference == directReference);
  BOOST_CHECK_EQUAL(directLocation.operationId, direct.operationId);
  BOOST_CHECK_EQUAL(directLocation.generation, direct.generation);
  BOOST_CHECK_EQUAL(directLocation.locationVersion, 1U);
  BOOST_CHECK_EQUAL(directLocation.routeState, "ROUTABLE");
  BOOST_CHECK(durableCatalog.lookupPersistentLocation(
    directReference, logicalRepo, provider.getName().toUri(), 0).has_value());
  checkPersistentReceiptRow(metadataPath, directLocation);

  response = dispatch(provider, storeName, direct.toWire(), "direct-store-retry");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto retriedDirectLocation = ArtifactLocationRecord::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(retriedDirectLocation.toJson(), directLocation.toJson());
  checkPersistentReceiptRow(metadataPath, retriedDirectLocation);
  BOOST_CHECK(RepoClient::getRange(
    *node, artifactStorageObjectName(directReference),
    RepoByteRange{0, directBytes.size()}) == directBytes);

  auto conflictingDirect = direct;
  conflictingDirect.operationId = "operation-direct-conflict";
  conflictingDirect.generation = 2;
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
  const std::string operation = "operation-ranged";
  ArtifactRangeWriteRequest first;
  first.operationId = operation;
  first.generation = 1;
  first.reference = rangedReference;
  first.offsetBytes = 0;
  first.lengthBytes = 3;
  first.bytes = {1, 2, 3};
  const auto storeRangeName = makeRepoServiceName(servicePrefix, "STORE_RANGE");
  response = dispatch(provider, storeRangeName, first.toWire(), "ranged-first");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK(!durableCatalog.lookupPersistentLocation(
    rangedReference, logicalRepo, provider.getName().toUri(), 0));

  ArtifactRangeWriteRequest second = first;
  second.offsetBytes = 3;
  second.bytes = {4, 5, 6};
  response = dispatch(provider, storeRangeName, second.toWire(), "ranged-second");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK(!durableCatalog.lookupPersistentLocation(
    rangedReference, logicalRepo, provider.getName().toUri(), 0));

  ArtifactRangeCommitRequest commit;
  commit.operationId = operation;
  commit.generation = 1;
  commit.reference = rangedReference;
  const auto commitName = makeRepoServiceName(servicePrefix, "COMMIT_RANGES");
  response = dispatch(provider, commitName, commit.toWire(), "ranged-commit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto rangedLocation = ArtifactLocationRecord::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(rangedLocation.repoNode, logicalRepo);
  BOOST_CHECK_EQUAL(rangedLocation.providerIdentity, provider.getName().toUri());
  BOOST_CHECK(rangedLocation.reference == rangedReference);
  BOOST_CHECK_EQUAL(rangedLocation.operationId, operation);
  BOOST_CHECK_EQUAL(rangedLocation.generation, 1U);
  BOOST_CHECK_EQUAL(rangedLocation.routeState, "ROUTABLE");
  const auto localBytes = RepoClient::getRange(
    *node, artifactStorageObjectName(rangedReference), RepoByteRange{0, rangedBytes.size()});
  BOOST_CHECK(localBytes == rangedBytes);
  checkPersistentReceiptRow(metadataPath, rangedLocation);

  BOOST_CHECK_EQUAL(node->core().reapExpiredRangeReservations(0ms), 0U);
  response = dispatch(provider, storeRangeName, first.toWire(), "ranged-retry");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  response = dispatch(provider, commitName, commit.toWire(), "ranged-recommit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  const auto retriedRangedLocation = ArtifactLocationRecord::fromJson(responseString(response));
  BOOST_CHECK_EQUAL(retriedRangedLocation.toJson(), rangedLocation.toJson());
  BOOST_CHECK_EQUAL(node->core().reapExpiredRangeReservations(0ms), 0U);

  auto conflictingRange = first;
  conflictingRange.operationId = "operation-ranged-conflict";
  conflictingRange.generation = 2;
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
    servicePrefix, capability, store, logicalRepo, provider.getName().toUri(),
    metadataPath);
  node->registerServices(provider);

  const auto service = makeRepoServiceName(servicePrefix, "STORE_RANGE");
  const auto abortService = makeRepoServiceName(servicePrefix, "ABORT_RANGES");
  const auto commitService = makeRepoServiceName(servicePrefix, "COMMIT_RANGES");
  const std::vector<uint8_t> ownedBytes{0x10, 0x20, 0x30, 0x40};
  const auto ownedReference = makeReference("owner-bound", ownedBytes);
  ArtifactRangeWriteRequest write;
  write.operationId = "owner-bound-operation";
  write.generation = 1;
  write.reference = ownedReference;
  write.offsetBytes = 0;
  write.lengthBytes = ownedBytes.size();
  write.bytes = ownedBytes;
  auto response = dispatch(provider, service, write.toWire(), "range-owner-write");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());

  ArtifactRangeAbortRequest wrongOwner;
  wrongOwner.operationId = "different-operation";
  wrongOwner.generation = 1;
  wrongOwner.reference = ownedReference;
  const auto wrongWire = wrongOwner.toWire();
  BOOST_CHECK(ArtifactRangeAbortRequest::fromWire(wrongWire).operationId ==
              wrongOwner.operationId);
  response = dispatch(provider, abortService, wrongWire, "range-wrong-abort");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(responseString(response), "aborted");

  ArtifactRangeCommitRequest commit;
  commit.operationId = write.operationId;
  commit.generation = 1;
  commit.reference = ownedReference;
  response = dispatch(provider, commitService, commit.toWire(), "range-owned-commit");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK(node->core().has(artifactStorageObjectName(ownedReference)));
  BOOST_CHECK(RepoClient::getRange(
    *node, artifactStorageObjectName(ownedReference), {0, ownedBytes.size()}) == ownedBytes);

  const std::vector<uint8_t> abortedBytes{0x51, 0x52, 0x53};
  const auto abortedReference = makeReference("exact-abort", abortedBytes);
  write.operationId = "exact-abort-operation";
  write.reference = abortedReference;
  write.lengthBytes = abortedBytes.size();
  write.bytes = abortedBytes;
  response = dispatch(provider, service, write.toWire(), "range-exact-write");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  ArtifactRangeAbortRequest exactAbort;
  exactAbort.operationId = write.operationId;
  exactAbort.generation = 1;
  exactAbort.reference = abortedReference;
  response = dispatch(provider, abortService, exactAbort.toWire(), "range-exact-abort");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  commit.operationId = write.operationId;
  commit.reference = abortedReference;
  response = dispatch(provider, commitService, commit.toWire(), "range-aborted-commit");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(!node->core().has(artifactStorageObjectName(abortedReference)));

  const std::vector<uint8_t> expiredBytes{0x61, 0x62, 0x63, 0x64, 0x65};
  const auto expiredReference = makeReference("expired-range", expiredBytes);
  write.operationId = "expired-range-operation";
  write.reference = expiredReference;
  write.lengthBytes = expiredBytes.size();
  write.bytes = expiredBytes;
  response = dispatch(provider, service, write.toWire(), "range-expired-write");
  BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
  BOOST_CHECK_EQUAL(node->core().reapExpiredRangeReservations(0ms), 1U);
  commit.operationId = write.operationId;
  commit.reference = expiredReference;
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

BOOST_AUTO_TEST_CASE(ServerAndClientDefaultsAndOverrideKeepRegisteredStatusPrefix)
{
  BOOST_CHECK_EQUAL(RepoClient::DEFAULT_SERVICE_NAME,
                    "/NDNSF/DistributedRepo/Object/v1");
  PersistentRepoServerConfig serverConfig;
  BOOST_CHECK_EQUAL(serverConfig.servicePrefix.toUri(), RepoClient::DEFAULT_SERVICE_NAME);
  PersistentRepoClientConfig clientConfig;
  BOOST_CHECK_EQUAL(clientConfig.servicePrefix.toUri(), RepoClient::DEFAULT_SERVICE_NAME);

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
    capability.repoNode, provider.getName().toUri());
  node->registerServices(provider);

  const auto customStatus = makeRepoServiceName(clientConfig.servicePrefix, "STATUS");
  BOOST_CHECK_EQUAL(customStatus.toUri(), "/spec192/custom-repo/v2/STATUS");
  auto response = dispatch(provider, customStatus, {}, "custom-status");
  BOOST_CHECK(response.getStatus());
  const auto defaultStatus = makeRepoServiceName(
    ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), "STATUS");
  response = dispatch(provider, defaultStatus, {}, "wrong-default-status");
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK_EQUAL(response.getErrorInfo(),
                    "No handler registered for " + defaultStatus.toUri());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::tests
