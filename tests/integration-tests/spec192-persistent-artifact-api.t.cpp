#include "tests/boost-test.hpp"
#include "tests/fixtures/spec202-syscall-fault.hpp"
#include "ndnsf-integration-fixture.hpp"

#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include "ndn-service-framework/utils.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>

#include <openssl/sha.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <exception>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace ndn_service_framework::test {
namespace {

namespace fs = std::filesystem;
namespace repo = ndnsf_distributed_repo;
using namespace std::chrono_literals;

class TempRoot
{
public:
  TempRoot()
  {
    static std::atomic<uint64_t> sequence{0};
    m_path = fs::temp_directory_path() /
      ("spec192-persistent-api-" + std::to_string(getpid()) + "-" +
       std::to_string(sequence.fetch_add(1)));
    fs::create_directories(m_path);
  }
  ~TempRoot()
  {
    std::error_code error;
    fs::remove_all(m_path, error);
  }
  const fs::path& path() const noexcept { return m_path; }

private:
  fs::path m_path;
};

class ScopedRequestPublisher
{
public:
  ScopedRequestPublisher(
    ndn_service_framework::ServiceUser& user,
    ndn_service_framework::ServiceUser::RequestPublisher publisher)
    : m_user(user)
  {
    m_user.setRequestPublisher(std::move(publisher));
  }

  ~ScopedRequestPublisher()
  {
    m_user.setRequestPublisher({});
  }

private:
  ndn_service_framework::ServiceUser& m_user;
};

class ProducerSubscriptionGuard
{
public:
  ProducerSubscriptionGuard(ndn::svs::SVSPubSub& pubSub, uint32_t handle)
    : m_pubSub(pubSub)
    , m_handle(handle)
  {
  }

  ProducerSubscriptionGuard(const ProducerSubscriptionGuard&) = delete;
  ProducerSubscriptionGuard& operator=(const ProducerSubscriptionGuard&) = delete;

  ~ProducerSubscriptionGuard() noexcept
  {
    reset();
  }

  void reset() noexcept
  {
    if (!m_active) {
      return;
    }
    try {
      m_pubSub.unsubscribe(m_handle);
    }
    catch (...) {
      std::terminate();
    }
    m_active = false;
  }

private:
  ndn::svs::SVSPubSub& m_pubSub;
  uint32_t m_handle;
  bool m_active = true;
};

class ScopedThreadJoin
{
public:
  explicit ScopedThreadJoin(std::thread& thread)
    : m_thread(thread)
  {
  }

  ~ScopedThreadJoin()
  {
    if (m_thread.joinable()) m_thread.join();
  }

private:
  std::thread& m_thread;
};

void
queueSyntheticResponse(NdnsfIntegrationEnvironment& environment,
                       size_t signerIndex,
                       const ndn::Name& providerInResponseName,
                       const ndn::Name& serviceName,
                       const ndn::Name& requestId,
                       const ndn_service_framework::ResponseMessage& response)
{
  const auto responseName = ndn_service_framework::makeResponseNameV2(
    providerInResponseName, environment.user().getName(), serviceName, requestId);
  const auto responseWire = response.WireEncode();
  ndn::Buffer content(responseWire.data(),
                      responseWire.data() + responseWire.size());
  auto* pubSub = &environment.providerPubSub(signerIndex);
  environment.providerFace(signerIndex).getIoContext().post(
    [pubSub, responseName, content = std::move(content)] {
      pubSub->publish(responseName,
        ndn::span<const uint8_t>(content.data(), content.size()));
    });
}

void
queueAuthenticatedResponseForApiTest(
    NdnsfIntegrationEnvironment& environment,
    size_t signerIndex,
    const ndn::Name& providerIdentity,
    const ndn::Name& serviceName,
    const ndn::Name& requestId,
    ndn_service_framework::ResponseMessage response)
{
  // This is the post-authentication/decryption callback boundary consumed by
  // ArtifactRepositoryApi. The production Data name/KeyLocator gate is tested
  // separately; a raw Response TLV cannot stand in for a request-scoped AEAD
  // response when a Controller is configured.
  const auto responseName = ndn_service_framework::makeResponseNameV2(
    providerIdentity, environment.user().getName(), serviceName, requestId);
  response.setAuthenticatedTransportEvidence(
    responseName.toUri(),
    environment.provider(signerIndex).getSigningCertificateName().toUri(),
    "spec192-api-callback-fixture", providerIdentity.toUri());
  auto* user = &environment.user();
  environment.userFace().getIoContext().post(
    [user, providerIdentity, requestId, response = std::move(response)] {
      user->handleResponse(requestId, providerIdentity, response);
    });
}

ndn_service_framework::ResponseMessage
makeSyntheticResponse(ndn::Buffer payload)
{
  ndn_service_framework::ResponseMessage response;
  response.setStatus(true);
  response.setPayload(payload, payload.size());
  return response;
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

repo::ArtifactReference
makeReference(const std::string& name, const std::vector<uint8_t>& bytes)
{
  repo::ArtifactReference reference;
  reference.logicalName = "/" + name;
  reference.contentDigest = sha256(bytes);
  reference.sizeBytes = bytes.size();
  reference.rootManifestName = "/spec192/persistent/" + name;
  reference.publisherIdentity = "/test/publisher/spec192";
  reference.policyEpoch = "repo-policy-v1";
  return reference;
}

ndn_service_framework::ResponseMessage
dispatchRepoRequest(ndn_service_framework::ServiceProvider& provider,
                    const ndn::Name& requesterIdentity,
                    const ndn::Name& serviceName,
                    const std::vector<uint8_t>& payload,
                    const std::string& requestId)
{
  ndn_service_framework::RequestMessage request;
  ndn::Buffer buffer;
  if (!payload.empty()) buffer = ndn::Buffer(payload.data(), payload.size());
  request.setPayload(buffer, buffer.size());
  return provider.dispatchRequest(
    requesterIdentity, provider.getName(), serviceName,
    ndn::Name("/spec192/request").append(requestId), request);
}

void
releaseRepoNodeServiceHandlersForRestart(
  ndn_service_framework::ServiceProvider& provider,
  const ndn::Name& servicePrefix)
{
  const auto unavailableAck = [] (const ndn_service_framework::RequestMessage&) {
    ndn_service_framework::ServiceProvider::AckDecision decision;
    decision.status = false;
    decision.message = "repo-owner-restarting";
    return decision;
  };
  const auto unavailableRequest = [] (
    const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
    const ndn_service_framework::RequestMessage&) {
      ndn_service_framework::ResponseMessage response;
      response.setStatus(false);
      response.setErrorInfo("repo-owner-restarting");
      return response;
    };
  const std::vector<std::string> operations{
    "STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES", "INSERT",
    "STORE_MANIFEST", "FETCH", "FETCH_RANGE", "MANIFEST", "INVENTORY",
    "CAPABILITY", "CACHE_STATUS", "STATUS", "CATALOG_STATUS",
    "CATALOG_SNAPSHOT", "CATALOG_DELTA", "CATALOG_LOOKUP", "DELETE",
  };
  for (const auto& operation : operations) {
    provider.addService(repo::makeRepoServiceName(servicePrefix, operation),
                        unavailableAck, unavailableRequest);
  }
}

std::vector<uint8_t>
responseBytes(const ndn_service_framework::ResponseMessage& response)
{
  const auto payload = response.getPayload();
  return {payload.begin(), payload.end()};
}

void
writeFile(const fs::path& path, const std::vector<uint8_t>& bytes)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("could not create Persistent API source fixture");
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("could not write Persistent API source fixture");
}

std::vector<uint8_t>
readFile(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("could not open Persistent API oracle file");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

fs::path
committedPayloadPath(const fs::path& repoRoot,
                     const repo::ArtifactReference& reference)
{
  return repoRoot / "payloads" / reference.digestAlgorithm /
         reference.contentDigest.substr(0, 2) / reference.contentDigest;
}

size_t
repoManifestCount(const fs::path& repoRoot)
{
  const auto directory = repoRoot / "manifests";
  if (!fs::exists(directory)) return 0;
  return static_cast<size_t>(std::distance(
    fs::directory_iterator(directory), fs::directory_iterator{}));
}

int64_t
persistentReceiptRows(const fs::path& databasePath, const std::string& repoNode,
                      const std::string& providerIdentity,
                      const repo::ArtifactReference& reference)
{
  sqlite3* rawDatabase = nullptr;
  if (sqlite3_open_v2(databasePath.c_str(), &rawDatabase,
                      SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    const std::string error = rawDatabase ? sqlite3_errmsg(rawDatabase) : "open failed";
    if (rawDatabase) sqlite3_close(rawDatabase);
    throw std::runtime_error("receipt oracle database open failed: " + error);
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(rawDatabase, sqlite3_close);
  const char* sql =
    "SELECT COUNT(*) FROM artifact_persistent_receipts WHERE repo_node=? AND "
    "provider_identity=? AND logical_name=? AND digest_algorithm=? AND "
    "content_digest=? AND size_bytes=? AND format_version=? AND "
    "root_manifest_name=? AND publisher_identity=? AND policy_epoch=? AND "
    "state='COMMITTED'";
  sqlite3_stmt* rawStatement = nullptr;
  if (sqlite3_prepare_v2(database.get(), sql, -1, &rawStatement, nullptr) != SQLITE_OK) {
    throw std::runtime_error("receipt oracle query could not be prepared");
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
    rawStatement, sqlite3_finalize);
  const auto bindText = [&] (int index, const std::string& value) {
    if (sqlite3_bind_text(statement.get(), index, value.c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throw std::runtime_error("receipt oracle text binding failed");
    }
  };
  bindText(1, repoNode);
  bindText(2, providerIdentity);
  bindText(3, reference.logicalName);
  bindText(4, reference.digestAlgorithm);
  bindText(5, reference.contentDigest);
  if (sqlite3_bind_int64(statement.get(), 6, reference.sizeBytes) != SQLITE_OK) {
    throw std::runtime_error("receipt oracle size binding failed");
  }
  bindText(7, reference.formatVersion);
  bindText(8, reference.rootManifestName);
  bindText(9, reference.publisherIdentity);
  bindText(10, reference.policyEpoch);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    throw std::runtime_error("receipt oracle query returned no count");
  }
  return sqlite3_column_int64(statement.get(), 0);
}

template<typename Work>
auto
runWithFacePump(NdnsfIntegrationEnvironment& environment, Work&& work)
  -> decltype(work())
{
  auto pending = std::async(std::launch::async, std::forward<Work>(work));
  const auto deadline = std::chrono::steady_clock::now() + 120s;
  while (pending.wait_for(0ms) != std::future_status::ready) {
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error(
        "Spec192 persistent API call did not finish while pumping Faces");
    }
    // Each fixture pump is a bounded batch. Continue pumping because the first
    // request for a service may need a lazy DKEY fetch and more than one batch.
    environment.pumpUntilWithAttributeAuthority(
      [&] { return pending.wait_for(0ms) == std::future_status::ready; });
  }
  return pending.get();
}

repo::PublishOperationStatus
waitForTerminalPublish(NdnsfIntegrationEnvironment& environment,
                       const repo::ArtifactRepositoryApi& persistent,
                       const repo::PublishOperationHandle& handle)
{
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  size_t queryNumber = 0;
  const auto queryStatus = [&] {
    const auto currentQuery = ++queryNumber;
    std::fprintf(stderr,
                 "Spec192 status poll worker: entering getPublishStatus #%zu\n",
                 currentQuery);
    std::fflush(stderr);
    auto result = persistent.getPublishStatus(handle, 8000);
    std::fprintf(stderr,
                 "Spec192 status poll worker: returned from getPublishStatus #%zu state=%s\n",
                 currentQuery, result.state.c_str());
    std::fflush(stderr);
    return result;
  };
  BOOST_TEST_MESSAGE("Spec192 status poll: issuing first request");
  auto status = runWithFacePump(environment, queryStatus);
  BOOST_TEST_MESSAGE("Spec192 status poll: first response state=" << status.state);
  while (status.state != "COMMITTED" && status.state != "PARTIAL" &&
         status.state != "FAILED") {
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error("Spec192 publish operation did not become terminal");
    }
    std::this_thread::sleep_for(20ms);
    BOOST_TEST_MESSAGE("Spec192 status poll: issuing another request");
    status = runWithFacePump(environment, queryStatus);
    BOOST_TEST_MESSAGE("Spec192 status poll: response state=" << status.state);
  }
  return status;
}

uint64_t
currentLocationVersion(const fs::path& databasePath,
                       const repo::ArtifactReference& reference,
                       const std::string& repoNode,
                       const std::string& providerIdentity)
{
  repo::SqliteArtifactMetadataStore catalog(databasePath.string(), false, 15);
  const auto location = catalog.lookupPersistentLocation(
    reference, repoNode, providerIdentity, 0);
  if (!location) {
    throw std::runtime_error("Persistent API location oracle found no current row");
  }
  return location->locationVersion;
}

repo::ArtifactLocationRecord
currentLocationRecord(const fs::path& databasePath,
                      const repo::ArtifactReference& reference,
                      const std::string& repoNode,
                      const std::string& providerIdentity)
{
  repo::SqliteArtifactMetadataStore catalog(databasePath.string(), false, 15);
  const auto location = catalog.lookupPersistentLocation(
    reference, repoNode, providerIdentity, 0);
  if (!location) {
    throw std::runtime_error("Persistent API location oracle found no current row");
  }
  return *location;
}

template<typename BindValue>
void
mutateCurrentLocation(const fs::path& databasePath,
                      const repo::ArtifactReference& reference,
                      const std::string& repoNode,
                      const std::string& providerIdentity,
                      const char* sql, BindValue bindValue)
{
  sqlite3* rawDatabase = nullptr;
  if (sqlite3_open_v2(databasePath.c_str(), &rawDatabase,
                      SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    const std::string error = rawDatabase ? sqlite3_errmsg(rawDatabase) : "open failed";
    if (rawDatabase) sqlite3_close(rawDatabase);
    throw std::runtime_error("Persistent API mutation database open failed: " + error);
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(rawDatabase, sqlite3_close);
  sqlite3_stmt* rawStatement = nullptr;
  if (sqlite3_prepare_v2(database.get(), sql, -1, &rawStatement, nullptr) != SQLITE_OK) {
    throw std::runtime_error("Persistent API location mutation could not be prepared");
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
    rawStatement, sqlite3_finalize);
  if (bindValue(statement.get(), 1) != SQLITE_OK) {
    throw std::runtime_error("Persistent API location mutation value binding failed");
  }
  const auto bindText = [&] (int index, const std::string& value) {
    if (sqlite3_bind_text(statement.get(), index, value.c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throw std::runtime_error("Persistent API location key binding failed");
    }
  };
  bindText(2, repoNode);
  bindText(3, providerIdentity);
  bindText(4, reference.logicalName);
  bindText(5, reference.digestAlgorithm);
  bindText(6, reference.contentDigest);
  if (sqlite3_bind_int64(statement.get(), 7,
                         static_cast<sqlite3_int64>(reference.sizeBytes)) != SQLITE_OK) {
    throw std::runtime_error("Persistent API location size binding failed");
  }
  bindText(8, reference.formatVersion);
  bindText(9, reference.rootManifestName);
  bindText(10, reference.publisherIdentity);
  bindText(11, reference.policyEpoch);
  if (sqlite3_step(statement.get()) != SQLITE_DONE ||
      sqlite3_changes(database.get()) != 1) {
    throw std::runtime_error("Persistent API location mutation did not update one row");
  }
}

ndn::Name
repoService(const ndn::Name& prefix, const std::string& operation)
{
  auto service = prefix;
  service.append(operation);
  return service;
}

std::string
consistencyName(repo::RepoWriteConsistency consistency)
{
  switch (consistency) {
    case repo::RepoWriteConsistency::One: return "ONE";
    case repo::RepoWriteConsistency::Quorum: return "QUORUM";
    case repo::RepoWriteConsistency::All: return "ALL";
  }
  throw std::logic_error("unknown Repo write consistency");
}

std::shared_ptr<ndn_service_framework::ServiceUser>
makeRepoPublisherUser(NdnsfIntegrationEnvironment& environment, size_t index,
                      const ndn::Name& servicePrefix,
                      const std::vector<std::string>& operations)
{
  auto& keyChain = environment.keyChain();
  const auto providerIdentity = environment.provider(index).getName();
  const auto providerCert = keyChain.getPib().getIdentity(providerIdentity)
    .getDefaultKey().getDefaultCertificate();
  const auto authorityCert = keyChain.getPib().getIdentity(
    environment.profile().attributeAuthority)
    .getDefaultKey().getDefaultCertificate();
  auto publisherUser = std::make_shared<ndn_service_framework::ServiceUser>(
    ndn_service_framework::ServiceUser::LocalMockTag{},
    environment.providerFace(index), environment.profile().groupPrefix,
    providerCert, authorityCert, environment.profile().trustSchemaPath);
  publisherUser->useSigningKeyChainForTest(keyChain);

  ndn::svs::SecurityOptions security(keyChain);
  security.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  security.dataSigner->signingInfo =
    ndn::security::signingByCertificate(providerCert);
  security.pubSigner->signingInfo =
    ndn::security::signingByCertificate(providerCert);
  security.validator = std::make_shared<ndn::svs::BaseValidator>();
  security.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  ndn::svs::SVSPubSubOptions pubSubOptions;
  pubSubOptions.useTimestamp = false;
  // Provider ingress interprets the final producer-node component as the
  // process session ID. Keep each simulated publisher in its own namespace;
  // the provider index is not a session number shared across publishers.
  auto producerNode = environment.profile().providerNode;
  if (index != 0) {
    producerNode.append("p" + std::to_string(index));
  }
  producerNode.append("repo-publisher-user").append("0");
  auto pubSub = std::make_shared<ndn::svs::SVSPubSub>(
    environment.profile().syncPrefix, producerNode,
    environment.providerFace(index),
    [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
    pubSubOptions, security);
  publisherUser->attachLocalMockPubSubForTest(std::move(pubSub));

  ndn_service_framework::PermissionResponse permissions;
  permissions.setTargetIdentity(providerIdentity.toUri());
  permissions.setPermissionKind(ndn_service_framework::tlv::UserPermission);
  permissions.setPolicyEpoch(1);
  for (size_t targetIndex = 0; targetIndex < environment.providerCount(); ++targetIndex) {
    const auto targetProvider = environment.provider(targetIndex).getName();
    for (const auto& operation : operations) {
      ndn_service_framework::PermissionEntry entry;
      entry.setProviderName(targetProvider.toUri());
      entry.setServiceName(repoService(servicePrefix, operation).toUri());
      entry.setToken("");
      entry.setTtl(0);
      entry.setVersion(1);
      permissions.addEntry(entry);
    }
  }
  publisherUser->applyPermissionResponse(permissions);
  publisherUser->init();
  publisherUser->refreshNacProducerForTest();
  environment.pumpUntilWithAttributeAuthority([&] {
    return publisherUser->isNacConsumerReadyForTest() &&
           publisherUser->isNacProducerReadyForTest();
  });
  return publisherUser;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedRepoPersistentArtifactApi)

BOOST_AUTO_TEST_CASE(OperationOwnedArtifactWithoutReceiptStoreIsHidden)
{
  TempRoot temporary;
  const ndn::Name servicePrefix(repo::RepoClient::DEFAULT_SERVICE_NAME);
  repo::ArtifactLimits artifactLimits;
  const std::string repoNodeName = "/logical/spec192/no-receipt-store";
  repo::StorageCapability capability;
  capability.repoNode = repoNodeName;
  capability.repoMode = "persistent";
  capability.freeBytes = 1024 * 1024;
  const auto objects = temporary.path() / "no-receipt-objects";
  repo::RepoNode node(
    servicePrefix, capability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(
      objects.string(), 1024 * 1024, 4096, "spec192-no-receipt-store"),
    repoNodeName, "/test/provider/spec192/no-receipt-store", {}, artifactLimits);

  const std::vector<uint8_t> bytes{0x4e, 0x6f, 0x52, 0x65};
  const auto makeManifest = [&] (const std::string& name,
                                 const std::string& operationId) {
    const auto reference = makeReference(name, bytes);
    repo::RepoObjectManifest manifest;
    manifest.objectName = repo::artifactStorageObjectName(reference);
    manifest.objectType = "artifact-v2-bytes";
    manifest.sha256 = reference.contentDigest;
    manifest.size = reference.sizeBytes;
    manifest.segmentCount = 1;
    manifest.replicationFactor = 1;
    manifest.policyEpoch = reference.policyEpoch;
    manifest.generation = 1;
    manifest.writeConsistency = "ONE";
    manifest.requiredWriteAcks = 1;
    manifest.operationId = operationId;
    manifest.lifecycleState = "COMMITTED";
    return manifest;
  };
  const auto commitCoreObject = [&] (const repo::RepoObjectManifest& manifest) {
    node.core().putRange(
      manifest, repo::RepoByteRange{0, bytes.size()}, bytes);
    node.core().commitRanges(manifest);
  };

  const auto operationOwned = makeManifest(
    "no-receipt-store-operation-owned", "operation-no-receipt-store");
  commitCoreObject(operationOwned);
  BOOST_CHECK(node.core().get(operationOwned.objectName) == bytes);
  BOOST_CHECK_EXCEPTION(
    node.get(operationOwned.objectName), std::out_of_range,
    [] (const std::out_of_range& error) {
      return std::string(error.what()).find("repo-artifact-not-accepted:") == 0;
    });
  const auto visible = node.list();
  BOOST_CHECK(std::none_of(visible.begin(), visible.end(), [&] (const auto& manifest) {
    return manifest.objectName == operationOwned.objectName;
  }));

  const auto legacy = makeManifest("no-receipt-store-legacy", "");
  commitCoreObject(legacy);
  BOOST_CHECK(node.get(legacy.objectName) == bytes);
}

BOOST_AUTO_TEST_CASE(RecoveredPendingPublishTargetGetsOneDurableReceipt)
{
  TempRoot temporary;
  const ndn::Name servicePrefix(repo::RepoClient::DEFAULT_SERVICE_NAME);
  repo::ArtifactLimits artifactLimits;
  artifactLimits.maxChunkBytes = 4;
  BootstrapProfile profile;
  profile.groupPrefix = "/ndnsf/spec192/recovery/group";
  profile.syncPrefix = "/ndnsf/spec192/recovery/sync";
  profile.userNode = "/ndnsf/spec192/recovery/user";
  profile.providerNode = "/ndnsf/spec192/recovery/provider";
  profile.userIdentity = "/test/user/spec192/recovery";
  profile.providerIdentity = "/test/provider/spec192/recovery";
  profile.attributeAuthority = "/test/aa/spec192/recovery";
  profile.providerCount = 3;
  profile.providerCanActAsUser = true;
  const std::vector<std::string> operations{
    "CAPABILITY", "STATUS", "STORE", "STORE_RANGE", "COMMIT_RANGES",
    "ABORT_RANGES", "STORE_MANIFEST", "INSERT", "DELETE", "FETCH", "FETCH_RANGE",
    "MANIFEST", "INVENTORY", "CATALOG_STATUS", "CATALOG_SNAPSHOT",
    "CATALOG_DELTA", "CATALOG_LOOKUP",
  };
  profile.serviceName = repoService(servicePrefix, operations.front());
  for (size_t index = 1; index < operations.size(); ++index) {
    profile.additionalServices.push_back(repoService(servicePrefix, operations[index]));
  }

  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  std::vector<std::shared_ptr<ndn_service_framework::ServiceUser>> publisherUsers;
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    publisherUsers.push_back(makeRepoPublisherUser(
      environment, index, servicePrefix, operations));
  }

  const std::string seedRepo = "/logical/spec192/recovery-seed";
  const std::string replicaRepo = "/logical/spec192/recovery-replica";
  const std::string unselectedRepo = "/logical/spec192/recovery-unselected";
  const auto seedProvider = environment.provider(0).getName();
  const auto replicaProvider = environment.provider(1).getName();
  const auto unselectedProvider = environment.provider(2).getName();
  const repo::RepoArtifactTarget seed{
    seedRepo, seedProvider.toUri(), servicePrefix.toUri()};
  const repo::RepoArtifactTarget replica{
    replicaRepo, replicaProvider.toUri(), servicePrefix.toUri()};
  const repo::RepoArtifactTarget unselected{
    unselectedRepo, unselectedProvider.toUri(), servicePrefix.toUri()};
  const auto seedRoot = temporary.path() / "seed";
  const auto seedObjects = seedRoot / "objects";
  const auto seedDatabase = seedRoot / "artifact-v2.sqlite";
  const auto replicaRoot = temporary.path() / "replica";
  const auto replicaObjects = replicaRoot / "objects";
  const auto replicaDatabase = replicaRoot / "artifact-v2.sqlite";
  const auto unselectedRoot = temporary.path() / "unselected";
  const auto unselectedObjects = unselectedRoot / "objects";
  const auto unselectedDatabase = unselectedRoot / "artifact-v2.sqlite";
  repo::StorageCapability seedCapability;
  seedCapability.repoNode = seedRepo;
  seedCapability.repoMode = "persistent";
  seedCapability.freeBytes = 8 * 1024 * 1024;
  repo::StorageCapability replicaCapability = seedCapability;
  replicaCapability.repoNode = replicaRepo;

  auto replicaNode = std::make_shared<repo::RepoNode>(
    servicePrefix, replicaCapability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(
      replicaObjects.string(), 1024 * 1024, 4096, "spec192-recovery-replica"),
    replicaRepo, replicaProvider.toUri(), replicaDatabase.string(), artifactLimits);
  replicaNode->registerServices(environment.provider(1), publisherUsers[1], 1500, 8000);
  repo::StorageCapability unselectedCapability = seedCapability;
  unselectedCapability.repoNode = unselectedRepo;
  auto unselectedNode = std::make_shared<repo::RepoNode>(
    servicePrefix, unselectedCapability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(
      unselectedObjects.string(), 1024 * 1024, 4096, "spec192-recovery-unselected"),
    unselectedRepo, unselectedProvider.toUri(), unselectedDatabase.string(), artifactLimits);
  unselectedNode->registerServices(
    environment.provider(2), publisherUsers[2], 1500, 8000);
  BOOST_REQUIRE(unselectedNode);
  auto seedNode = std::make_shared<repo::RepoNode>(
    servicePrefix, seedCapability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(
      seedObjects.string(), 1024 * 1024, 4096, "spec192-recovery-seed"),
    seedRepo, seedProvider.toUri(), seedDatabase.string(), artifactLimits);
  seedNode->registerServices(environment.provider(0));

  environment.enableProductionIngressForTest();
  environment.pumpUntilWithAttributeAuthority([&] {
    return std::all_of(publisherUsers.begin(), publisherUsers.end(), [] (const auto& user) {
      return user->isNacConsumerReadyForTest() && user->isNacProducerReadyForTest();
    });
  });

  const std::vector<uint8_t> bytes{0x53, 0x70, 0x65, 0x63};
  auto reference = makeReference("restart-pending-target", bytes);
  const auto canonicalDigest = reference.contentDigest;
  std::transform(reference.contentDigest.begin(), reference.contentDigest.end(),
                 reference.contentDigest.begin(), [] (char value) {
    return value >= 'a' && value <= 'f' ? static_cast<char>(value - 'a' + 'A') : value;
  });
  BOOST_REQUIRE(reference.contentDigest != canonicalDigest);
  const std::string idempotencyKey = "spec192-recovery-pending-target-key";
  repo::PublishOperationIntent intent;
  intent.idempotencyKeyDigest = repo::digestPublishIdempotencyKey(idempotencyKey);
  intent.reference = reference;
  intent.generation = 1;
  intent.seed = seed;
  intent.consistency = "ALL";
  intent.targets = {seed, replica};
  BOOST_CHECK(std::find(intent.targets.begin(), intent.targets.end(), unselected) ==
              intent.targets.end());
  intent.requestDigest = repo::makePublishRequestDigest(
    reference, intent.generation, seed, intent.consistency, intent.targets);
  const auto submittingPrincipal = environment.user().getName().toUri();
  const auto operationId = repo::makePublishOperationId(
    submittingPrincipal, seed, intent.idempotencyKeyDigest);

  const auto intentJson = intent.toJson();
  const std::vector<uint8_t> intentWire(intentJson.begin(), intentJson.end());
  const auto storeService = repoService(servicePrefix, "STORE");
  const auto storeRangeService = repoService(servicePrefix, "STORE_RANGE");
  const auto commitRangesService = repoService(servicePrefix, "COMMIT_RANGES");
  const auto abortRangesService = repoService(servicePrefix, "ABORT_RANGES");
  const auto fetchService = repoService(servicePrefix, "FETCH");
  const auto fetchRangeService = repoService(servicePrefix, "FETCH_RANGE");
  const auto manifestService = repoService(servicePrefix, "MANIFEST");
  const auto inventoryService = repoService(servicePrefix, "INVENTORY");
  const auto catalogSnapshotService = repoService(servicePrefix, "CATALOG_SNAPSHOT");
  const auto catalogDeltaService = repoService(servicePrefix, "CATALOG_DELTA");
  const auto catalogLookupService = repoService(servicePrefix, "CATALOG_LOOKUP");
  const auto catalogStatusService = repoService(servicePrefix, "CATALOG_STATUS");
  const auto storeManifestService = repoService(servicePrefix, "STORE_MANIFEST");
  const auto insertService = repoService(servicePrefix, "INSERT");
  const auto deleteService = repoService(servicePrefix, "DELETE");
  const std::vector<uint8_t> unacceptedBytes{0x75, 0x6e, 0x61, 0x75};
  const auto unacceptedReference = makeReference(
    "matching-owner-without-journal", unacceptedBytes);
  const auto verifyUnacceptedSeedState = [&] {
    const auto objectName = repo::artifactStorageObjectName(unacceptedReference);
    BOOST_CHECK(!seedNode->core().has(objectName));
    BOOST_CHECK(!fs::exists(committedPayloadPath(seedObjects, unacceptedReference)));
    BOOST_CHECK_EQUAL(repoManifestCount(seedObjects), 0U);
    BOOST_CHECK_EQUAL(persistentReceiptRows(
      seedDatabase, seedRepo, seedProvider.toUri(), unacceptedReference), 0);
    repo::SqliteArtifactMetadataStore catalog(seedDatabase.string(), false, 15);
    BOOST_CHECK(!catalog.lookupPersistentLocation(
      unacceptedReference, seedRepo, seedProvider.toUri(), 0));
  };
  repo::ArtifactStoreRequest unacceptedStore;
  unacceptedStore.operationId = repo::makePublishOperationId(
    submittingPrincipal, seed,
    repo::digestPublishIdempotencyKey("spec192-unaccepted-owner-key"));
  unacceptedStore.generation = 1;
  unacceptedStore.reference = unacceptedReference;
  unacceptedStore.bytes = unacceptedBytes;
  const auto makeArtifactManifestForWrite = [] (
      const repo::ArtifactReference& reference, const std::string& operationId,
      uint64_t generation) {
    repo::RepoObjectManifest manifest;
    manifest.objectName = repo::artifactStorageObjectName(reference);
    manifest.objectType = "artifact-v2-bytes";
    manifest.sha256 = reference.contentDigest;
    std::transform(manifest.sha256.begin(), manifest.sha256.end(),
                   manifest.sha256.begin(), [] (char value) {
      return value >= 'A' && value <= 'F' ? static_cast<char>(value - 'A' + 'a') : value;
    });
    manifest.size = reference.sizeBytes;
    manifest.segmentCount = 1;
    manifest.replicationFactor = 1;
    manifest.policyEpoch = reference.policyEpoch;
    manifest.generation = generation;
    manifest.writeConsistency = "ONE";
    manifest.requiredWriteAcks = 1;
    manifest.operationId = operationId;
    manifest.lifecycleState = "COMMITTED";
    return manifest;
  };
  const auto unacceptedManifest = makeArtifactManifestForWrite(
    unacceptedReference, unacceptedStore.operationId, unacceptedStore.generation);
  const auto dispatchUnaccepted = [&] (const ndn::Name& service,
                                       const std::vector<uint8_t>& wire,
                                       const std::string& requestId) {
    return runWithFacePump(environment, [&] {
      return dispatchRepoRequest(environment.provider(0), environment.user().getName(),
                                 service, wire, requestId);
    });
  };
  size_t readProbeSequence = 0;
  const auto assertArtifactVisibility = [&] (
      repo::RepoNode& node, size_t providerIndex,
      const repo::RepoObjectManifest& manifest,
      const std::vector<uint8_t>& expectedBytes, bool visible,
      const std::string& label) {
    const auto objectName = manifest.objectName;
    const auto objectNameWire = repo::toBytes(objectName);
    const auto request = [&] (const ndn::Name& service,
                              const std::vector<uint8_t>& payload,
                              const std::string& suffix) {
      return dispatchRepoRequest(
        environment.provider(providerIndex), environment.user().getName(), service,
        payload, label + "-" + suffix + "-" + std::to_string(readProbeSequence++));
    };
    const auto checkRead = [&] (const ndn_service_framework::ResponseMessage& response,
                                const std::vector<uint8_t>& expected,
                                const std::string& suffix) {
      if (visible) {
        BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
        BOOST_CHECK(responseBytes(response) == expected);
      }
      else {
        BOOST_CHECK_MESSAGE(!response.getStatus(), suffix + ": " + response.getErrorInfo());
      }
    };
    const auto checkListed = [&] (const ndn_service_framework::ResponseMessage& response,
                                  const std::string& suffix) {
      BOOST_REQUIRE_MESSAGE(response.getStatus(), response.getErrorInfo());
      const auto payload = responseBytes(response);
      const std::string json(payload.begin(), payload.end());
      BOOST_CHECK_MESSAGE((json.find(objectName) != std::string::npos) == visible,
                          suffix + " visibility differs");
    };
    const auto rejectNotAccepted = [] (const std::exception& error) {
      return std::string(error.what()).find("repo-artifact-not-accepted:") !=
             std::string::npos;
    };

    // The RepoCore object is deliberately present at this crash boundary;
    // public Persistent Repo reads must wait for its local durable receipt.
    BOOST_CHECK(node.core().get(objectName) == expectedBytes);
    if (visible) {
      BOOST_CHECK(node.get(objectName) == expectedBytes);
      BOOST_CHECK(node.getRange(objectName, {0, expectedBytes.size()}) == expectedBytes);
      BOOST_CHECK_EQUAL(node.getManifest(objectName).objectName, objectName);
    }
    else {
      BOOST_CHECK_EXCEPTION(node.get(objectName), std::out_of_range, rejectNotAccepted);
      BOOST_CHECK_EXCEPTION((node.getRange(objectName, {0, expectedBytes.size()})),
                            std::out_of_range, rejectNotAccepted);
      BOOST_CHECK_EXCEPTION(node.getManifest(objectName),
                            std::out_of_range, rejectNotAccepted);
    }
    const auto localManifests = node.list();
    const auto localEntry = std::find_if(localManifests.begin(), localManifests.end(),
      [&objectName] (const auto& item) { return item.objectName == objectName; });
    BOOST_CHECK_EQUAL(localEntry != localManifests.end(), visible);

    checkRead(request(fetchService, objectNameWire, "fetch"), expectedBytes, "fetch");
    checkRead(request(fetchRangeService,
                      repo::encodeRangeReadRequest(objectName, {0, expectedBytes.size()}),
                      "fetch-range"), expectedBytes, "fetch-range");
    const auto manifestResponse = request(manifestService, objectNameWire, "manifest");
    if (visible) {
      BOOST_REQUIRE_MESSAGE(manifestResponse.getStatus(), manifestResponse.getErrorInfo());
      const auto payload = responseBytes(manifestResponse);
      const std::string json(payload.begin(), payload.end());
      BOOST_CHECK(json.find(objectName) != std::string::npos);
    }
    else {
      BOOST_CHECK_MESSAGE(!manifestResponse.getStatus(), manifestResponse.getErrorInfo());
    }
    checkListed(request(inventoryService, {}, "inventory"), "inventory");
    checkListed(request(catalogSnapshotService, {}, "catalog-snapshot"),
                "catalog-snapshot");
    checkListed(request(catalogDeltaService, repo::toBytes("0"), "catalog-delta"),
                "catalog-delta");
    const auto lookupResponse = request(catalogLookupService, objectNameWire,
                                        "catalog-lookup");
    if (visible) {
      BOOST_REQUIRE_MESSAGE(lookupResponse.getStatus(), lookupResponse.getErrorInfo());
      const auto payload = responseBytes(lookupResponse);
      const std::string json(payload.begin(), payload.end());
      BOOST_CHECK(json.find(objectName) != std::string::npos);
    }
    else {
      BOOST_CHECK_MESSAGE(!lookupResponse.getStatus(), lookupResponse.getErrorInfo());
    }
    const auto catalogStatusResponse = request(
      catalogStatusService, {}, "catalog-status");
    BOOST_REQUIRE_MESSAGE(catalogStatusResponse.getStatus(),
                          catalogStatusResponse.getErrorInfo());
    const auto catalogStatusPayload = responseBytes(catalogStatusResponse);
    const std::string catalogStatusJson(
      catalogStatusPayload.begin(), catalogStatusPayload.end());
    const auto expectedVisibleCount = "\"objectCount\":" +
      std::to_string(node.list().size());
    BOOST_CHECK_MESSAGE(catalogStatusJson.find(expectedVisibleCount) != std::string::npos,
                        "catalog status count differs from the visible local listing");
  };
  auto denied = dispatchUnaccepted(
    storeService, unacceptedStore.toWire(artifactLimits), "unaccepted-owner-store");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());
  verifyUnacceptedSeedState();
  const auto seedStagingDirectory = seedObjects / "staging";
  if (fs::exists(seedStagingDirectory)) {
    BOOST_CHECK(fs::is_empty(seedStagingDirectory));
  }
  const std::vector<uint8_t> firstHalf(
    unacceptedBytes.begin(), unacceptedBytes.begin() + 2);
  const std::vector<uint8_t> secondHalf(
    unacceptedBytes.begin() + 2, unacceptedBytes.end());
  seedNode->core().putRange(
    unacceptedManifest, repo::RepoByteRange{0, firstHalf.size()}, firstHalf);
  repo::FilesystemArtifactPayloadStore seedPayloadStore(seedObjects.string());
  const repo::ArtifactByteRange fullUnacceptedRange{0, unacceptedBytes.size()};
  const auto stagedBytesBefore = seedPayloadStore.readRange(
    unacceptedReference, unacceptedStore.generation, fullUnacceptedRange);
  const auto stagedRangesBefore = seedPayloadStore.verifiedRanges(
    unacceptedReference, unacceptedStore.generation);
  BOOST_REQUIRE_EQUAL(stagedRangesBefore.size(), 1U);
  BOOST_CHECK_EQUAL(stagedRangesBefore.front().offsetBytes, 0U);
  BOOST_CHECK_EQUAL(stagedRangesBefore.front().lengthBytes, firstHalf.size());
  BOOST_CHECK(stagedBytesBefore == std::vector<uint8_t>(
    {firstHalf[0], firstHalf[1], 0, 0}));
  repo::ArtifactRangeWriteRequest unacceptedRange;
  unacceptedRange.operationId = unacceptedStore.operationId;
  unacceptedRange.generation = unacceptedStore.generation;
  unacceptedRange.reference = unacceptedReference;
  unacceptedRange.offsetBytes = firstHalf.size();
  unacceptedRange.lengthBytes = secondHalf.size();
  unacceptedRange.bytes = secondHalf;
  denied = dispatchUnaccepted(
    storeRangeService, unacceptedRange.toWire(artifactLimits), "unaccepted-owner-range");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());
  verifyUnacceptedSeedState();
  const auto stagedBytesAfter = seedPayloadStore.readRange(
    unacceptedReference, unacceptedStore.generation, fullUnacceptedRange);
  const auto stagedRangesAfter = seedPayloadStore.verifiedRanges(
    unacceptedReference, unacceptedStore.generation);
  BOOST_CHECK(stagedBytesAfter == stagedBytesBefore);
  BOOST_REQUIRE_EQUAL(stagedRangesAfter.size(), stagedRangesBefore.size());
  BOOST_CHECK_EQUAL(stagedRangesAfter.front().offsetBytes,
                    stagedRangesBefore.front().offsetBytes);
  BOOST_CHECK_EQUAL(stagedRangesAfter.front().lengthBytes,
                    stagedRangesBefore.front().lengthBytes);
  seedNode->core().abortRanges(unacceptedManifest.objectName);
  seedNode->core().putRange(
    unacceptedManifest, repo::RepoByteRange{0, unacceptedBytes.size()}, unacceptedBytes);
  const auto unacceptedFullBytesBefore = seedPayloadStore.readRange(
    unacceptedReference, unacceptedStore.generation, fullUnacceptedRange);
  const auto unacceptedFullRangesBefore = seedPayloadStore.verifiedRanges(
    unacceptedReference, unacceptedStore.generation);
  BOOST_REQUIRE_EQUAL(unacceptedFullRangesBefore.size(), 1U);
  BOOST_CHECK_EQUAL(unacceptedFullRangesBefore.front().offsetBytes, 0U);
  BOOST_CHECK_EQUAL(unacceptedFullRangesBefore.front().lengthBytes, unacceptedBytes.size());
  repo::ArtifactRangeCommitRequest unacceptedCommit;
  unacceptedCommit.operationId = unacceptedStore.operationId;
  unacceptedCommit.generation = unacceptedStore.generation;
  unacceptedCommit.reference = unacceptedReference;
  denied = dispatchUnaccepted(
    commitRangesService, unacceptedCommit.toWire(artifactLimits), "unaccepted-owner-commit");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());
  verifyUnacceptedSeedState();
  const auto unacceptedFullBytesAfter = seedPayloadStore.readRange(
    unacceptedReference, unacceptedStore.generation, fullUnacceptedRange);
  const auto unacceptedFullRangesAfter = seedPayloadStore.verifiedRanges(
    unacceptedReference, unacceptedStore.generation);
  BOOST_CHECK(unacceptedFullBytesAfter == unacceptedFullBytesBefore);
  BOOST_REQUIRE_EQUAL(unacceptedFullRangesAfter.size(), unacceptedFullRangesBefore.size());
  BOOST_CHECK_EQUAL(unacceptedFullRangesAfter.front().offsetBytes,
                    unacceptedFullRangesBefore.front().offsetBytes);
  BOOST_CHECK_EQUAL(unacceptedFullRangesAfter.front().lengthBytes,
                    unacceptedFullRangesBefore.front().lengthBytes);
  seedNode->core().abortRanges(unacceptedManifest.objectName);

  const auto makeLegacyManifest = [] (const repo::ArtifactReference& reference,
                                      const std::string& operationId) {
    repo::RepoObjectManifest manifest;
    manifest.objectName = repo::artifactStorageObjectName(reference);
    manifest.objectType = "artifact-v2-bytes";
    manifest.sha256 = reference.contentDigest;
    manifest.size = reference.sizeBytes;
    manifest.generation = 1;
    manifest.operationId = operationId;
    manifest.policyEpoch = reference.policyEpoch;
    return manifest;
  };
  const auto dispatchLegacyMutation = [&] (const ndn::Name& service,
                                            const std::vector<uint8_t>& wire,
                                            const std::string& requestId) {
    return dispatchRepoRequest(environment.provider(2), environment.user().getName(),
                               service, wire, requestId);
  };

  // Persistent artifact bytes are writable only through the journal-bound
  // artifact envelopes. Exercise each exposed generic Repo mutation against
  // the production handlers, using the exact object names the artifact API
  // derives so a legacy request cannot bypass receipt ownership.
  const auto legacyStoreReference = makeReference("legacy-generic-store", unacceptedBytes);
  const auto legacyStoreManifest = makeLegacyManifest(
    legacyStoreReference, "spec192-legacy-store");
  denied = dispatchLegacyMutation(
    storeService, repo::encodeStoreRequest(legacyStoreManifest, unacceptedBytes),
    "legacy-generic-store");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());
  BOOST_CHECK(!unselectedNode->core().has(legacyStoreManifest.objectName));

  const auto legacyRangeReference = makeReference("legacy-generic-range", unacceptedBytes);
  const auto legacyRangeManifest = makeLegacyManifest(
    legacyRangeReference, "spec192-legacy-range");
  const std::vector<uint8_t> rangeFirstHalf(
    unacceptedBytes.begin(), unacceptedBytes.begin() + 2);
  const std::vector<uint8_t> rangeSecondHalf(
    unacceptedBytes.begin() + 2, unacceptedBytes.end());
  unselectedNode->core().putRange(
    legacyRangeManifest, repo::RepoByteRange{0, rangeFirstHalf.size()}, rangeFirstHalf);
  denied = dispatchLegacyMutation(
    storeRangeService,
    repo::encodeRangeWriteRequest(
      legacyRangeManifest,
      repo::RepoByteRange{rangeFirstHalf.size(), rangeSecondHalf.size()}, rangeSecondHalf),
    "legacy-generic-range");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());
  BOOST_CHECK_THROW(unselectedNode->core().getRange(
    legacyRangeManifest.objectName, repo::RepoByteRange{0, unacceptedBytes.size()}),
    std::exception);
  unselectedNode->core().abortRanges(legacyRangeManifest.objectName);

  const auto legacyCommitReference = makeReference("legacy-generic-commit", unacceptedBytes);
  const auto legacyCommitManifest = makeLegacyManifest(
    legacyCommitReference, "spec192-legacy-commit");
  repo::FilesystemArtifactPayloadStore legacyCommitPayloadStore(
    unselectedObjects.string());
  const repo::ArtifactByteRange legacyCommitRange{
    0, legacyCommitReference.sizeBytes};
  unselectedNode->core().putRange(
    legacyCommitManifest, repo::RepoByteRange{0, unacceptedBytes.size()}, unacceptedBytes);
  const auto legacyStagedBytesBefore = legacyCommitPayloadStore.readRange(
    legacyCommitReference, legacyCommitManifest.generation, legacyCommitRange);
  const auto legacyStagedRangesBefore = legacyCommitPayloadStore.verifiedRanges(
    legacyCommitReference, legacyCommitManifest.generation);
  BOOST_REQUIRE(legacyStagedBytesBefore == unacceptedBytes);
  BOOST_REQUIRE_EQUAL(legacyStagedRangesBefore.size(), 1U);
  BOOST_REQUIRE_EQUAL(legacyStagedRangesBefore.front().offsetBytes, 0U);
  BOOST_REQUIRE_EQUAL(legacyStagedRangesBefore.front().lengthBytes,
                      legacyCommitReference.sizeBytes);
  denied = dispatchLegacyMutation(
    commitRangesService, repo::encodeManifestRequest(legacyCommitManifest),
    "legacy-generic-commit");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());
  BOOST_CHECK(!unselectedNode->core().has(legacyCommitManifest.objectName));
  const auto legacyStagedBytesAfter = legacyCommitPayloadStore.readRange(
    legacyCommitReference, legacyCommitManifest.generation, legacyCommitRange);
  const auto legacyStagedRangesAfter = legacyCommitPayloadStore.verifiedRanges(
    legacyCommitReference, legacyCommitManifest.generation);
  BOOST_CHECK(legacyStagedBytesAfter == legacyStagedBytesBefore);
  BOOST_REQUIRE_EQUAL(legacyStagedRangesAfter.size(), legacyStagedRangesBefore.size());
  BOOST_CHECK_EQUAL(legacyStagedRangesAfter.front().offsetBytes,
                    legacyStagedRangesBefore.front().offsetBytes);
  BOOST_CHECK_EQUAL(legacyStagedRangesAfter.front().lengthBytes,
                    legacyStagedRangesBefore.front().lengthBytes);
  unselectedNode->core().abortRanges(legacyCommitManifest.objectName);

  const auto legacyAbortReference = makeReference("legacy-generic-abort", unacceptedBytes);
  const auto legacyAbortManifest = makeLegacyManifest(
    legacyAbortReference, "spec192-legacy-abort");
  repo::FilesystemArtifactPayloadStore legacyAbortPayloadStore(
    unselectedObjects.string());
  const repo::ArtifactByteRange legacyAbortRange{
    0, legacyAbortReference.sizeBytes};
  unselectedNode->core().putRange(
    legacyAbortManifest, repo::RepoByteRange{0, unacceptedBytes.size()}, unacceptedBytes);
  const auto legacyAbortStagedBytesBefore = legacyAbortPayloadStore.readRange(
    legacyAbortReference, legacyAbortManifest.generation, legacyAbortRange);
  const auto legacyAbortStagedRangesBefore = legacyAbortPayloadStore.verifiedRanges(
    legacyAbortReference, legacyAbortManifest.generation);
  BOOST_REQUIRE(legacyAbortStagedBytesBefore == unacceptedBytes);
  BOOST_REQUIRE_EQUAL(legacyAbortStagedRangesBefore.size(), 1U);
  BOOST_REQUIRE_EQUAL(legacyAbortStagedRangesBefore.front().offsetBytes, 0U);
  BOOST_REQUIRE_EQUAL(legacyAbortStagedRangesBefore.front().lengthBytes,
                      legacyAbortReference.sizeBytes);
  denied = dispatchLegacyMutation(
    abortRangesService, repo::encodeRangeAbortRequest(legacyAbortManifest.objectName),
    "legacy-generic-abort");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());
  const auto legacyAbortStagedBytesAfter = legacyAbortPayloadStore.readRange(
    legacyAbortReference, legacyAbortManifest.generation, legacyAbortRange);
  const auto legacyAbortStagedRangesAfter = legacyAbortPayloadStore.verifiedRanges(
    legacyAbortReference, legacyAbortManifest.generation);
  BOOST_CHECK(legacyAbortStagedBytesAfter == legacyAbortStagedBytesBefore);
  BOOST_REQUIRE_EQUAL(legacyAbortStagedRangesAfter.size(),
                      legacyAbortStagedRangesBefore.size());
  BOOST_CHECK_EQUAL(legacyAbortStagedRangesAfter.front().offsetBytes,
                    legacyAbortStagedRangesBefore.front().offsetBytes);
  BOOST_CHECK_EQUAL(legacyAbortStagedRangesAfter.front().lengthBytes,
                    legacyAbortStagedRangesBefore.front().lengthBytes);
  unselectedNode->core().abortRanges(legacyAbortManifest.objectName);

  const auto legacyManifestReference = makeReference(
    "legacy-generic-manifest", unacceptedBytes);
  auto legacyManifest = makeLegacyManifest(
    legacyManifestReference, "spec192-legacy-manifest");
  // If the persistent network guard is missing, this valid metadata-only
  // manifest must be accepted by the backing store, making the no-write
  // oracle exercise the guard rather than payload validation.
  legacyManifest.segmentCount = 2;
  const auto manifestsBeforeLegacyManifest = repoManifestCount(unselectedObjects);
  BOOST_CHECK(!unselectedNode->core().has(legacyManifest.objectName));
  denied = dispatchLegacyMutation(
    storeManifestService, repo::encodeManifestRequest(legacyManifest),
    "legacy-generic-manifest");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());
  BOOST_CHECK(!unselectedNode->core().has(legacyManifest.objectName));
  BOOST_CHECK_EQUAL(repoManifestCount(unselectedObjects), manifestsBeforeLegacyManifest);

  const auto legacyInsertReference = makeReference("legacy-generic-insert", unacceptedBytes);
  repo::RepoDataReference dataReference;
  dataReference.objectName = repo::artifactStorageObjectName(legacyInsertReference);
  dataReference.dataPrefix = "/spec192/legacy-source";
  dataReference.firstSegment = 0;
  dataReference.finalSegment = 0;
  dataReference.hasFinalSegment = true;
  denied = dispatchLegacyMutation(
    insertService, repo::encodeDataReferenceRequest(dataReference),
    "legacy-generic-insert");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());

  const auto legacyDeleteReference = makeReference("legacy-generic-delete", unacceptedBytes);
  const auto legacyDeleteName = repo::artifactStorageObjectName(legacyDeleteReference);
  unselectedNode->core().put(legacyDeleteName, unacceptedBytes,
                             "artifact-v2-bytes", 1, legacyDeleteReference.policyEpoch);
  denied = dispatchLegacyMutation(
    deleteService, repo::toBytes(legacyDeleteName), "legacy-generic-delete");
  BOOST_CHECK_MESSAGE(!denied.getStatus(), denied.getErrorInfo());
  BOOST_CHECK(unselectedNode->core().has(legacyDeleteName));
  (void)unselectedNode->core().handleDelete(repo::toBytes(legacyDeleteName));

  BOOST_CHECK_EQUAL(persistentReceiptRows(
    unselectedDatabase, unselectedRepo, unselectedProvider.toUri(),
    legacyStoreReference), 0);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    unselectedDatabase, unselectedRepo, unselectedProvider.toUri(),
    legacyRangeReference), 0);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    unselectedDatabase, unselectedRepo, unselectedProvider.toUri(),
    legacyCommitReference), 0);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    unselectedDatabase, unselectedRepo, unselectedProvider.toUri(),
    legacyAbortReference), 0);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    unselectedDatabase, unselectedRepo, unselectedProvider.toUri(),
    legacyManifestReference), 0);
  BOOST_CHECK(!fs::exists(committedPayloadPath(unselectedObjects, legacyStoreReference)));
  BOOST_CHECK(!fs::exists(committedPayloadPath(unselectedObjects, legacyRangeReference)));
  BOOST_CHECK(!fs::exists(committedPayloadPath(unselectedObjects, legacyCommitReference)));
  BOOST_CHECK(!fs::exists(committedPayloadPath(unselectedObjects, legacyAbortReference)));
  const auto stagingDirectory = unselectedObjects / "staging";
  if (fs::exists(stagingDirectory)) {
    BOOST_CHECK(fs::is_empty(stagingDirectory));
  }
  const auto acceptedResponse = dispatchRepoRequest(
    environment.provider(0), environment.user().getName(), storeService,
    intentWire, "recovery-publish-intent");
  BOOST_REQUIRE_MESSAGE(acceptedResponse.getStatus(), acceptedResponse.getErrorInfo());
  const auto acceptedPayload = responseBytes(acceptedResponse);
  BOOST_CHECK_EQUAL(std::string(acceptedPayload.begin(), acceptedPayload.end()), operationId);

  const auto seedManifest = makeArtifactManifestForWrite(reference, operationId,
                                                         intent.generation);
  seedNode->core().putRange(seedManifest, {0, bytes.size()}, bytes);
  (void)seedNode->core().commitRanges(seedManifest);
  assertArtifactVisibility(*seedNode, 0, seedManifest, bytes, false,
                           "seed-core-commit-before-receipt");

  repo::ArtifactStoreRequest store;
  store.operationId = operationId;
  store.generation = intent.generation;
  store.reference = reference;
  store.bytes = bytes;
  const auto seedStoreResponse = dispatchRepoRequest(
    environment.provider(0), environment.user().getName(), storeService,
    store.toWire(artifactLimits), "recovery-seed-store");
  BOOST_REQUIRE_MESSAGE(seedStoreResponse.getStatus(), seedStoreResponse.getErrorInfo());
  const auto seedReceiptWire = responseBytes(seedStoreResponse);
  const auto seedReceipt = repo::RepoArtifactCommitReceipt::fromJson(
    std::string(seedReceiptWire.begin(), seedReceiptWire.end()), artifactLimits);
  BOOST_CHECK_EQUAL(seedReceipt.operationId, operationId);
  assertArtifactVisibility(*seedNode, 0, seedManifest, bytes, true,
                           "seed-receipt-committed");

  {
    repo::SqliteArtifactMetadataStore seedCatalog(seedDatabase.string(), true, 15);
    const auto accepted = seedCatalog.lookupPublishOperation(
      operationId, submittingPrincipal);
    BOOST_REQUIRE(accepted);
    BOOST_CHECK(accepted->status.state == "ACCEPTED" ||
                accepted->status.state == "IN_PROGRESS");
    const auto pendingReplica = std::find_if(
      accepted->status.targets.begin(), accepted->status.targets.end(), [&replica] (const auto& item) {
        return item.target == replica;
      });
    BOOST_REQUIRE(pendingReplica != accepted->status.targets.end());
    BOOST_CHECK(pendingReplica->state == "PENDING" || pendingReplica->state == "QUEUED");
    auto interruptedReplica = *pendingReplica;
    interruptedReplica.state = "IN_FLIGHT";
    interruptedReplica.attemptCount = 1;
    const auto interruptedAt = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    seedCatalog.updatePublishOperationTarget(
      operationId, interruptedReplica, interruptedAt);
    const auto committedSeed = std::find_if(
      accepted->status.targets.begin(), accepted->status.targets.end(), [&seed] (const auto& item) {
        return item.target == seed;
      });
    BOOST_REQUIRE(committedSeed != accepted->status.targets.end());
    BOOST_REQUIRE(committedSeed->receipt);
    BOOST_CHECK_EQUAL(committedSeed->receipt->receiptId, seedReceipt.receiptId);
  }

  store.operationOwner = seed;
  const auto unselectedStoreResponse = runWithFacePump(environment, [&] {
    return dispatchRepoRequest(
      environment.provider(2), seedProvider, storeService,
      store.toWire(artifactLimits), "unselected-matching-owner-store");
  });
  const auto checkUnselectedOwnerDenied = [] (const auto& response) {
    BOOST_CHECK_MESSAGE(!response.getStatus(), response.getErrorInfo());
    BOOST_CHECK_MESSAGE(
      response.getErrorInfo().find("repo-publish-operation-unauthorized: target is not an attempted selected Repo") !=
          std::string::npos ||
      response.getErrorInfo().find("repo-publish-replica-owner-unverified: seed did not select and attempt this exact target") !=
          std::string::npos,
      response.getErrorInfo());
  };
  checkUnselectedOwnerDenied(unselectedStoreResponse);
  repo::ArtifactRangeWriteRequest unselectedRange;
  unselectedRange.operationId = operationId;
  unselectedRange.generation = intent.generation;
  unselectedRange.reference = reference;
  unselectedRange.operationOwner = seed;
  unselectedRange.lengthBytes = bytes.size();
  unselectedRange.bytes = bytes;
  const auto unselectedRangeResponse = runWithFacePump(environment, [&] {
    return dispatchRepoRequest(
      environment.provider(2), seedProvider, storeRangeService,
      unselectedRange.toWire(artifactLimits), "unselected-matching-owner-range");
  });
  checkUnselectedOwnerDenied(unselectedRangeResponse);
  repo::ArtifactRangeCommitRequest unselectedCommit;
  unselectedCommit.operationId = operationId;
  unselectedCommit.generation = intent.generation;
  unselectedCommit.reference = reference;
  unselectedCommit.operationOwner = seed;
  const auto unselectedCommitResponse = runWithFacePump(environment, [&] {
    return dispatchRepoRequest(
      environment.provider(2), seedProvider, commitRangesService,
      unselectedCommit.toWire(artifactLimits), "unselected-matching-owner-commit");
  });
  checkUnselectedOwnerDenied(unselectedCommitResponse);
  BOOST_CHECK(!fs::exists(committedPayloadPath(unselectedObjects, reference)));
  BOOST_CHECK_EQUAL(repoManifestCount(unselectedObjects), 0U);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    unselectedDatabase, unselectedRepo, unselectedProvider.toUri(), reference), 0);
  {
    repo::SqliteArtifactMetadataStore catalog(unselectedDatabase.string(), false, 15);
    BOOST_CHECK(!catalog.lookupPersistentLocation(
      reference, unselectedRepo, unselectedProvider.toUri(), 0));
  }
  const auto replicaManifest = makeArtifactManifestForWrite(reference, operationId,
                                                            intent.generation);
  replicaNode->core().putRange(replicaManifest, {0, bytes.size()}, bytes);
  (void)replicaNode->core().commitRanges(replicaManifest);
  assertArtifactVisibility(*replicaNode, 1, replicaManifest, bytes, false,
                           "replica-core-commit-before-receipt");
  const auto replicaStoreResponse = runWithFacePump(environment, [&] {
    return dispatchRepoRequest(
      environment.provider(1), seedProvider, storeService,
      store.toWire(artifactLimits), "recovery-replica-commit-before-owner-restart");
  });
  BOOST_REQUIRE_MESSAGE(replicaStoreResponse.getStatus(),
                        replicaStoreResponse.getErrorInfo());
  const auto replicaReceiptPayload = responseBytes(replicaStoreResponse);
  const auto replicaReceipt = repo::RepoArtifactCommitReceipt::fromJson(
    std::string(replicaReceiptPayload.begin(), replicaReceiptPayload.end()),
    artifactLimits);
  BOOST_CHECK_EQUAL(replicaReceipt.operationId, operationId);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    replicaDatabase, replicaRepo, replicaProvider.toUri(), reference), 1);
  BOOST_CHECK_EQUAL(currentLocationVersion(
    replicaDatabase, reference, replicaRepo, replicaProvider.toUri()), 1U);
  assertArtifactVisibility(*replicaNode, 1, replicaManifest, bytes, true,
                           "replica-receipt-committed");

  // A proof cached while the selected target is IN_FLIGHT must not authorize
  // a durable receipt after the owner records that target as UNKNOWN.
  const std::vector<uint8_t> lateCommitBytes{0x6c, 0x61, 0x74, 0x65};
  const auto lateCommitReference = makeReference("stale-owner-proof", lateCommitBytes);
  auto lateCommitIntent = intent;
  lateCommitIntent.idempotencyKeyDigest = repo::digestPublishIdempotencyKey(
    "spec192-stale-owner-proof-key");
  lateCommitIntent.reference = lateCommitReference;
  // Keep a third queued target so the fixture can durably set UNKNOWN while
  // the operation remains mutable. The seed receipt meets ONE consistency if
  // the recovery worker later reopens this isolated setup operation.
  lateCommitIntent.consistency = "ONE";
  lateCommitIntent.targets = {seed, replica, unselected};
  lateCommitIntent.requestDigest = repo::makePublishRequestDigest(
    lateCommitReference, lateCommitIntent.generation, seed,
    lateCommitIntent.consistency, lateCommitIntent.targets);
  const auto lateCommitOperationId = repo::makePublishOperationId(
    submittingPrincipal, seed, lateCommitIntent.idempotencyKeyDigest);
  const auto lateCommitIntentJson = lateCommitIntent.toJson();
  const std::vector<uint8_t> lateCommitIntentWire(
    lateCommitIntentJson.begin(), lateCommitIntentJson.end());
  const auto lateCommitIntentResponse = dispatchRepoRequest(
    environment.provider(0), environment.user().getName(), storeService,
    lateCommitIntentWire,
    "recovery-stale-owner-proof-intent");
  BOOST_REQUIRE_MESSAGE(lateCommitIntentResponse.getStatus(),
                        lateCommitIntentResponse.getErrorInfo());
  const auto lateCommitAcceptedPayload = responseBytes(lateCommitIntentResponse);
  BOOST_CHECK_EQUAL(std::string(lateCommitAcceptedPayload.begin(),
                                lateCommitAcceptedPayload.end()),
                    lateCommitOperationId);
  repo::ArtifactStoreRequest lateCommitSeedStore;
  lateCommitSeedStore.operationId = lateCommitOperationId;
  lateCommitSeedStore.generation = lateCommitIntent.generation;
  lateCommitSeedStore.reference = lateCommitReference;
  lateCommitSeedStore.bytes = lateCommitBytes;
  const auto lateCommitSeedStoreResponse = dispatchRepoRequest(
    environment.provider(0), environment.user().getName(), storeService,
    lateCommitSeedStore.toWire(artifactLimits), "recovery-stale-owner-proof-seed-store");
  BOOST_REQUIRE_MESSAGE(lateCommitSeedStoreResponse.getStatus(),
                        lateCommitSeedStoreResponse.getErrorInfo());
  repo::PublishOperationTargetStatus lateCommitInFlight;
  {
    repo::SqliteArtifactMetadataStore seedCatalog(seedDatabase.string(), true, 15);
    const auto accepted = seedCatalog.lookupPublishOperation(
      lateCommitOperationId, submittingPrincipal);
    BOOST_REQUIRE(accepted);
    const auto target = std::find_if(
      accepted->status.targets.begin(), accepted->status.targets.end(), [&replica] (const auto& item) {
        return item.target == replica;
      });
    BOOST_REQUIRE(target != accepted->status.targets.end());
    lateCommitInFlight = *target;
    lateCommitInFlight.state = "IN_FLIGHT";
    lateCommitInFlight.attemptCount = 1;
    lateCommitInFlight.failureCode.clear();
    lateCommitInFlight.updatedAtMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    seedCatalog.updatePublishOperationTarget(
      lateCommitOperationId, lateCommitInFlight, lateCommitInFlight.updatedAtMs);
  }
  repo::ArtifactRangeWriteRequest lateCommitRange;
  lateCommitRange.operationId = lateCommitOperationId;
  lateCommitRange.generation = lateCommitIntent.generation;
  lateCommitRange.reference = lateCommitReference;
  lateCommitRange.operationOwner = seed;
  lateCommitRange.offsetBytes = 0;
  lateCommitRange.lengthBytes = lateCommitBytes.size();
  lateCommitRange.bytes = lateCommitBytes;
  const auto stagedLateCommitRange = runWithFacePump(environment, [&] {
    return dispatchRepoRequest(
      environment.provider(1), seedProvider, storeRangeService,
      lateCommitRange.toWire(artifactLimits), "recovery-stale-owner-proof-range");
  });
  BOOST_REQUIRE_MESSAGE(stagedLateCommitRange.getStatus(),
                        stagedLateCommitRange.getErrorInfo());
  lateCommitInFlight.state = "UNKNOWN";
  lateCommitInFlight.failureCode = "repo-target-outcome-unknown";
  lateCommitInFlight.updatedAtMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  {
    repo::SqliteArtifactMetadataStore seedCatalog(seedDatabase.string(), true, 15);
    seedCatalog.updatePublishOperationTarget(
      lateCommitOperationId, lateCommitInFlight, lateCommitInFlight.updatedAtMs);
  }
  repo::ArtifactRangeCommitRequest lateCommit;
  lateCommit.operationId = lateCommitOperationId;
  lateCommit.generation = lateCommitIntent.generation;
  lateCommit.reference = lateCommitReference;
  lateCommit.operationOwner = seed;
  const auto lateCommitResponse = runWithFacePump(environment, [&] {
    return dispatchRepoRequest(
      environment.provider(1), seedProvider, commitRangesService,
      lateCommit.toWire(artifactLimits), "recovery-stale-owner-proof-commit");
  });
  BOOST_CHECK_MESSAGE(!lateCommitResponse.getStatus(), lateCommitResponse.getErrorInfo());
  BOOST_CHECK_MESSAGE(
    lateCommitResponse.getErrorInfo().find(
      "repo-publish-replica-owner-unverified: seed did not select and attempt this exact target") !=
        std::string::npos,
    lateCommitResponse.getErrorInfo());
  const auto lateCommitManifest = makeArtifactManifestForWrite(
    lateCommitReference, lateCommitOperationId, lateCommitIntent.generation);
  BOOST_CHECK(replicaNode->core().has(lateCommitManifest.objectName));
  BOOST_CHECK(fs::exists(committedPayloadPath(replicaObjects, lateCommitReference)));
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    replicaDatabase, replicaRepo, replicaProvider.toUri(), lateCommitReference), 0);
  assertArtifactVisibility(*replicaNode, 1, lateCommitManifest, lateCommitBytes,
                           false, "replica-stale-owner-proof-denied");
  releaseRepoNodeServiceHandlersForRestart(environment.provider(0), servicePrefix);
  const auto seedStopped = runWithFacePump(environment, [&] {
    seedNode.reset();
    return true;
  });
  BOOST_REQUIRE_MESSAGE(seedStopped,
                        "seed RepoNode worker did not stop during Face pumping");

  BOOST_TEST_MESSAGE("Spec192 recovery: constructing replacement seed RepoNode");
  auto recoveredSeedNode = std::make_shared<repo::RepoNode>(
    servicePrefix, seedCapability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(
      seedObjects.string(), 1024 * 1024, 4096, "spec192-recovery-seed"),
    seedRepo, seedProvider.toUri(), seedDatabase.string(), artifactLimits);
  BOOST_TEST_MESSAGE("Spec192 recovery: registering replacement seed RepoNode");
  recoveredSeedNode->registerServices(
    environment.provider(0), publisherUsers[0], 1500, 8000);
  BOOST_TEST_MESSAGE("Spec192 recovery: replacement seed RepoNode registered");

  repo::PersistentRepoClientConfig clientConfig;
  clientConfig.servicePrefix = servicePrefix;
  clientConfig.ackCollectionTimeMs = 1000;
  clientConfig.requestTimeoutMs = 20000;
  clientConfig.limits = artifactLimits;
  auto persistent = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), clientConfig);
  // Isolate the wider TSan ACK window to recovery and reconciliation status
  // checks. The bounded timeout scenario keeps the fixture's original 1 s API.
  auto recoveryStatusConfig = clientConfig;
  recoveryStatusConfig.ackCollectionTimeMs = 3000;
  auto recoveryStatusApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), recoveryStatusConfig);
  repo::PublishOperationHandle handle;
  handle.operationId = operationId;
  handle.seed = seed;
  BOOST_TEST_MESSAGE("Spec192 recovery: querying terminal publish status");
  const auto recovered = [&] {
    try {
      return waitForTerminalPublish(environment, recoveryStatusApi, handle);
    }
    catch (...) {
      // Stop the replacement seed while continuing to service pending Face
      // callbacks, so a failed status request remains visible in the test
      // report instead of hanging in RepoNode worker destruction.
      try {
        releaseRepoNodeServiceHandlersForRestart(environment.provider(0), servicePrefix);
        (void)runWithFacePump(environment, [&] {
          recoveredSeedNode.reset();
          return true;
        });
      }
      catch (...) {
        BOOST_ERROR("replacement seed cleanup failed while pumping Faces");
      }
      throw;
    }
  }();
  BOOST_TEST_MESSAGE("Spec192 recovery: terminal publish status returned " <<
                     recovered.state);
  BOOST_CHECK_EQUAL(recovered.state, "COMMITTED");
  BOOST_REQUIRE(!recovered.receiptId.empty());
  const auto recoveredSeed = std::find_if(
    recovered.targets.begin(), recovered.targets.end(), [&seed] (const auto& item) {
      return item.target == seed;
    });
  const auto recoveredReplica = std::find_if(
    recovered.targets.begin(), recovered.targets.end(), [&replica] (const auto& item) {
      return item.target == replica;
    });
  BOOST_REQUIRE(recoveredSeed != recovered.targets.end());
  BOOST_REQUIRE(recoveredSeed->receipt);
  BOOST_CHECK_EQUAL(recoveredSeed->receipt->receiptId, seedReceipt.receiptId);
  BOOST_REQUIRE(recoveredReplica != recovered.targets.end());
  BOOST_CHECK_EQUAL(recoveredReplica->state, "COMMITTED");
  BOOST_REQUIRE(recoveredReplica->receipt);
  BOOST_CHECK_EQUAL(recoveredReplica->receipt->operationId, operationId);
  BOOST_CHECK_EQUAL(recoveredReplica->receipt->receiptId, replicaReceipt.receiptId);
  BOOST_CHECK_EQUAL(recoveredReplica->attemptCount, 2U);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    seedDatabase, seedRepo, seedProvider.toUri(), reference), 1);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    replicaDatabase, replicaRepo, replicaProvider.toUri(), reference), 1);
  BOOST_CHECK_EQUAL(currentLocationVersion(
    seedDatabase, reference, seedRepo, seedProvider.toUri()), 1U);
  BOOST_CHECK_EQUAL(currentLocationVersion(
    replicaDatabase, reference, replicaRepo, replicaProvider.toUri()), 1U);

  // Count 2 is durable before its STORE can be emitted. A later owner restart
  // must replay that same operation ID once in the new worker pass; the target
  // commits one local receipt and the owner records that exact result.
  releaseRepoNodeServiceHandlersForRestart(environment.provider(0), servicePrefix);
  recoveredSeedNode.reset();
  const std::vector<uint8_t> exhaustedReplayBytes{0x72, 0x65, 0x70};
  const auto exhaustedReplayReference = makeReference(
    "restart-exhausted-replay", exhaustedReplayBytes);
  const std::string exhaustedReplayKey =
    "spec192-recovery-exhausted-replay-key";
  repo::PublishOperationIntent exhaustedReplayIntent;
  exhaustedReplayIntent.idempotencyKeyDigest =
    repo::digestPublishIdempotencyKey(exhaustedReplayKey);
  exhaustedReplayIntent.reference = exhaustedReplayReference;
  exhaustedReplayIntent.generation = 1;
  exhaustedReplayIntent.seed = seed;
  exhaustedReplayIntent.consistency = "ALL";
  exhaustedReplayIntent.targets = {seed, replica};
  exhaustedReplayIntent.requestDigest = repo::makePublishRequestDigest(
    exhaustedReplayReference, exhaustedReplayIntent.generation, seed,
    exhaustedReplayIntent.consistency, exhaustedReplayIntent.targets);
  const auto exhaustedReplayOperationId = repo::makePublishOperationId(
    submittingPrincipal, seed,
    exhaustedReplayIntent.idempotencyKeyDigest);
  auto exhaustedSeedNode = std::make_shared<repo::RepoNode>(
    servicePrefix, seedCapability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(
      seedObjects.string(), 1024 * 1024, 4096, "spec192-recovery-seed"),
    seedRepo, seedProvider.toUri(), seedDatabase.string(), artifactLimits);
  exhaustedSeedNode->registerServices(environment.provider(0));
  const auto exhaustedIntentJson = exhaustedReplayIntent.toJson();
  const std::vector<uint8_t> exhaustedIntentWire(
    exhaustedIntentJson.begin(), exhaustedIntentJson.end());
  const auto exhaustedIntentResponse = dispatchRepoRequest(
    environment.provider(0), environment.user().getName(), storeService,
    exhaustedIntentWire, "recovery-exhausted-replay-intent");
  BOOST_REQUIRE_MESSAGE(exhaustedIntentResponse.getStatus(),
                        exhaustedIntentResponse.getErrorInfo());
  repo::ArtifactStoreRequest exhaustedSeedStore;
  exhaustedSeedStore.operationId = exhaustedReplayOperationId;
  exhaustedSeedStore.generation = exhaustedReplayIntent.generation;
  exhaustedSeedStore.reference = exhaustedReplayReference;
  exhaustedSeedStore.bytes = exhaustedReplayBytes;
  const auto exhaustedSeedStoreResponse = dispatchRepoRequest(
    environment.provider(0), environment.user().getName(), storeService,
    exhaustedSeedStore.toWire(artifactLimits),
    "recovery-exhausted-replay-seed-store");
  BOOST_REQUIRE_MESSAGE(exhaustedSeedStoreResponse.getStatus(),
                        exhaustedSeedStoreResponse.getErrorInfo());
  {
    repo::SqliteArtifactMetadataStore seedCatalog(seedDatabase.string(), true, 15);
    const auto accepted = seedCatalog.lookupPublishOperation(
      exhaustedReplayOperationId, submittingPrincipal);
    BOOST_REQUIRE(accepted);
    const auto pending = std::find_if(
      accepted->status.targets.begin(), accepted->status.targets.end(),
      [&replica] (const auto& item) { return item.target == replica; });
    BOOST_REQUIRE(pending != accepted->status.targets.end());
    auto exhausted = *pending;
    exhausted.state = "IN_FLIGHT";
    exhausted.attemptCount = 2;
    const auto exhaustedAt = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    seedCatalog.updatePublishOperationTarget(
      exhaustedReplayOperationId, exhausted, exhaustedAt);
  }
  releaseRepoNodeServiceHandlersForRestart(environment.provider(0), servicePrefix);
  exhaustedSeedNode.reset();
  auto exhaustedReplayStoreCalls = std::make_shared<std::atomic<unsigned>>(0);
  ndn_service_framework::ServiceProvider::AckStrategyHandler exhaustedReplayAck =
    [] (const ndn_service_framework::RequestMessage&) {
      ndn_service_framework::ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.message = "repo-ready";
      return decision;
    };
  // Route the injected request counter through Provider 1 to the selected
  // replica's registered production handler on Provider 2. This preserves the
  // selected Provider identity while keeping the test out of private methods.
  replicaNode->registerServices(environment.provider(2));
  ndn_service_framework::ServiceProvider::RequestHandler exhaustedReplayStore =
    [&environment, exhaustedReplayStoreCalls, replicaProvider, storeService] (
      const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&,
      const ndn::Name& requestId,
      const ndn_service_framework::RequestMessage& request) {
        exhaustedReplayStoreCalls->fetch_add(1);
        return environment.provider(2).dispatchRequest(
          requesterIdentity, replicaProvider, storeService, requestId, request);
      };
  environment.provider(1).addService(
    storeService, exhaustedReplayAck, std::move(exhaustedReplayStore));
  recoveredSeedNode = std::make_shared<repo::RepoNode>(
    servicePrefix, seedCapability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(
      seedObjects.string(), 1024 * 1024, 4096, "spec192-recovery-seed"),
    seedRepo, seedProvider.toUri(), seedDatabase.string(), artifactLimits);
  recoveredSeedNode->registerServices(
    environment.provider(0), publisherUsers[0], 1500, 8000);
  repo::PublishOperationHandle exhaustedReplayHandle;
  exhaustedReplayHandle.operationId = exhaustedReplayOperationId;
  exhaustedReplayHandle.seed = seed;
  const auto exhaustedReplayStatus = waitForTerminalPublish(
    environment, recoveryStatusApi, exhaustedReplayHandle);
  BOOST_CHECK_EQUAL(exhaustedReplayStatus.state, "COMMITTED");
  const auto exhaustedTarget = std::find_if(
    exhaustedReplayStatus.targets.begin(), exhaustedReplayStatus.targets.end(),
    [&replica] (const auto& item) { return item.target == replica; });
  BOOST_REQUIRE(exhaustedTarget != exhaustedReplayStatus.targets.end());
  BOOST_CHECK_EQUAL(exhaustedTarget->state, "COMMITTED");
  BOOST_CHECK_EQUAL(exhaustedTarget->attemptCount, 3U);
  BOOST_REQUIRE(exhaustedTarget->receipt);
  BOOST_CHECK_EQUAL(exhaustedTarget->receipt->operationId,
                    exhaustedReplayOperationId);
  BOOST_CHECK_EQUAL(exhaustedReplayStoreCalls->load(), 1U);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    replicaDatabase, replicaRepo, replicaProvider.toUri(),
    exhaustedReplayReference), 1);

  releaseRepoNodeServiceHandlersForRestart(environment.provider(0), servicePrefix);
  recoveredSeedNode.reset();
  auto recoverySeedNode = std::make_shared<repo::RepoNode>(
    servicePrefix, seedCapability,
    std::make_shared<repo::FilesystemRepoStoreBackend>(
      seedObjects.string(), 1024 * 1024, 4096, "spec192-recovery-seed"),
    seedRepo, seedProvider.toUri(), seedDatabase.string(), artifactLimits);
  recoverySeedNode->registerServices(
    environment.provider(0), publisherUsers[0], 1500, 8000);

  // Provider 2's registered replica handlers remain available to the injected
  // response-fault wrapper below.

  auto delayedStoreCalls = std::make_shared<std::atomic<unsigned>>(0);
  ndn_service_framework::ServiceProvider::RequestHandler delayedStore =
    [delayedStoreCalls] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
                         const ndn::Name&,
                         const ndn_service_framework::RequestMessage&) {
      delayedStoreCalls->fetch_add(1);
      std::this_thread::sleep_for(2s);
      ndn_service_framework::ResponseMessage response;
      response.setStatus(false);
      response.setErrorInfo("spec192-injected-late-store-response");
      return response;
    };
  ndn_service_framework::ServiceProvider::AckStrategyHandler readyAck =
    [] (const ndn_service_framework::RequestMessage&) {
      ndn_service_framework::ServiceProvider::AckDecision decision;
      decision.status = true;
      decision.message = "repo-ready";
      return decision;
    };
  auto commitThenErrorCalls = std::make_shared<std::atomic<unsigned>>(0);
  ndn_service_framework::ServiceProvider::RequestHandler commitThenError =
    [&environment, commitThenErrorCalls, replicaProvider, storeService] (
      const ndn::Name& requesterIdentity, const ndn::Name&, const ndn::Name&,
      const ndn::Name& requestId,
      const ndn_service_framework::RequestMessage& request) {
        const auto committed = environment.provider(2).dispatchRequest(
          requesterIdentity, replicaProvider, storeService, requestId, request);
        if (!committed.getStatus()) {
          return committed;
        }
        ndn_service_framework::ResponseMessage response;
        if (commitThenErrorCalls->fetch_add(1) == 0) {
          response.setStatus(false);
          response.setErrorInfo("spec192-committed-store-error-response");
        }
        else {
          response.setStatus(true);
          const auto committedBytes = responseBytes(committed);
          ndn::Buffer responsePayload;
          if (!committedBytes.empty()) {
            responsePayload = ndn::Buffer(
              committedBytes.data(), committedBytes.size());
          }
          response.setPayload(responsePayload, responsePayload.size());
        }
        return response;
      };
  environment.provider(1).addService(
    storeService, readyAck, std::move(commitThenError));

  const std::vector<uint8_t> reconciliationBytes{0x72, 0x65, 0x63};
  const auto reconciliationReference = makeReference(
    "target-reconciliation", reconciliationBytes);
  const auto reconciliationSource = temporary.path() / "target-reconciliation.bin";
  writeFile(reconciliationSource, reconciliationBytes);
  repo::SelectedRepoSet reconciliationPlacement;
  reconciliationPlacement.targets = {
    {seedRepo, seedProvider, servicePrefix},
    {replicaRepo, replicaProvider, servicePrefix},
  };
  reconciliationPlacement.seedRepoId = seedRepo;
  reconciliationPlacement.consistency = repo::RepoWriteConsistency::All;
  const auto reconciliationHandle = runWithFacePump(environment, [&] {
    return persistent.beginPublishFile(
      reconciliationSource.string(), reconciliationReference,
      reconciliationPlacement, "spec192-committed-store-error-key", 120000);
  });
  const auto reconciled = waitForTerminalPublish(
    environment, recoveryStatusApi, reconciliationHandle);
  BOOST_CHECK_EQUAL(reconciled.state, "COMMITTED");
  const auto reconciledTarget = std::find_if(
    reconciled.targets.begin(), reconciled.targets.end(), [&replica] (const auto& item) {
      return item.target == replica;
    });
  BOOST_REQUIRE(reconciledTarget != reconciled.targets.end());
  BOOST_CHECK_MESSAGE(
    reconciledTarget->state == "COMMITTED",
    "reconciliation target state=" << reconciledTarget->state
      << ", failureCode=" << reconciledTarget->failureCode
      << ", attempts=" << reconciledTarget->attemptCount
      << ", STORE calls=" << commitThenErrorCalls->load());
  BOOST_REQUIRE(reconciledTarget->receipt);
  BOOST_CHECK_EQUAL(reconciledTarget->attemptCount, 2U);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    replicaDatabase, replicaRepo, replicaProvider.toUri(),
    reconciliationReference), 1);
  BOOST_CHECK_EQUAL(currentLocationVersion(
    replicaDatabase, reconciliationReference, replicaRepo,
    replicaProvider.toUri()), 1U);
  BOOST_CHECK_EQUAL(commitThenErrorCalls->load(), 2U);
  BOOST_CHECK(fs::exists(committedPayloadPath(
    replicaObjects, reconciliationReference)));

  environment.provider(1).addService(
    storeService, std::move(readyAck),
    std::move(delayedStore));

  // Keep this within maxChunkBytes so the selected request uses STORE, the
  // exact operation covered by delayedStore above.
  const std::vector<uint8_t> timeoutBytes{0x74, 0x69, 0x6d};
  const auto timeoutReference = makeReference("target-timeout", timeoutBytes);
  const auto timeoutSource = temporary.path() / "target-timeout.bin";
  writeFile(timeoutSource, timeoutBytes);
  repo::SelectedRepoSet timeoutPlacement;
  timeoutPlacement.targets = {
    {seedRepo, seedProvider, servicePrefix},
    {replicaRepo, replicaProvider, servicePrefix},
  };
  timeoutPlacement.seedRepoId = seedRepo;
  timeoutPlacement.consistency = repo::RepoWriteConsistency::All;
  const auto timeoutStart = std::chrono::steady_clock::now();
  const auto timeoutResult = runWithFacePump(environment, [&] {
    return persistent.publishFile(
      timeoutSource.string(), timeoutReference, timeoutPlacement, 500);
  });
  BOOST_REQUIRE(timeoutResult.operationHandle);
  BOOST_REQUIRE(timeoutResult.operationStatus);
  BOOST_CHECK(timeoutResult.operationStatus->state == "ACCEPTED" ||
              timeoutResult.operationStatus->state == "IN_PROGRESS");
  BOOST_CHECK(!timeoutResult.handle);
  BOOST_CHECK(!timeoutResult.receipt);
  const auto timeoutHandle = *timeoutResult.operationHandle;
  const auto reconnectedTimeoutStatus = runWithFacePump(environment, [&] {
    return persistent.getPublishStatus(timeoutHandle, 8000);
  });
  BOOST_CHECK_EQUAL(reconnectedTimeoutStatus.operationId,
                    timeoutHandle.operationId);
  const auto timeoutStatus = waitForTerminalPublish(
    environment, persistent, timeoutHandle);
  BOOST_CHECK(std::chrono::steady_clock::now() - timeoutStart < 20s);
  BOOST_CHECK_EQUAL(timeoutStatus.state, "PARTIAL");
  const auto unknownTarget = std::find_if(
    timeoutStatus.targets.begin(), timeoutStatus.targets.end(), [&replica] (const auto& item) {
      return item.target.repoNode == replica.repoNode &&
             item.target.providerIdentity == replica.providerIdentity;
    });
  BOOST_REQUIRE(unknownTarget != timeoutStatus.targets.end());
  BOOST_CHECK_EQUAL(unknownTarget->state, "UNKNOWN");
  BOOST_CHECK_EQUAL(unknownTarget->failureCode, "repo-target-outcome-unknown");
  BOOST_CHECK_EQUAL(unknownTarget->attemptCount, 2U);
  BOOST_CHECK(!unknownTarget->receipt);
  BOOST_CHECK_EQUAL(delayedStoreCalls->load(), 2U);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    seedDatabase, seedRepo, seedProvider.toUri(), timeoutReference), 1);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    replicaDatabase, replicaRepo, replicaProvider.toUri(), timeoutReference), 0);
  BOOST_CHECK(!fs::exists(committedPayloadPath(replicaObjects, timeoutReference)));

  const auto lateResponseDrainDeadline = std::chrono::steady_clock::now() + 2s;
  environment.pumpUntilWithAttributeAuthority([&] {
    return std::chrono::steady_clock::now() >= lateResponseDrainDeadline;
  });
  replicaNode->registerServices(environment.provider(1), publisherUsers[1], 1500, 8000);
}

BOOST_AUTO_TEST_CASE(PersistentApiSelectsProvidersAndCountsOnlyDurableReceipts)
{
  TempRoot temporary;
  const ndn::Name servicePrefix(repo::RepoClient::DEFAULT_SERVICE_NAME);
  repo::ArtifactLimits artifactLimits;
  artifactLimits.maxChunkBytes = 4;
  BootstrapProfile profile;
  profile.groupPrefix = "/ndnsf/spec192/persistent/group";
  profile.syncPrefix = "/ndnsf/spec192/persistent/sync";
  profile.userNode = "/ndnsf/spec192/persistent/user";
  profile.providerNode = "/ndnsf/spec192/persistent/provider";
  profile.userIdentity = "/test/user/spec192/persistent";
  profile.providerIdentity = "/test/provider/spec192/persistent";
  profile.attributeAuthority = "/test/aa/spec192/persistent";
  profile.providerCount = 3;
  profile.providerCanActAsUser = true;
  const std::vector<std::string> operations{
    "CAPABILITY", "STATUS", "STORE", "STORE_RANGE", "COMMIT_RANGES",
    "ABORT_RANGES", "FETCH", "FETCH_RANGE",
  };
  profile.serviceName = repoService(servicePrefix, operations.front());
  for (size_t index = 1; index < operations.size(); ++index) {
    profile.additionalServices.push_back(repoService(servicePrefix, operations[index]));
  }

  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);

  std::vector<std::shared_ptr<ndn_service_framework::ServiceUser>> publisherUsers;
  publisherUsers.reserve(environment.providerCount());
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    publisherUsers.push_back(
      makeRepoPublisherUser(environment, index, servicePrefix, operations));
  }

  std::vector<std::shared_ptr<repo::RepoNode>> nodes;
  std::map<std::string, fs::path> repoRoots;
  std::map<std::string, fs::path> metadataDatabases;
  std::map<std::string, size_t> expectedManifestCounts;
  std::set<std::string> expectedProviders;
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    const std::string logicalRepo = "/logical/spec192/repo-" + std::to_string(index);
    const auto providerIdentity = environment.provider(index).getName().toUri();
    expectedProviders.insert(providerIdentity);
    const auto repoRoot = temporary.path() / ("repo-" + std::to_string(index));
    const auto objectRoot = repoRoot / "objects";
    const auto metadataPath = repoRoot / "artifact-v2.sqlite";
    repo::StorageCapability capability;
    capability.repoNode = logicalRepo;
    capability.freeBytes = 8 * 1024 * 1024;
    capability.failureDomain = "spec192-domain-" + std::to_string(index);
    capability.storageClasses = {"ssd"};
    auto store = std::make_shared<repo::FilesystemRepoStoreBackend>(
      objectRoot.string(), 1024 * 1024, 4096,
      "spec192-persistent-repo-" + std::to_string(index));
    auto node = std::make_shared<repo::RepoNode>(
      servicePrefix, capability, std::move(store), logicalRepo,
      providerIdentity, metadataPath.string(), artifactLimits);
    node->registerServices(environment.provider(index), publisherUsers[index],
                           1000, 8000);
    nodes.push_back(std::move(node));
    repoRoots.emplace(logicalRepo, objectRoot);
    metadataDatabases.emplace(logicalRepo, metadataPath);
    expectedManifestCounts.emplace(logicalRepo, 0);
  }
  environment.enableProductionIngressForTest();
  environment.pumpUntilWithAttributeAuthority([&] {
    return std::all_of(publisherUsers.begin(), publisherUsers.end(), [] (const auto& user) {
      return user->isNacConsumerReadyForTest() && user->isNacProducerReadyForTest();
    });
  });

  repo::PersistentRepoClientConfig clientConfig;
  clientConfig.servicePrefix = servicePrefix;
  clientConfig.ackCollectionTimeMs = 1000;
  clientConfig.requestTimeoutMs = 8000;
  clientConfig.limits = artifactLimits;
  auto persistent = repo::ArtifactRepositoryApi::makePersistent(environment.user(), clientConfig);

  // Give signed discovery and selected-write ACK collection their own windows;
  // availability probes keep their existing shorter timeouts.
  auto discoveryConfig = clientConfig;
  discoveryConfig.ackCollectionTimeMs = 5000;
  discoveryConfig.requestTimeoutMs = 20000;
  auto discoveryApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), discoveryConfig);
  auto selectedWriteConfig = clientConfig;
  selectedWriteConfig.ackCollectionTimeMs = 5000;
  selectedWriteConfig.requestTimeoutMs = 20000;
  auto selectedWriteApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), selectedWriteConfig);

  const std::vector<uint8_t> discoveryBytes{0x10, 0x11, 0x12, 0x13, 0x14};
  repo::ArtifactCapabilityRequirements requirements;
  requirements.artifact = makeReference("discovery-probe", discoveryBytes);
  requirements.rootSignatureAlgorithm = "rsa-sha256";
  requirements.chunkBytes = 4;
  requirements.rootEncodedBytes = 1;
  requirements.pageEncodedBytes = 1;
  requirements.pageEntries = 1;
  requirements.manifestDepth = 1;
  requirements.requireResume = true;
  requirements.requireReplicaReceipts = true;
  const auto discovered = runWithFacePump(environment, [&] {
    return discoveryApi.discoverRepoCandidates(requirements,
                                               discoveryConfig.requestTimeoutMs);
  });
  BOOST_REQUIRE_EQUAL(discovered.candidates.size(), 3);
  std::set<std::string> discoveredProviders;
  std::set<std::string> discoveredRepos;
  for (const auto& candidate : discovered.candidates) {
    discoveredProviders.insert(candidate.providerIdentity.toUri());
    discoveredRepos.insert(candidate.repoNode);
    BOOST_CHECK_NE(candidate.repoNode, candidate.providerIdentity.toUri());
    BOOST_CHECK_EQUAL(candidate.servicePrefix.toUri(), servicePrefix.toUri());
    BOOST_CHECK(candidate.capability.supportsReplicaReceipts);
    BOOST_CHECK_EQUAL(repoManifestCount(repoRoots.at(candidate.repoNode)), 0U);
    BOOST_CHECK(!fs::exists(committedPayloadPath(
      repoRoots.at(candidate.repoNode), requirements.artifact)));
    BOOST_CHECK_EQUAL(persistentReceiptRows(
      metadataDatabases.at(candidate.repoNode), candidate.repoNode,
      candidate.providerIdentity.toUri(), requirements.artifact), 0);
  }
  BOOST_CHECK(discoveredProviders == expectedProviders);
  BOOST_CHECK_EQUAL(discoveredRepos.size(), 3);

  repo::SelectedRepoSet selected;
  for (const auto& candidate : discovered.candidates) {
    selected.targets.push_back({candidate.repoNode, candidate.providerIdentity,
                                candidate.servicePrefix});
  }
  std::sort(selected.targets.begin(), selected.targets.end(),
            [] (const auto& lhs, const auto& rhs) {
              return lhs.repoNode < rhs.repoNode;
            });
  BOOST_REQUIRE_EQUAL(selected.targets.size(), 3);
  selected.seedRepoId = selected.targets.front().repoNode;

  for (const auto consistency : {repo::RepoWriteConsistency::One,
                                 repo::RepoWriteConsistency::Quorum,
                                 repo::RepoWriteConsistency::All}) {
    const auto label = consistencyName(consistency);
    const std::vector<uint8_t> bytes{
      0x53, 0x70, 0x65, 0x63, 0x31, 0x39, 0x32,
      static_cast<uint8_t>(label.size()),
      static_cast<uint8_t>(consistency == repo::RepoWriteConsistency::One ? 1 :
                           consistency == repo::RepoWriteConsistency::Quorum ? 2 : 3),
      0xa1, 0xb2, 0xc3,
    };
    const auto reference = makeReference("direct-" + label, bytes);
    const auto sourcePath = temporary.path() / ("source-" + label + ".bin");
    writeFile(sourcePath, bytes);
    auto placement = selected;
    placement.consistency = consistency;
    const auto published = runWithFacePump(environment, [&] {
      return selectedWriteApi.publishFile(sourcePath.string(), reference,
                                           placement, 60000);
    });

    const auto required = repo::requiredWriteAcks(3, consistency);
    BOOST_REQUIRE(published.receipt);
    BOOST_REQUIRE(published.handle);
    BOOST_CHECK_EQUAL(published.achievedReplicas, required);
    BOOST_CHECK_EQUAL(published.receipt->achievedReplicas, required);
    BOOST_CHECK_EQUAL(published.receipt->requiredAcks, required);
    BOOST_CHECK_EQUAL(published.receipt->state, "COMMITTED");
    BOOST_CHECK_EQUAL(published.receipt->placementState, "SATISFIED");

    uint32_t committed = 0;
    for (const auto& target : placement.targets) {
      const auto replica = std::find_if(
        published.receipt->replicaCommits.begin(),
        published.receipt->replicaCommits.end(),
        [&target] (const auto& item) { return item.repoNode == target.repoNode; });
      BOOST_REQUIRE(replica != published.receipt->replicaCommits.end());
      const auto& payloadPath = committedPayloadPath(repoRoots.at(target.repoNode), reference);
      if (replica->state == "COMMITTED") {
        ++committed;
        ++expectedManifestCounts[target.repoNode];
        BOOST_CHECK_EQUAL(replica->providerIdentity, target.providerIdentity.toUri());
        BOOST_CHECK_EQUAL(persistentReceiptRows(
          metadataDatabases.at(target.repoNode), target.repoNode,
          target.providerIdentity.toUri(), reference), 1);
        BOOST_CHECK(fs::exists(payloadPath));
        BOOST_CHECK(readFile(payloadPath) == bytes);
        BOOST_CHECK_EQUAL(repoManifestCount(repoRoots.at(target.repoNode)),
                          expectedManifestCounts[target.repoNode]);
      }
      else {
        BOOST_CHECK_MESSAGE(
          replica->state == "SKIPPED",
          "target Repo " << target.repoNode << " state=" << replica->state
                         << " failure=" << replica->failureReason);
        BOOST_CHECK_EQUAL(persistentReceiptRows(
          metadataDatabases.at(target.repoNode), target.repoNode,
          target.providerIdentity.toUri(), reference), 0);
        BOOST_CHECK(!fs::exists(payloadPath));
        BOOST_CHECK_EQUAL(repoManifestCount(repoRoots.at(target.repoNode)),
                          expectedManifestCounts[target.repoNode]);
      }
    }
    BOOST_CHECK_EQUAL(committed, required);
  }

  auto oneTarget = selected;
  oneTarget.targets.resize(1);
  oneTarget.seedRepoId = oneTarget.targets.front().repoNode;
  oneTarget.consistency = repo::RepoWriteConsistency::All;
  const std::vector<uint8_t> selectedBytes{0x73, 0x65, 0x6c, 0x65, 0x63, 0x74, 0x65, 0x64};
  const auto selectedReference = makeReference("selected-only", selectedBytes);
  const auto selectedPath = temporary.path() / "selected-only.bin";
  writeFile(selectedPath, selectedBytes);
  const auto selectedResult = runWithFacePump(environment, [&] {
    return selectedWriteApi.publishFile(selectedPath.string(), selectedReference,
                                        oneTarget, 60000);
  });
  BOOST_REQUIRE(selectedResult.receipt);
  BOOST_REQUIRE(selectedResult.handle);
  BOOST_CHECK_EQUAL(selectedResult.requestedReplicas, 1U);
  BOOST_CHECK_EQUAL(selectedResult.achievedReplicas, 1U);
  for (const auto& candidate : discovered.candidates) {
    const bool wasSelected = candidate.repoNode == oneTarget.targets.front().repoNode &&
                             candidate.providerIdentity == oneTarget.targets.front().providerIdentity;
    const auto payloadPath = committedPayloadPath(repoRoots.at(candidate.repoNode), selectedReference);
    const auto rowCount = persistentReceiptRows(
      metadataDatabases.at(candidate.repoNode), candidate.repoNode,
      candidate.providerIdentity.toUri(), selectedReference);
    if (wasSelected) {
      ++expectedManifestCounts[candidate.repoNode];
      BOOST_CHECK_EQUAL(rowCount, 1);
      BOOST_CHECK(readFile(payloadPath) == selectedBytes);
    }
    else {
      BOOST_CHECK_EQUAL(rowCount, 0);
      BOOST_CHECK(!fs::exists(payloadPath));
    }
    BOOST_CHECK_EQUAL(repoManifestCount(repoRoots.at(candidate.repoNode)),
                      expectedManifestCounts[candidate.repoNode]);
  }

  const std::string replayKey = "spec192-selected-replay-key";
  const auto firstHandle = runWithFacePump(environment, [&] {
    return selectedWriteApi.beginPublishFile(
      selectedPath.string(), selectedReference, oneTarget, replayKey, 180000);
  });
  const auto replayedHandle = runWithFacePump(environment, [&] {
    return selectedWriteApi.beginPublishFile(
      selectedPath.string(), selectedReference, oneTarget, replayKey, 180000);
  });
  BOOST_CHECK_EQUAL(replayedHandle.toJson(), firstHandle.toJson());
  const auto firstStatus = waitForTerminalPublish(environment, selectedWriteApi, firstHandle);
  BOOST_CHECK_EQUAL(firstStatus.state, "COMMITTED");
  BOOST_REQUIRE(!firstStatus.receiptId.empty());
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    metadataDatabases.at(oneTarget.targets.front().repoNode),
    oneTarget.targets.front().repoNode,
    oneTarget.targets.front().providerIdentity.toUri(), selectedReference), 2);
  BOOST_CHECK_EQUAL(currentLocationVersion(
    metadataDatabases.at(oneTarget.targets.front().repoNode), selectedReference,
    oneTarget.targets.front().repoNode,
    oneTarget.targets.front().providerIdentity.toUri()), 2U);

  auto changedSelection = oneTarget;
  const auto changedCandidate = std::find_if(
    discovered.candidates.begin(), discovered.candidates.end(),
    [&oneTarget] (const auto& candidate) {
      return candidate.repoNode != oneTarget.targets.front().repoNode;
    });
  BOOST_REQUIRE(changedCandidate != discovered.candidates.end());
  changedSelection.targets.push_back({changedCandidate->repoNode,
                                      changedCandidate->providerIdentity,
                                      changedCandidate->servicePrefix});
  BOOST_CHECK_EXCEPTION(
    runWithFacePump(environment, [&] {
      return selectedWriteApi.beginPublishFile(
        selectedPath.string(), selectedReference, changedSelection,
        replayKey, 180000);
    }),
    std::exception,
    [] (const std::exception& error) {
      return std::string(error.what()).find(
        "repo-publish-idempotency-conflict:") != std::string::npos;
    });

  const auto repairedHandle = runWithFacePump(environment, [&] {
    return selectedWriteApi.beginPublishFile(
      selectedPath.string(), selectedReference, oneTarget,
      "spec192-selected-repair-key", 180000);
  });
  const auto repairedStatus = waitForTerminalPublish(
    environment, selectedWriteApi, repairedHandle);
  BOOST_CHECK_EQUAL(repairedStatus.state, "COMMITTED");
  BOOST_REQUIRE(!repairedStatus.receiptId.empty());
  BOOST_CHECK_NE(repairedHandle.operationId, firstHandle.operationId);
  BOOST_CHECK_NE(repairedStatus.receiptId, firstStatus.receiptId);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    metadataDatabases.at(oneTarget.targets.front().repoNode),
    oneTarget.targets.front().repoNode,
    oneTarget.targets.front().providerIdentity.toUri(), selectedReference), 3);
  BOOST_CHECK_EQUAL(currentLocationVersion(
    metadataDatabases.at(oneTarget.targets.front().repoNode), selectedReference,
    oneTarget.targets.front().repoNode,
    oneTarget.targets.front().providerIdentity.toUri()), 3U);
  const auto historicalStatus = runWithFacePump(environment, [&] {
    return selectedWriteApi.getPublishStatus(firstHandle, 8000);
  });
  BOOST_CHECK_EQUAL(historicalStatus.receiptId, firstStatus.receiptId);
  const auto repairedFetch = temporary.path() / "selected-after-repair.bin";
  runWithFacePump(environment, [&] {
    persistent.fetchFile(*selectedResult.handle, repairedFetch.string(), 60000);
  });
  BOOST_CHECK(readFile(repairedFetch) == selectedBytes);
  const auto repairedAvailability = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(*selectedResult.handle, "metadata", 8000);
  });
  BOOST_REQUIRE_EQUAL(repairedAvailability.locations.size(), 1U);
  const auto repairedTarget = std::find_if(
    repairedStatus.targets.begin(), repairedStatus.targets.end(), [&oneTarget] (const auto& item) {
      return item.target.repoNode == oneTarget.targets.front().repoNode &&
             item.target.providerIdentity == oneTarget.targets.front().providerIdentity.toUri();
    });
  BOOST_REQUIRE(repairedTarget != repairedStatus.targets.end());
  BOOST_REQUIRE(repairedTarget->receipt);
  BOOST_CHECK_EQUAL(repairedAvailability.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(repairedAvailability.locations[0].objectPresence, "PRESENT");
  BOOST_CHECK_EQUAL(repairedAvailability.locations[0].operationId,
                    repairedHandle.operationId);
  BOOST_CHECK_EQUAL(repairedAvailability.locations[0].receiptId,
                    repairedTarget->receipt->receiptId);
  BOOST_CHECK_EQUAL(repairedAvailability.locations[0].locationVersion, 3U);
  BOOST_CHECK_EQUAL(repoManifestCount(
    repoRoots.at(oneTarget.targets.front().repoNode)),
    expectedManifestCounts.at(oneTarget.targets.front().repoNode));

  const auto otherCandidate = std::find_if(
    discovered.candidates.begin(), discovered.candidates.end(),
    [&oneTarget] (const auto& candidate) {
      return candidate.repoNode != oneTarget.targets.front().repoNode;
    });
  BOOST_REQUIRE(otherCandidate != discovered.candidates.end());
  repo::SelectedRepoSet disjointTarget;
  disjointTarget.targets.push_back({otherCandidate->repoNode,
                                    otherCandidate->providerIdentity,
                                    otherCandidate->servicePrefix});
  disjointTarget.seedRepoId = otherCandidate->repoNode;
  disjointTarget.consistency = repo::RepoWriteConsistency::One;
  const auto disjointResult = runWithFacePump(environment, [&] {
    return selectedWriteApi.publishFile(selectedPath.string(), selectedReference,
                                        disjointTarget, 60000);
  });
  BOOST_REQUIRE(disjointResult.receipt);
  BOOST_CHECK_NE(disjointResult.operationId, selectedResult.operationId);
  BOOST_CHECK_NE(disjointResult.receipt->receiptId,
                 selectedResult.receipt->receiptId);
  BOOST_CHECK_EQUAL(disjointResult.requestedReplicas, 1U);
  BOOST_CHECK_EQUAL(disjointResult.achievedReplicas, 1U);
  ++expectedManifestCounts[otherCandidate->repoNode];
  for (const auto& candidate : discovered.candidates) {
    const int expectedRows =
      candidate.repoNode == oneTarget.targets.front().repoNode ? 3 :
      candidate.repoNode == otherCandidate->repoNode ? 1 : 0;
    const auto payloadPath = committedPayloadPath(
      repoRoots.at(candidate.repoNode), selectedReference);
    const auto rowCount = persistentReceiptRows(
      metadataDatabases.at(candidate.repoNode), candidate.repoNode,
      candidate.providerIdentity.toUri(), selectedReference);
    BOOST_CHECK_EQUAL(rowCount, expectedRows);
    BOOST_CHECK_EQUAL(fs::exists(payloadPath), expectedRows != 0);
    if (expectedRows != 0) {
      BOOST_CHECK(readFile(payloadPath) == selectedBytes);
    }
    BOOST_CHECK_EQUAL(repoManifestCount(repoRoots.at(candidate.repoNode)),
                      expectedManifestCounts[candidate.repoNode]);
  }

  repo::SelectedRepoSet quorumTargets;
  for (const auto& candidate : discovered.candidates) {
    quorumTargets.targets.push_back({candidate.repoNode,
                                    candidate.providerIdentity,
                                    candidate.servicePrefix});
  }
  quorumTargets.seedRepoId = quorumTargets.targets.front().repoNode;
  quorumTargets.consistency = repo::RepoWriteConsistency::Quorum;
  auto permutedQuorumTargets = quorumTargets;
  std::reverse(permutedQuorumTargets.targets.begin(),
               permutedQuorumTargets.targets.end());
  const std::vector<uint8_t> quorumBytes{0x71, 0x75, 0x6f, 0x72, 0x75, 0x6d};
  const auto quorumReference = makeReference("quorum-order", quorumBytes);
  const auto quorumPath = temporary.path() / "quorum-order.bin";
  writeFile(quorumPath, quorumBytes);
  const auto quorumResult = runWithFacePump(environment, [&] {
    return selectedWriteApi.publishFile(quorumPath.string(), quorumReference,
                                        quorumTargets, 60000);
  });
  const auto permutedQuorumResult = runWithFacePump(environment, [&] {
    return selectedWriteApi.publishFile(quorumPath.string(), quorumReference,
                                        permutedQuorumTargets, 60000);
  });
  BOOST_REQUIRE(quorumResult.receipt);
  BOOST_REQUIRE(permutedQuorumResult.receipt);
  BOOST_REQUIRE(quorumResult.handle);
  BOOST_REQUIRE(permutedQuorumResult.handle);
  BOOST_CHECK_NE(quorumResult.operationId, permutedQuorumResult.operationId);
  BOOST_CHECK_NE(quorumResult.receipt->receiptId,
                 permutedQuorumResult.receipt->receiptId);
  BOOST_CHECK_NE(quorumResult.handle->toJson(),
                 permutedQuorumResult.handle->toJson());
  BOOST_CHECK_EQUAL(quorumResult.achievedReplicas,
                    quorumResult.receipt->requiredAcks);
  BOOST_CHECK_EQUAL(permutedQuorumResult.achievedReplicas,
                    permutedQuorumResult.receipt->requiredAcks);
  const auto committedRepos = [] (const repo::ArtifactCommitReceipt& receipt) {
    std::set<std::string> result;
    for (const auto& replica : receipt.replicaCommits) {
      if (replica.state == "COMMITTED") result.insert(replica.repoNode);
    }
    return result;
  };
  const auto firstCommittedRepos = committedRepos(*quorumResult.receipt);
  const auto secondCommittedRepos =
    committedRepos(*permutedQuorumResult.receipt);
  BOOST_CHECK_EQUAL(firstCommittedRepos.size(),
                    quorumResult.receipt->requiredAcks);
  BOOST_CHECK_EQUAL(secondCommittedRepos.size(),
                    permutedQuorumResult.receipt->requiredAcks);
  // Independent quorum operations may durably commit different selected
  // subsets. Account for both receipts instead of treating the first result's
  // subset as the oracle for the second operation. The Core manifest is
  // content-addressed and stored once per Repo/object, while receipt rows and
  // LOCATION versions advance for each operation.
  for (const auto& candidate : discovered.candidates) {
    const auto quorumWrites = static_cast<int>(
      firstCommittedRepos.count(candidate.repoNode) +
      secondCommittedRepos.count(candidate.repoNode));
    if (quorumWrites != 0) {
      ++expectedManifestCounts[candidate.repoNode];
    }
    const auto payloadPath = committedPayloadPath(
      repoRoots.at(candidate.repoNode), quorumReference);
    const auto rowCount = persistentReceiptRows(
      metadataDatabases.at(candidate.repoNode), candidate.repoNode,
      candidate.providerIdentity.toUri(), quorumReference);
    BOOST_CHECK_EQUAL(rowCount, quorumWrites);
    BOOST_CHECK_EQUAL(fs::exists(payloadPath), quorumWrites != 0);
    if (quorumWrites != 0) {
      BOOST_CHECK(readFile(payloadPath) == quorumBytes);
      BOOST_CHECK_EQUAL(currentLocationVersion(
        metadataDatabases.at(candidate.repoNode), quorumReference,
        candidate.repoNode, candidate.providerIdentity.toUri()),
        static_cast<uint64_t>(quorumWrites));
    }
    BOOST_CHECK_EQUAL(repoManifestCount(repoRoots.at(candidate.repoNode)),
                      expectedManifestCounts[candidate.repoNode]);
  }

  // T006 exercises direct signed LOCATION and range fetch failover against
  // the real ServiceUser callback boundary. Only the response transport is
  // injected; the API's identity, tuple, digest, deadline and destination
  // checks remain production code.
  using FetchRequestPublisher = ndn_service_framework::ServiceUser::RequestPublisher;
  auto fetchConfig = clientConfig;
  fetchConfig.ackCollectionTimeMs = 10;
  fetchConfig.requestTimeoutMs = 120;
  fetchConfig.fetchFailureCooldownMs = 1200;
  fetchConfig.maxFetchHealthEntries = 8;
  const auto fetchStatusService = repoService(servicePrefix, "STATUS");
  const auto fetchRangeService = repoService(servicePrefix, "FETCH_RANGE");
  const auto fetchHandle = *quorumResult.handle;
  const auto findFetchTarget = [&] (const repo::ArtifactHandle& handle,
                                   const ndn::Name& provider) -> const repo::SelectedRepoTarget& {
    const auto target = std::find_if(handle.selectedTargets.begin(), handle.selectedTargets.end(),
      [&provider] (const auto& item) { return item.providerIdentity == provider; });
    if (target == handle.selectedTargets.end()) {
      throw std::runtime_error("Spec192 fetch fixture cannot find selected Provider");
    }
    return *target;
  };
  const auto findFetchReplica = [&] (const repo::ArtifactHandle& handle,
                                    const repo::SelectedRepoTarget& target) -> const repo::ArtifactReplicaCommit& {
    const auto replica = std::find_if(handle.replicaCommits.begin(), handle.replicaCommits.end(),
      [&target] (const auto& item) {
        return item.repoNode == target.repoNode &&
               item.providerIdentity == target.providerIdentity.toUri() &&
               item.state == "COMMITTED";
      });
    if (replica == handle.replicaCommits.end()) {
      throw std::runtime_error("Spec192 fetch fixture cannot find committed replica");
    }
    return *replica;
  };
  const auto fetchLocationJson = [&] (const repo::ArtifactHandle& handle,
                                     const ndn::Name& provider,
                                     bool newerVersion,
                                     const std::string& locationFault) {
    const auto& target = findFetchTarget(handle, provider);
    const auto& replica = findFetchReplica(handle, target);
    repo::ArtifactLocationRecord location;
    location.repoNode = target.repoNode;
    location.providerIdentity = provider.toUri();
    location.operationId = replica.operationId;
    location.receiptId = replica.receiptId;
    location.reference = handle.reference;
    location.generation = replica.generation;
    location.locationVersion = replica.locationVersion;
    location.committedAtMs = replica.completedAtMs;
    if (newerVersion) {
      ++location.locationVersion;
      location.operationId = "spec192-fetch-newer-operation";
      location.receiptId = "spec192-fetch-newer-receipt";
    }
    const auto now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    location.expiresAtMs = now + 60000;
    if (locationFault == "expired") location.expiresAtMs = now - 1;
    if (locationFault == "tuple-mismatch") location.repoNode += "/mismatch";
    if (locationFault == "equal-version-conflict" && newerVersion) {
      location.operationId = "spec192-fetch-conflicting-operation";
      location.receiptId = "spec192-fetch-conflicting-receipt";
    }
    location.routeState = "ROUTABLE";
    location.forwardingHint = target.servicePrefix.toUri();
    return location.toJson();
  };
  const auto providerIndexFor = [&] (const ndn::Name& provider) {
    for (size_t index = 0; index < environment.providerCount(); ++index) {
      if (environment.provider(index).getName() == provider) return index;
    }
    throw std::runtime_error("Spec192 fetch fixture cannot find Provider transport");
  };
  const auto makeFetchPublisher = [&] (
      const repo::ArtifactHandle& sourceHandle,
      const std::vector<uint8_t>& sourceBytes,
      std::set<std::string> withheldLocations = {},
      std::set<std::string> deniedRanges = {},
      std::set<std::string> corruptRanges = {},
      bool newerLocation = false,
      std::thread* lateLocationThread = nullptr,
      std::string locationFault = {},
      std::atomic<size_t>* rangeRequestCounter = nullptr,
      std::map<std::string, uint32_t> responseDelayMs = {},
      std::string newerLocationProvider = {}) -> FetchRequestPublisher {
    return [&, sourceHandle, sourceBytes,
            withheldLocations = std::move(withheldLocations),
            deniedRanges = std::move(deniedRanges),
            corruptRanges = std::move(corruptRanges),
            newerLocation, lateLocationThread,
            locationFault = std::move(locationFault),
            rangeRequestCounter,
            responseDelayMs = std::move(responseDelayMs),
            newerLocationProvider = std::move(newerLocationProvider)]
      (const ndn::Name& requestId, const ndn::Name&,
       const std::vector<ndn::Name>& providers, const ndn::Name& serviceName,
       const ndn_service_framework::RequestMessage& requestMessage, size_t) mutable {
      if (providers.empty()) return;
      const auto provider = providers.front();
      const auto providerUri = provider.toUri();
      const auto providerIndex = providerIndexFor(provider);
      if (serviceName == fetchStatusService) {
        if (const auto delay = responseDelayMs.find(providerUri);
            delay != responseDelayMs.end() && delay->second != 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(delay->second));
        }
        if (withheldLocations.erase(providerUri) != 0) {
          if (lateLocationThread != nullptr) {
            const auto lateJson = fetchLocationJson(sourceHandle, provider, false, {});
            *lateLocationThread = std::thread([&, requestId, provider, providerIndex,
                                               lateJson, serviceName] {
              std::this_thread::sleep_for(250ms);
              queueAuthenticatedResponseForApiTest(environment, providerIndex, provider,
                serviceName, requestId,
                makeSyntheticResponse(ndn::Buffer(lateJson.begin(), lateJson.end())));
            });
          }
          return;
        }
        const auto requestBuffer = requestMessage.getPayload();
        const std::string requestWire(requestBuffer.begin(), requestBuffer.end());
        if (requestWire.find("\"expectedLocation\"") != std::string::npos) {
          const auto probe = repo::RepoAvailabilityProbeRequest::fromJson(requestWire);
          repo::RepoAvailabilityProbeResponse response;
          response.reference = probe.reference;
          response.expectedLocation = probe.expectedLocation;
          response.report.locationKind = "PERSISTENT";
          response.report.repoNode = probe.expectedLocation.repoNode;
          response.report.providerIdentity = probe.expectedLocation.providerIdentity;
          response.report.generation = probe.expectedLocation.generation;
          response.report.operationId = probe.expectedLocation.operationId;
          response.report.receiptId = probe.expectedLocation.receiptId;
          response.report.locationVersion = probe.expectedLocation.locationVersion;
          response.report.reachability = "REACHABLE";
          response.report.objectPresence = "PRESENT";
          response.report.integrity = "NOT_CHECKED";
          response.report.observedAtMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count());
          response.report.validUntilMs = response.report.observedAtMs + 30000;
          response.report.probeKind = probe.probeKind;
          response.report.reason = "Spec192 authenticated fetch fixture reports present";
          const auto wire = response.toJson();
          queueAuthenticatedResponseForApiTest(environment, providerIndex, provider,
            serviceName, requestId,
            makeSyntheticResponse(ndn::Buffer(wire.begin(), wire.end())));
        }
        else {
          if (locationFault == "wrong-signer") {
            const auto wrongIndex = (providerIndex + 1) % environment.providerCount();
            const auto wire = fetchLocationJson(sourceHandle, provider, false, {});
            queueSyntheticResponse(environment, wrongIndex, provider, serviceName,
              requestId, makeSyntheticResponse(ndn::Buffer(wire.begin(), wire.end())));
            return;
          }
          if (locationFault == "wrong-provider") {
            const auto wrongIndex = (providerIndex + 1) % environment.providerCount();
            const auto wrongProvider = environment.provider(wrongIndex).getName();
            const auto wire = fetchLocationJson(sourceHandle, provider, false, {});
            queueAuthenticatedResponseForApiTest(environment, wrongIndex, wrongProvider,
              serviceName, requestId,
              makeSyntheticResponse(ndn::Buffer(wire.begin(), wire.end())));
            return;
          }
          const bool useNewerLocation = newerLocation &&
            (newerLocationProvider.empty() || newerLocationProvider == providerUri);
          const auto wire = fetchLocationJson(
            sourceHandle, provider, useNewerLocation, locationFault);
          queueAuthenticatedResponseForApiTest(environment, providerIndex, provider,
            serviceName, requestId,
            makeSyntheticResponse(ndn::Buffer(wire.begin(), wire.end())));
        }
        return;
      }
      if (serviceName != fetchRangeService) return;
      if (rangeRequestCounter != nullptr) {
        rangeRequestCounter->fetch_add(1, std::memory_order_acq_rel);
      }
      if (deniedRanges.count(providerUri) != 0) {
        ndn_service_framework::ResponseMessage response;
        response.setStatus(false);
        response.setErrorInfo("repo-authorization-denied: Spec192 fetch fixture");
        queueAuthenticatedResponseForApiTest(environment, providerIndex, provider,
          serviceName, requestId, std::move(response));
        return;
      }
      const auto requestBuffer = requestMessage.getPayload();
      const std::vector<uint8_t> requestBytes(requestBuffer.begin(), requestBuffer.end());
      std::string objectName;
      repo::RepoByteRange range;
      repo::decodeRangeReadRequest(requestBytes, objectName, range);
      (void)objectName;
      if (range.offsetBytes > sourceBytes.size() ||
          range.lengthBytes > sourceBytes.size() - range.offsetBytes) {
        throw std::runtime_error("Spec192 fetch fixture range is outside payload");
      }
      const auto begin = sourceBytes.begin() + static_cast<std::ptrdiff_t>(range.offsetBytes);
      std::vector<uint8_t> bytes(begin,
        begin + static_cast<std::ptrdiff_t>(range.lengthBytes));
      if (corruptRanges.erase(providerUri) != 0 && !bytes.empty()) bytes.front() ^= 0xff;
      queueAuthenticatedResponseForApiTest(environment, providerIndex, provider,
        serviceName, requestId,
        makeSyntheticResponse(ndn::Buffer(bytes.begin(), bytes.end())));
    };
  };
  const auto runFetchWithPublisher = [&] (FetchRequestPublisher publisher,
                                         auto&& action) {
    ScopedRequestPublisher publisherGuard(environment.user(), std::move(publisher));
    return runWithFacePump(environment, std::forward<decltype(action)>(action));
  };
  const auto seedTarget = std::find_if(fetchHandle.selectedTargets.begin(),
    fetchHandle.selectedTargets.end(), [&fetchHandle] (const auto& target) {
      return target.repoNode == fetchHandle.seedRepoId;
    });
  BOOST_REQUIRE(seedTarget != fetchHandle.selectedTargets.end());
  const auto seedProvider = seedTarget->providerIdentity;
  const auto seedUri = seedProvider.toUri();

  for (auto mode : {spec202_test::Fault::SyncError, spec202_test::Fault::SyncDelay}) {
    for (bool existing : {false, true}) {
      const auto directory = temporary.path() /
        ("postrename-" + std::to_string(static_cast<int>(mode)) + (existing ? "-old" : "-absent"));
      fs::create_directory(directory);
      const auto destination = directory / "output.bin";
      const std::vector<uint8_t> oldBytes{0x6f, 0x6c, 0x64};
      struct stat before {};
      if (existing) {
        writeFile(destination, oldBytes);
        BOOST_REQUIRE_EQUAL(::lstat(destination.c_str(), &before), 0);
      }
      std::set<std::string> withheld;
      for (const auto& target : fetchHandle.selectedTargets)
        if (target.providerIdentity != seedProvider) withheld.insert(target.providerIdentity.toUri());
      auto api = repo::ArtifactRepositoryApi::makePersistent(environment.user(), fetchConfig);
      repo::ArtifactFetchResult failure;
      bool failed = false;
      {
        spec202_test::ScopedFault fault(destination, mode, 1200);
        failure = runFetchWithPublisher(makeFetchPublisher(fetchHandle, quorumBytes, withheld), [&] {
          try { return api.fetchFile(fetchHandle, destination.string(), 1000); }
          catch (const repo::ArtifactFetchError& error) { failed = true; return error.result(); }
        });
        BOOST_CHECK_EQUAL(fault.hits(), 1U);
      }
      BOOST_CHECK(failed);
      BOOST_CHECK_EQUAL(failure.bytesVerified, 0U);
      BOOST_CHECK(std::all_of(failure.attempts.begin(), failure.attempts.end(), [] (const auto& attempt) {
        return attempt.bytesVerified == 0 && attempt.outcome != "SUCCEEDED";
      }));
      if (existing) {
        struct stat after {};
        BOOST_REQUIRE_EQUAL(::lstat(destination.c_str(), &after), 0);
        BOOST_CHECK_EQUAL(after.st_ino, before.st_ino);
        BOOST_CHECK(readFile(destination) == oldBytes);
      }
      else BOOST_CHECK(!fs::exists(destination));
      BOOST_CHECK_EQUAL(std::distance(fs::directory_iterator(directory), fs::directory_iterator()),
                        existing ? 1 : 0);
      auto healthy = repo::ArtifactRepositoryApi::makePersistent(environment.user(), fetchConfig);
      const auto success = runFetchWithPublisher(makeFetchPublisher(fetchHandle, quorumBytes), [&] {
        return healthy.fetchFile(fetchHandle, destination.string(), 1000);
      });
      BOOST_CHECK_EQUAL(success.bytesVerified, quorumBytes.size());
      BOOST_CHECK(readFile(destination) == quorumBytes);
    }
  }

  for (auto mode : {spec202_test::Fault::RollbackRename, spec202_test::Fault::RollbackSync,
                    spec202_test::Fault::CleanupUnlink, spec202_test::Fault::PreparationCleanup}) {
    const auto directory = temporary.path() / ("recovery-" + std::to_string(static_cast<int>(mode)));
    fs::create_directory(directory);
    const auto destination = directory / "output.bin";
    const std::vector<uint8_t> oldBytes{0x6f, 0x6c, 0x64};
    writeFile(destination, oldBytes);
    auto api = repo::ArtifactRepositoryApi::makePersistent(environment.user(), fetchConfig);
    std::string message;
    bool failed = false;
    spec202_test::ScopedFault fault(destination, mode);
    const auto result = runFetchWithPublisher(makeFetchPublisher(fetchHandle, quorumBytes), [&] {
      try { return api.fetchFile(fetchHandle, destination.string(), 1000); }
      catch (const repo::ArtifactFetchError& error) {
        failed = true; message = error.what(); return error.result();
      }
    });
    BOOST_CHECK_EQUAL(result.attempts.size(), 1U);
    if (mode == spec202_test::Fault::CleanupUnlink) {
      BOOST_CHECK(!failed);
      BOOST_CHECK_EQUAL(fault.hits(), 1U);
      BOOST_CHECK_EQUAL(result.bytesVerified, quorumBytes.size());
      BOOST_CHECK(readFile(destination) == quorumBytes);
    }
    else {
      BOOST_CHECK(failed);
      BOOST_CHECK_EQUAL(fault.hits(), 2U);
      BOOST_CHECK_EQUAL(result.bytesVerified, 0U);
      BOOST_CHECK(message.find("repo-artifact-destination-recovery-required") == 0);
    }
    bool foundOld = false;
    for (const auto& entry : fs::directory_iterator(directory)) {
      if (entry.is_regular_file() && readFile(entry.path()) == oldBytes) {
        foundOld = true;
        if (mode != spec202_test::Fault::CleanupUnlink)
          BOOST_CHECK(message.find(entry.path().string()) != std::string::npos);
      }
    }
    BOOST_CHECK(foundOld);
  }

  // A missing Face/Core timeout callback must not leave this synchronous
  // wrapper blocked beyond its own request deadline. Keep the Face pump idle
  // until the independent waiter has had time to settle.
  auto callbackDeadlineApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  std::future<repo::ArtifactFetchResult> callbackDeadlineFuture;
  std::future_status callbackDeadlineStatus;
  {
    ScopedRequestPublisher publisherGuard(environment.user(),
      makeFetchPublisher(fetchHandle, quorumBytes, {seedUri}));
    callbackDeadlineFuture = std::async(std::launch::async, [&] {
      try {
        return callbackDeadlineApi.fetchFile(fetchHandle,
          (temporary.path() / "fetch-missing-timeout-callback.bin").string(), 80);
      }
      catch (const repo::ArtifactFetchError& error) {
        return error.result();
      }
    });
    callbackDeadlineStatus = callbackDeadlineFuture.wait_for(350ms);
    if (callbackDeadlineStatus != std::future_status::ready) {
      environment.pumpUntilWithAttributeAuthority([&] {
        return callbackDeadlineFuture.wait_for(0ms) == std::future_status::ready;
      });
    }
  }
  BOOST_CHECK(callbackDeadlineStatus == std::future_status::ready);
  const auto callbackDeadlineResult = callbackDeadlineFuture.get();
  BOOST_REQUIRE_EQUAL(callbackDeadlineResult.attempts.size(), 1U);
  BOOST_CHECK_EQUAL(callbackDeadlineResult.attempts.front().failureClass, "timeout");
  BOOST_CHECK_LE(callbackDeadlineResult.totalElapsedMs, 250U);

  auto duplicateFetchApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  auto duplicateProviderHandle = fetchHandle;
  BOOST_REQUIRE(duplicateProviderHandle.selectedTargets.size() > 1);
  const auto seedTargetIndex = static_cast<size_t>(std::distance(
    fetchHandle.selectedTargets.begin(), seedTarget));
  const auto duplicateProviderIndex = seedTargetIndex == 0 ? 1U : 0U;
  duplicateProviderHandle.selectedTargets[duplicateProviderIndex].providerIdentity = seedProvider;
  std::atomic<size_t> duplicateFetchInterests{0};
  const auto duplicateProviderError = runFetchWithPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const ndn_service_framework::RequestMessage&, size_t) {
      duplicateFetchInterests.fetch_add(1, std::memory_order_acq_rel);
    }, [&] {
      try {
        (void)duplicateFetchApi.fetchFile(
          duplicateProviderHandle,
          (temporary.path() / "duplicate-fetch.bin").string(), 1000);
      }
      catch (const std::exception& error) {
        return std::string(error.what());
      }
      return std::string{};
    });
  BOOST_CHECK(duplicateProviderError.find("duplicate") != std::string::npos);
  BOOST_CHECK_EQUAL(duplicateFetchInterests.load(std::memory_order_acquire), 0U);

  std::thread lateLocationThread;
  auto fallbackApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  const auto fallbackPath = temporary.path() / "fetch-timeout-fallback.bin";
  const auto timeoutFallback = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {seedUri}, {}, {}, false,
                       &lateLocationThread), [&] {
      return fallbackApi.fetchFile(fetchHandle, fallbackPath.string(), 1500);
    });
  BOOST_REQUIRE_EQUAL(timeoutFallback.attempts.size(), 2U);
  BOOST_CHECK_EQUAL(timeoutFallback.attempts.front().providerIdentity, seedUri);
  BOOST_CHECK_EQUAL(timeoutFallback.attempts.front().failureClass, "timeout");
  BOOST_CHECK_EQUAL(timeoutFallback.attempts.back().outcome, "SUCCEEDED");
  BOOST_CHECK(timeoutFallback.attempts.back().providerIdentity != seedUri);
  BOOST_CHECK_EQUAL(timeoutFallback.bytesVerified, quorumBytes.size());
  BOOST_CHECK(readFile(fallbackPath) == quorumBytes);
  BOOST_REQUIRE(lateLocationThread.joinable());
  lateLocationThread.join();
  const auto lateResponsePump = runWithFacePump(environment, [&] {
    std::this_thread::sleep_for(20ms);
    return true;
  });
  BOOST_CHECK(lateResponsePump);
  BOOST_CHECK_EQUAL(timeoutFallback.attempts.size(), 2U);

  const auto healthyPublisher = makeFetchPublisher(fetchHandle, quorumBytes);
  const auto cooledPath = temporary.path() / "fetch-cooldown-order.bin";
  const auto cooledOrder = runFetchWithPublisher(healthyPublisher, [&] {
    return fallbackApi.fetchFile(fetchHandle, cooledPath.string(), 1000);
  });
  BOOST_REQUIRE(!cooledOrder.attempts.empty());
  BOOST_CHECK(cooledOrder.attempts.front().providerIdentity != seedUri);

  const auto probed = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes), [&] {
      return fallbackApi.checkAvailability(fetchHandle, "metadata", 1500);
    });
  BOOST_REQUIRE_EQUAL(probed.locations.size(), fetchHandle.replicaCommits.size());
  for (size_t i = 0; i < fetchHandle.replicaCommits.size(); ++i) {
    const auto& replica = fetchHandle.replicaCommits[i];
    const auto& location = probed.locations[i];
    BOOST_CHECK_EQUAL(location.repoNode, replica.repoNode);
    BOOST_CHECK_EQUAL(location.providerIdentity, replica.providerIdentity);
    if (replica.state == "COMMITTED" && !replica.receiptId.empty()) {
      BOOST_CHECK_EQUAL(location.reachability, "REACHABLE");
      BOOST_CHECK_EQUAL(location.objectPresence, "PRESENT");
    }
    else {
      BOOST_CHECK_EQUAL(location.reachability, "UNKNOWN");
      BOOST_CHECK_EQUAL(location.objectPresence, "UNKNOWN");
    }
  }
  const auto readmittedPath = temporary.path() / "fetch-probe-readmission.bin";
  const auto afterProbe = runFetchWithPublisher(healthyPublisher, [&] {
    return fallbackApi.fetchFile(fetchHandle, readmittedPath.string(), 1000);
  });
  BOOST_REQUIRE(!afterProbe.attempts.empty());
  BOOST_CHECK_EQUAL(afterProbe.attempts.front().providerIdentity, seedUri);
  BOOST_CHECK(!afterProbe.attempts.front().wasCoolingDownAtSelection);

  auto contentionApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  auto firstContentionRequest = std::make_shared<std::promise<void>>();
  auto firstContentionStarted = std::make_shared<std::atomic<bool>>(false);
  auto contentionStatusCount = std::make_shared<std::atomic<size_t>>(0);
  const auto contentionPublisher = FetchRequestPublisher{
    [&, firstContentionRequest, firstContentionStarted, contentionStatusCount]
    (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
     const ndn::Name& serviceName,
     const ndn_service_framework::RequestMessage&, size_t) {
      if (serviceName != fetchStatusService) return;
      contentionStatusCount->fetch_add(1, std::memory_order_acq_rel);
      if (!firstContentionStarted->exchange(true, std::memory_order_acq_rel)) {
        firstContentionRequest->set_value();
      }
    }};
  const auto contentionResults = runFetchWithPublisher(contentionPublisher, [&] {
    auto firstCall = std::async(std::launch::async, [&] {
      try {
        return contentionApi.fetchFile(fetchHandle,
          (temporary.path() / "fetch-lock-holder.bin").string(), 800);
      }
      catch (const repo::ArtifactFetchError& error) {
        return error.result();
      }
    });
    auto firstRequestReady = firstContentionRequest->get_future();
    if (firstRequestReady.wait_for(1s) != std::future_status::ready) {
      throw std::runtime_error("Spec192 lock-holder fetch did not begin LOCATION");
    }
    repo::ArtifactFetchResult lockWaitFailure;
    try {
      (void)contentionApi.fetchFile(fetchHandle,
        (temporary.path() / "fetch-lock-waiter.bin").string(), 60);
      throw std::runtime_error("Spec192 lock waiter unexpectedly acquired the API");
    }
    catch (const repo::ArtifactFetchError& error) {
      lockWaitFailure = error.result();
    }
    const auto lockHolderFailure = firstCall.get();
    return std::make_pair(lockHolderFailure, lockWaitFailure);
  });
  BOOST_REQUIRE_EQUAL(contentionResults.first.attempts.size(), 2U);
  BOOST_CHECK(contentionResults.first.totalElapsedMs <= 400);
  BOOST_CHECK(contentionResults.second.attempts.empty());
  BOOST_CHECK(contentionResults.second.totalElapsedMs <= 120);
  BOOST_CHECK_EQUAL(contentionStatusCount->load(std::memory_order_acquire), 2U);

  auto cooldownConfig = fetchConfig;
  cooldownConfig.fetchFailureCooldownMs = 250;
  auto cooldownApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), cooldownConfig);
  const std::set<std::string> allCommittedProviders = [&] {
    std::set<std::string> result;
    for (const auto& replica : fetchHandle.replicaCommits) {
      if (replica.state == "COMMITTED") result.insert(replica.providerIdentity);
    }
    return result;
  }();
  const auto allTimeoutPublisher = [&] {
    return makeFetchPublisher(fetchHandle, quorumBytes, allCommittedProviders);
  };
  const auto runAllTimeout = [&] (const std::string& filename) {
    return runFetchWithPublisher(allTimeoutPublisher(), [&] {
      try {
        (void)cooldownApi.fetchFile(fetchHandle,
          (temporary.path() / filename).string(), 240);
      }
      catch (const repo::ArtifactFetchError& error) {
        return error.result();
      }
      throw std::runtime_error("Spec192 all-timeout fixture unexpectedly succeeded");
    });
  };
  const auto cooldownInitial = runAllTimeout("fetch-cooldown-initial.bin");
  BOOST_CHECK(cooldownInitial.totalElapsedMs <= 400);
  const auto cooldownActive = runAllTimeout("fetch-cooldown-active.bin");
  const auto activeSeed = std::find_if(cooldownActive.attempts.begin(),
    cooldownActive.attempts.end(), [&seedUri] (const auto& attempt) {
      return attempt.providerIdentity == seedUri;
    });
  BOOST_REQUIRE(activeSeed != cooldownActive.attempts.end());
  BOOST_CHECK(activeSeed->wasCoolingDownAtSelection);
  std::this_thread::sleep_for(300ms);
  const auto cooldownExpired = runAllTimeout("fetch-cooldown-expired.bin");
  const auto expiredSeed = std::find_if(cooldownExpired.attempts.begin(),
    cooldownExpired.attempts.end(), [&seedUri] (const auto& attempt) {
      return attempt.providerIdentity == seedUri;
    });
  BOOST_REQUIRE(expiredSeed != cooldownExpired.attempts.end());
  BOOST_CHECK(!expiredSeed->wasCoolingDownAtSelection);

  auto boundedHealthConfig = cooldownConfig;
  boundedHealthConfig.fetchFailureCooldownMs = 3000;
  boundedHealthConfig.maxFetchHealthEntries = 1;
  auto boundedHealthApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), boundedHealthConfig);
  const auto boundedHealthFetch = [&] (const std::string& filename) {
    return runFetchWithPublisher(allTimeoutPublisher(), [&] {
      try {
        (void)boundedHealthApi.fetchFile(fetchHandle,
          (temporary.path() / filename).string(), 400);
      }
      catch (const repo::ArtifactFetchError& error) {
        return error.result();
      }
      throw std::runtime_error("Spec192 bounded-health fixture unexpectedly succeeded");
    });
  };
  const auto healthCapacityFilled = boundedHealthFetch("fetch-health-capacity-fill.bin");
  BOOST_REQUIRE_EQUAL(healthCapacityFilled.attempts.size(), 2U);
  const auto healthCapacityEvicted = boundedHealthFetch("fetch-health-capacity-evict.bin");
  BOOST_REQUIRE(!healthCapacityEvicted.attempts.empty());
  BOOST_CHECK_EQUAL(healthCapacityEvicted.attempts.front().providerIdentity, seedUri);
  BOOST_CHECK(!healthCapacityEvicted.attempts.front().wasCoolingDownAtSelection);

  auto budgetConfig = cooldownConfig;
  budgetConfig.requestTimeoutMs = 280;
  auto budgetApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), budgetConfig);
  const auto budgetStarted = std::chrono::steady_clock::now();
  const auto boundedAllTimeouts = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, allCommittedProviders), [&] {
      try {
        (void)budgetApi.fetchFile(fetchHandle,
          (temporary.path() / "fetch-global-budget.bin").string(), 400);
      }
      catch (const repo::ArtifactFetchError& error) {
        return error.result();
      }
      throw std::runtime_error("Spec192 global-budget fixture unexpectedly succeeded");
    });
  const auto boundedAllTimeoutsWallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - budgetStarted).count();
  BOOST_REQUIRE_EQUAL(boundedAllTimeouts.attempts.size(), 2U);
  BOOST_CHECK_LE(boundedAllTimeoutsWallMs, 500); // 400 ms budget + frozen 100 ms tolerance
  BOOST_CHECK_LT(boundedAllTimeouts.attempts.back().requestTimeoutBudgetMs,
                 static_cast<uint64_t>(budgetConfig.requestTimeoutMs));

  auto corruptApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  const auto corruptPath = temporary.path() / "fetch-corrupt-fallback.bin";
  const auto corruptFallback = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {}, {}, {seedUri}), [&] {
      return corruptApi.fetchFile(fetchHandle, corruptPath.string(), 1200);
    });
  BOOST_REQUIRE_EQUAL(corruptFallback.attempts.size(), 2U);
  BOOST_CHECK_EQUAL(corruptFallback.attempts.front().failureClass, "integrity");
  BOOST_CHECK_EQUAL(corruptFallback.attempts.front().bytesVerified, 0U);
  BOOST_CHECK_EQUAL(corruptFallback.bytesVerified, quorumBytes.size());
  BOOST_CHECK(readFile(corruptPath) == quorumBytes);

  auto deniedApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  const auto deniedPath = temporary.path() / "fetch-denied-fallback.bin";
  const auto deniedFallback = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {}, {seedUri}), [&] {
      return deniedApi.fetchFile(fetchHandle, deniedPath.string(), 1200);
    });
  BOOST_REQUIRE_EQUAL(deniedFallback.attempts.size(), 2U);
  BOOST_CHECK_EQUAL(deniedFallback.attempts.front().failureClass, "authorization");
  BOOST_CHECK(readFile(deniedPath) == quorumBytes);

  auto newerConfig = fetchConfig;
  newerConfig.fetchFailureCooldownMs = 5000;
  auto newerApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), newerConfig);
  (void)runFetchWithPublisher(allTimeoutPublisher(), [&] {
    try {
      (void)newerApi.fetchFile(fetchHandle,
        (temporary.path() / "fetch-newer-prime.bin").string(), 240);
    }
    catch (const repo::ArtifactFetchError&) {
      return true;
    }
    throw std::runtime_error("Spec192 newer-version fixture unexpectedly succeeded");
  });
  const auto newerPath = temporary.path() / "fetch-newer-readmission.bin";
  const auto newerReadmission = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {}, {}, {}, true), [&] {
      return newerApi.fetchFile(fetchHandle, newerPath.string(), 1000);
    });
  BOOST_REQUIRE(!newerReadmission.attempts.empty());
  BOOST_CHECK_EQUAL(newerReadmission.attempts.front().providerIdentity, seedUri);
  BOOST_CHECK(newerReadmission.attempts.front().wasCoolingDownAtSelection);
  BOOST_CHECK(newerReadmission.attempts.front().locationVersion >
              findFetchReplica(fetchHandle,
                findFetchTarget(fetchHandle, seedProvider)).locationVersion);
  BOOST_CHECK(readFile(newerPath) == quorumBytes);
  const auto newerFollowup = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {}, {}, {}, true), [&] {
      return newerApi.fetchFile(fetchHandle,
        (temporary.path() / "fetch-newer-followup.bin").string(), 1000);
    });
  BOOST_REQUIRE(!newerFollowup.attempts.empty());
  BOOST_CHECK_EQUAL(newerFollowup.attempts.front().providerIdentity, seedUri);
  BOOST_CHECK(!newerFollowup.attempts.front().wasCoolingDownAtSelection);
  BOOST_CHECK_EQUAL(newerFollowup.attempts.front().locationVersion,
                    newerReadmission.attempts.front().locationVersion);

  auto latencyConfig = fetchConfig;
  latencyConfig.fetchFailureCooldownMs = 0;
  auto latencyApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), latencyConfig);
  const auto latencyPrime = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {}, {}, {}, false, nullptr,
                       {}, nullptr, {{seedUri, 60}}), [&] {
      return latencyApi.fetchFile(fetchHandle,
        (temporary.path() / "fetch-ewma-prime.bin").string(), 1200);
    });
  BOOST_REQUIRE(!latencyPrime.attempts.empty());
  BOOST_CHECK_EQUAL(latencyPrime.attempts.front().providerIdentity, seedUri);
  const auto latencyOrder = runFetchWithPublisher(healthyPublisher, [&] {
    return latencyApi.fetchFile(fetchHandle,
      (temporary.path() / "fetch-ewma-order.bin").string(), 1200);
  });
  BOOST_REQUIRE(!latencyOrder.attempts.empty());
  BOOST_CHECK(latencyOrder.attempts.front().providerIdentity != seedUri);
  BOOST_CHECK(!latencyOrder.attempts.front().wasCoolingDownAtSelection);

  // An old handle whose Provider already advertised a newer current LOCATION
  // must not cool that newer version when the old-receipt LOCATION times out.
  auto versionScopeApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  (void)runFetchWithPublisher(healthyPublisher, [&] {
    return versionScopeApi.fetchFile(fetchHandle,
      (temporary.path() / "fetch-version-scope-prime.bin").string(), 1200);
  });
  const auto versionScopeProbe = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {}, {}, {}, true, nullptr,
                       {}, nullptr, {}, seedUri), [&] {
      return versionScopeApi.checkAvailability(fetchHandle, "metadata", 1500);
    });
  BOOST_REQUIRE_EQUAL(versionScopeProbe.locations.size(), fetchHandle.replicaCommits.size());
  const auto seedReplica = std::find_if(fetchHandle.replicaCommits.begin(),
    fetchHandle.replicaCommits.end(), [&seedTarget] (const auto& replica) {
      return replica.repoNode == seedTarget->repoNode &&
             replica.providerIdentity == seedTarget->providerIdentity.toUri();
    });
  BOOST_REQUIRE(seedReplica != fetchHandle.replicaCommits.end());
  const auto seedReplicaIndex = static_cast<size_t>(std::distance(
    fetchHandle.replicaCommits.begin(), seedReplica));
  BOOST_CHECK_EQUAL(versionScopeProbe.locations[seedReplicaIndex].repoNode,
                    seedReplica->repoNode);
  BOOST_CHECK_EQUAL(versionScopeProbe.locations[seedReplicaIndex].providerIdentity,
                    seedReplica->providerIdentity);
  BOOST_CHECK_GT(versionScopeProbe.locations[seedReplicaIndex].locationVersion,
                 seedReplica->locationVersion);
  const auto staleTimeoutFallback = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {seedUri}), [&] {
      return versionScopeApi.fetchFile(fetchHandle,
        (temporary.path() / "fetch-stale-receipt-fallback.bin").string(), 1500);
    });
  BOOST_REQUIRE_EQUAL(staleTimeoutFallback.attempts.size(), 2U);
  BOOST_CHECK_EQUAL(staleTimeoutFallback.attempts.front().failureClass, "timeout");
  const auto afterStaleFailure = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {}, {}, {}, true, nullptr,
                       {}, nullptr, {}, seedUri), [&] {
      return versionScopeApi.fetchFile(fetchHandle,
        (temporary.path() / "fetch-stale-receipt-readmission.bin").string(), 1200);
    });
  BOOST_REQUIRE(!afterStaleFailure.attempts.empty());
  BOOST_CHECK_EQUAL(afterStaleFailure.attempts.front().providerIdentity, seedUri);
  BOOST_CHECK(!afterStaleFailure.attempts.front().wasCoolingDownAtSelection);

  const std::vector<uint8_t> sentinelBytes{0x53, 0x41, 0x46, 0x45};
  const auto expectLocationFailure = [&] (repo::ArtifactRepositoryApi& api,
                                         const std::string& fault,
                                         const std::string& fileName,
                                         const std::string& expectedFailureClass,
                                         bool newerLocation = false) {
    const auto destinationPath = temporary.path() / fileName;
    writeFile(destinationPath, sentinelBytes);
    std::atomic<size_t> rangeRequests{0};
    const auto failed = runFetchWithPublisher(
      makeFetchPublisher(fetchHandle, quorumBytes, {}, {}, {}, newerLocation, nullptr,
                         fault, &rangeRequests), [&] () -> repo::ArtifactFetchResult {
        try {
          return api.fetchFile(fetchHandle, destinationPath.string(), 1200);
        }
        catch (const repo::ArtifactFetchError& error) {
          return error.result();
        }
        throw std::runtime_error("Spec192 invalid LOCATION fixture unexpectedly succeeded");
      });
    BOOST_REQUIRE_EQUAL(failed.attempts.size(), 2U);
    BOOST_CHECK(std::all_of(failed.attempts.begin(), failed.attempts.end(),
      [&expectedFailureClass] (const auto& attempt) {
        return attempt.outcome == "FAILED" &&
               attempt.failureClass == expectedFailureClass;
      }));
    BOOST_CHECK_EQUAL(rangeRequests.load(std::memory_order_acquire), 0U);
    BOOST_CHECK(readFile(destinationPath) == sentinelBytes);
  };

  for (const auto& faultCase : std::vector<std::pair<std::string, std::string>>{
         {"wrong-signer", "timeout"},
         {"expired", "location"},
         {"tuple-mismatch", "location"}}) {
    auto invalidLocationApi = repo::ArtifactRepositoryApi::makePersistent(
      environment.user(), fetchConfig);
    expectLocationFailure(invalidLocationApi, faultCase.first,
      "fetch-invalid-location-" + faultCase.first + ".bin", faultCase.second);
  }

  // Failed ranged transfers must keep an existing destination intact even
  // after bytes have arrived and failed digest verification.
  auto corruptAllApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  const auto corruptAllPath = temporary.path() / "fetch-corrupt-all-preserves-destination.bin";
  writeFile(corruptAllPath, sentinelBytes);
  std::atomic<size_t> corruptRangeRequests{0};
  const auto corruptAll = runFetchWithPublisher(
    makeFetchPublisher(fetchHandle, quorumBytes, {}, {}, allCommittedProviders,
                       false, nullptr, {}, &corruptRangeRequests), [&] {
      try {
        return corruptAllApi.fetchFile(fetchHandle, corruptAllPath.string(), 1200);
      }
      catch (const repo::ArtifactFetchError& error) {
        return error.result();
      }
      throw std::runtime_error("Spec192 corrupt-range fixture unexpectedly succeeded");
    });
  BOOST_REQUIRE_EQUAL(corruptAll.attempts.size(), 2U);
  BOOST_CHECK(std::all_of(corruptAll.attempts.begin(), corruptAll.attempts.end(),
    [] (const auto& attempt) {
      return attempt.outcome == "FAILED" && attempt.failureClass == "integrity" &&
             attempt.bytesTransferred > 0 && attempt.bytesVerified == 0;
    }));
  BOOST_CHECK_GE(corruptRangeRequests.load(std::memory_order_acquire), 2U);
  BOOST_CHECK_GT(corruptAll.bytesTransferred, 0U);
  BOOST_CHECK_EQUAL(corruptAll.bytesVerified, 0U);
  BOOST_CHECK(readFile(corruptAllPath) == sentinelBytes);

  // Destination setup errors are operation failures too, so they expose the
  // bounded fetch trace even though no Provider attempt could start.
  auto destinationSetupApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  const auto destinationParentFile = temporary.path() / "fetch-destination-parent-file";
  writeFile(destinationParentFile, sentinelBytes);
  const auto impossibleDestination = destinationParentFile / "child.bin";
  std::string destinationSetupMessage;
  const auto destinationSetupResult = runFetchWithPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const ndn_service_framework::RequestMessage&, size_t) {},
    [&] {
      try {
        return destinationSetupApi.fetchFile(
          fetchHandle, impossibleDestination.string(), 1000);
      }
      catch (const repo::ArtifactFetchError& error) {
        destinationSetupMessage = error.what();
        return error.result();
      }
      throw std::runtime_error("Spec192 destination-setup fixture unexpectedly succeeded");
    });
  BOOST_CHECK(destinationSetupMessage.find("destination-directory-failed") !=
              std::string::npos);
  BOOST_CHECK(destinationSetupResult.reference == fetchHandle.reference);
  BOOST_CHECK(destinationSetupResult.attempts.empty());
  BOOST_CHECK(readFile(destinationParentFile) == sentinelBytes);

  auto tupleHistoryApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), fetchConfig);
  (void)runFetchWithPublisher(makeFetchPublisher(fetchHandle, quorumBytes, {seedUri}), [&] {
    return tupleHistoryApi.fetchFile(fetchHandle,
      (temporary.path() / "fetch-location-history-prime.bin").string(), 1500);
  });
  const auto newerProbePublisher = makeFetchPublisher(
    fetchHandle, quorumBytes, {}, {}, {}, true);
  const auto tupleHistory = runFetchWithPublisher(newerProbePublisher, [&] {
    return tupleHistoryApi.checkAvailability(fetchHandle, "metadata", 1500);
  });
  BOOST_REQUIRE_EQUAL(tupleHistory.locations.size(), fetchHandle.replicaCommits.size());
  for (size_t index = 0; index < fetchHandle.replicaCommits.size(); ++index) {
    const auto& replica = fetchHandle.replicaCommits[index];
    BOOST_CHECK_EQUAL(tupleHistory.locations[index].repoNode, replica.repoNode);
    BOOST_CHECK_EQUAL(tupleHistory.locations[index].providerIdentity,
                      replica.providerIdentity);
    if (replica.state == "COMMITTED") {
      BOOST_CHECK_GT(tupleHistory.locations[index].locationVersion,
                     replica.locationVersion);
    }
  }
  // The preceding successful probe installed v2 health records. Return each
  // Provider's valid signed historical v1 LOCATION to reach rollback handling.
  expectLocationFailure(tupleHistoryApi, "rollback",
    "fetch-location-rollback.bin", "location");
  expectLocationFailure(tupleHistoryApi, "equal-version-conflict",
    "fetch-location-equal-version-conflict.bin", "location", true);

  for (const auto& attempt : timeoutFallback.attempts) {
    BOOST_CHECK_LE(attempt.requestTimeoutBudgetMs,
                   static_cast<uint64_t>(fetchConfig.requestTimeoutMs));
  }
  BOOST_CHECK_GT(timeoutFallback.attempts.back().locationVersion, 0U);

  const std::vector<uint8_t> archivedBytes{
    0x61, 0x72, 0x63, 0x68, 0x69, 0x76, 0x65, 0x2d, 0x73, 0x6f, 0x75, 0x72, 0x63, 0x65,
  };
  const auto archivedReference = makeReference("inapp-archive", archivedBytes);
  const auto archivedSourcePath = temporary.path() / "inapp-archive.bin";
  writeFile(archivedSourcePath, archivedBytes);
  const repo::InAppRepoConfig inAppConfig{
    temporary.path() / "app-local", "spec192-publisher", clientConfig.limits};
  std::optional<repo::ArtifactHandle> localHandle;
  {
    auto inApp = repo::ArtifactRepositoryApi::makeInApp(inAppConfig);
    const auto localCommit = inApp.publishFile(
      archivedSourcePath.string(), archivedReference);
    BOOST_REQUIRE(localCommit.handle);
    localHandle = *localCommit.handle;
  }
  auto reopenedInApp = repo::ArtifactRepositoryApi::makeInApp(inAppConfig);
  auto archivePlacement = selected;
  archivePlacement.consistency = repo::RepoWriteConsistency::One;

  // Hold the In-App API operation lock inside a local commit blocked by a
  // separate SQLite writer. beginArchive must charge this source-side lock
  // wait to its entry deadline and must not start Persistent requests.
  auto archiveDeadlineConfig = clientConfig;
  archiveDeadlineConfig.ackCollectionTimeMs = 10;
  archiveDeadlineConfig.requestTimeoutMs = 250;
  auto archiveDeadlineApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), archiveDeadlineConfig);
  std::atomic<size_t> archiveRequestStarts{0};
  const ndn_service_framework::ServiceUser::RequestPublisher archiveRequestPublisher =
    [&archiveRequestStarts] (const ndn::Name&, const ndn::Name&,
      const std::vector<ndn::Name>&, const ndn::Name&,
      const ndn_service_framework::RequestMessage&, size_t) {
      archiveRequestStarts.fetch_add(1, std::memory_order_relaxed);
    };
  sqlite3* rawArchiveLockDatabase = nullptr;
  const auto archiveDatabasePath =
    (inAppConfig.durableRoot / "metadata.sqlite3").string();
  const int archiveDatabaseOpenResult = sqlite3_open_v2(
    archiveDatabasePath.c_str(), &rawArchiveLockDatabase,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
  BOOST_REQUIRE_EQUAL(archiveDatabaseOpenResult, SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> archiveLockDatabase(
    rawArchiveLockDatabase, sqlite3_close);
  char* archiveLockError = nullptr;
  const int archiveLockResult = sqlite3_exec(
    archiveLockDatabase.get(), "BEGIN IMMEDIATE", nullptr, nullptr,
    &archiveLockError);
  const std::string archiveLockErrorText =
    archiveLockError == nullptr ? std::string{} : archiveLockError;
  sqlite3_free(archiveLockError);
  BOOST_REQUIRE_MESSAGE(archiveLockResult == SQLITE_OK,
                        "cannot hold In-App SQLite writer lock: "
                          << archiveLockErrorText);

  const std::vector<uint8_t> blockedLocalBytes{0x62, 0x75, 0x73, 0x79};
  const auto blockedLocalReference = makeReference(
    "archive-source-operation-lock", blockedLocalBytes);
  const auto blockedLocalPath = temporary.path() / "archive-source-lock.bin";
  writeFile(blockedLocalPath, blockedLocalBytes);
  std::atomic<bool> blockedLocalPublishStarted{false};
  auto blockedLocalPublishFuture = std::async(std::launch::async, [&] {
    blockedLocalPublishStarted.store(true, std::memory_order_release);
    try {
      (void)reopenedInApp.publishFile(
        blockedLocalPath.string(), blockedLocalReference);
      return std::string{};
    }
    catch (const std::exception& error) {
      return std::string(error.what());
    }
  });
  const auto localPublishStartDeadline =
    std::chrono::steady_clock::now() + 500ms;
  while (!blockedLocalPublishStarted.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < localPublishStartDeadline) {
    std::this_thread::sleep_for(1ms);
  }
  const bool localPublishStartedInTime =
    blockedLocalPublishStarted.load(std::memory_order_acquire);
  if (localPublishStartedInTime) std::this_thread::sleep_for(100ms);
  const bool localPublishHoldsApiLock =
    localPublishStartedInTime &&
    blockedLocalPublishFuture.wait_for(0ms) != std::future_status::ready;
  bool archiveReturnedWithinBound = false;
  bool archiveFutureReadyForCleanup = false;
  std::chrono::milliseconds archiveDeadlineElapsed{0};
  std::string archiveDeadlineError;
  std::size_t archiveRequestsObserved = 0;
  if (localPublishHoldsApiLock) {
    ScopedRequestPublisher archivePublisherGuard(
      environment.user(), archiveRequestPublisher);
    const auto archiveStartedAt = std::chrono::steady_clock::now();
    auto archiveFuture = std::async(std::launch::async, [&] {
      try {
        (void)archiveDeadlineApi.beginArchive(
          reopenedInApp, *localHandle, archivePlacement,
          "spec192-archive-source-lock-deadline", 150);
        return std::string("unexpected beginArchive success");
      }
      catch (const std::exception& error) {
        return std::string(error.what());
      }
    });
    const auto archiveObserveDeadline = archiveStartedAt + 500ms;
    while (archiveFuture.wait_for(0ms) != std::future_status::ready &&
           std::chrono::steady_clock::now() < archiveObserveDeadline) {
      std::this_thread::sleep_for(1ms);
    }
    archiveFutureReadyForCleanup =
      archiveFuture.wait_for(0ms) == std::future_status::ready;
    archiveReturnedWithinBound = archiveFutureReadyForCleanup &&
      std::chrono::steady_clock::now() <= archiveObserveDeadline;
    if (archiveFutureReadyForCleanup) {
      archiveDeadlineError = archiveFuture.get();
      archiveDeadlineElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - archiveStartedAt);
    }

    char* releaseLockError = nullptr;
    const int releaseLockResult = sqlite3_exec(
      archiveLockDatabase.get(), "ROLLBACK", nullptr, nullptr,
      &releaseLockError);
    const std::string releaseLockErrorText =
      releaseLockError == nullptr ? std::string{} : releaseLockError;
    sqlite3_free(releaseLockError);
    BOOST_CHECK_MESSAGE(releaseLockResult == SQLITE_OK,
                        "cannot release In-App SQLite writer lock: "
                          << releaseLockErrorText);
    const auto blockedLocalPublishError = blockedLocalPublishFuture.get();
    BOOST_CHECK_MESSAGE(blockedLocalPublishError.empty(),
                        "blocked In-App publish failed: " << blockedLocalPublishError);
    if (!archiveFutureReadyForCleanup) {
      archiveDeadlineError = archiveFuture.get();
      archiveDeadlineElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - archiveStartedAt);
    }
    archiveRequestsObserved = archiveRequestStarts.load(std::memory_order_relaxed);
  }
  else {
    char* releaseLockError = nullptr;
    const int releaseLockResult = sqlite3_exec(
      archiveLockDatabase.get(), "ROLLBACK", nullptr, nullptr,
      &releaseLockError);
    sqlite3_free(releaseLockError);
    BOOST_CHECK_EQUAL(releaseLockResult, SQLITE_OK);
    (void)blockedLocalPublishFuture.get();
  }
  BOOST_CHECK(localPublishHoldsApiLock);
  BOOST_CHECK(archiveReturnedWithinBound);
  BOOST_CHECK_MESSAGE(
    archiveDeadlineError.find("repo-operation-timeout: deadline expired waiting for In-App lookup operation") == 0,
    "beginArchive source lock error=" << archiveDeadlineError);
  BOOST_CHECK_LE(archiveDeadlineElapsed.count(), 500);
  BOOST_CHECK_EQUAL(archiveRequestsObserved, 0U);

  const auto archived = runWithFacePump(environment, [&] {
    return selectedWriteApi.archive(reopenedInApp, *localHandle,
                                    archivePlacement, 60000);
  });
  BOOST_REQUIRE(archived.handle);
  BOOST_REQUIRE(archived.receipt);
  BOOST_CHECK_EQUAL(archived.achievedReplicas, 1U);

  const auto destination = temporary.path() / "independent-fetch.bin";
  auto reloadedPersistent = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), clientConfig);
  const auto reloadedHandle = repo::ArtifactHandle::fromJson(archived.handle->toJson());
  runWithFacePump(environment, [&] {
    reloadedPersistent.fetchFile(reloadedHandle, destination.string(), 60000);
  });
  BOOST_CHECK(readFile(destination) == archivedBytes);
  for (const auto& target : archivePlacement.targets) {
    const auto replica = std::find_if(
      archived.receipt->replicaCommits.begin(), archived.receipt->replicaCommits.end(),
      [&target] (const auto& item) { return item.repoNode == target.repoNode; });
    BOOST_REQUIRE(replica != archived.receipt->replicaCommits.end());
    const auto payloadPath = committedPayloadPath(
      repoRoots.at(target.repoNode), archivedReference);
    if (replica->state == "COMMITTED") {
      ++expectedManifestCounts[target.repoNode];
      BOOST_CHECK_EQUAL(persistentReceiptRows(
        metadataDatabases.at(target.repoNode), target.repoNode,
        target.providerIdentity.toUri(), archivedReference), 1);
      BOOST_CHECK(readFile(payloadPath) == archivedBytes);
    }
    else {
      BOOST_CHECK_EQUAL(replica->state, "SKIPPED");
      BOOST_CHECK_EQUAL(persistentReceiptRows(
        metadataDatabases.at(target.repoNode), target.repoNode,
        target.providerIdentity.toUri(), archivedReference), 0);
      BOOST_CHECK(!fs::exists(payloadPath));
    }
    BOOST_CHECK_EQUAL(repoManifestCount(repoRoots.at(target.repoNode)),
                      expectedManifestCounts[target.repoNode]);
  }

  // Exercise live availability through the registered STATUS handler. The
  // artifact receipt remains immutable while the backing object changes.
  const std::vector<uint8_t> availabilityBytes{0x41, 0x56, 0x41, 0x49};
  const auto availabilityReference = makeReference(
    "availability-state-matrix", availabilityBytes);
  const auto availabilitySource = temporary.path() / "availability-state-matrix.bin";
  writeFile(availabilitySource, availabilityBytes);
  const auto availabilityPublished = runWithFacePump(environment, [&] {
    return selectedWriteApi.publishFile(availabilitySource.string(),
                                        availabilityReference, oneTarget, 60000);
  });
  BOOST_REQUIRE(availabilityPublished.receipt);
  BOOST_REQUIRE(availabilityPublished.handle);
  BOOST_REQUIRE(availabilityPublished.operationHandle);
  const auto availabilityReceiptId = availabilityPublished.receipt->receiptId;
  BOOST_REQUIRE_EQUAL(availabilityPublished.receipt->replicaCommits.size(), 1U);
  const auto availabilityLocalReceiptId =
    availabilityPublished.receipt->replicaCommits.front().receiptId;
  const auto availabilityReceipt = runWithFacePump(environment, [&] {
    return persistent.getReceipt(availabilityReceiptId,
                                 availabilityPublished.handle, 8000);
  });
  BOOST_CHECK_EQUAL(availabilityReceipt.toJson(),
                    availabilityPublished.receipt->toJson());

  auto availability = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 8000);
  });
  BOOST_REQUIRE_EQUAL(availability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(availability.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(availability.locations[0].objectPresence, "PRESENT");
  BOOST_CHECK_EQUAL(availability.locations[0].integrity, "NOT_CHECKED");
  const auto verifiedAvailability = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "verify_content", 8000);
  });
  BOOST_REQUIRE_EQUAL(verifiedAvailability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(verifiedAvailability.locations[0].integrity, "VERIFIED");

  const auto availabilityRepo = oneTarget.targets.front().repoNode;
  const auto availabilityProvider = oneTarget.targets.front().providerIdentity;
  const auto availabilityMetadata = metadataDatabases.at(availabilityRepo);
  size_t availabilityProviderIndex = environment.providerCount();
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    if (environment.provider(index).getName() == availabilityProvider) {
      availabilityProviderIndex = index;
      break;
    }
  }
  BOOST_REQUIRE(availabilityProviderIndex < environment.providerCount());

  // A valid signature from a different Provider must not satisfy LOCATION for
  // the selected Provider named by the response packet.
  const auto wrongSignerIndex = (availabilityProviderIndex + 1) % environment.providerCount();
  const auto wrongSignerIdentity = environment.provider(wrongSignerIndex).getName();
  BOOST_REQUIRE_NE(wrongSignerIdentity, availabilityProvider);
  std::atomic<bool> wrongSignerInjected{false};
  const auto statusService = repoService(servicePrefix, "STATUS");

  // T004 may retain selected replicas that were skipped or failed after the
  // seed commit. They stay in handle order, but generation zero has no current
  // LOCATION tuple and must not trigger a STATUS request.
  const auto skippedCandidate = std::find_if(
    discovered.candidates.begin(), discovered.candidates.end(),
    [&availabilityRepo] (const auto& candidate) {
      return candidate.repoNode != availabilityRepo;
    });
  BOOST_REQUIRE(skippedCandidate != discovered.candidates.end());
  repo::SelectedRepoTarget skippedTarget;
  skippedTarget.repoNode = skippedCandidate->repoNode;
  skippedTarget.providerIdentity = skippedCandidate->providerIdentity;
  skippedTarget.servicePrefix = skippedCandidate->servicePrefix;
  auto handleWithSkippedReplica = *availabilityPublished.handle;
  handleWithSkippedReplica.selectedTargets.push_back(skippedTarget);
  repo::ArtifactReplicaCommit skippedReplica;
  skippedReplica.repoNode = skippedTarget.repoNode;
  skippedReplica.providerIdentity = skippedTarget.providerIdentity.toUri();
  skippedReplica.operationId = handleWithSkippedReplica.replicaCommits.front().operationId;
  skippedReplica.reference = availabilityReference;
  skippedReplica.completedAtMs = handleWithSkippedReplica.replicaCommits.front().completedAtMs;
  skippedReplica.routeState = "UNKNOWN";
  skippedReplica.state = "SKIPPED";
  handleWithSkippedReplica.replicaCommits.push_back(skippedReplica);
  handleWithSkippedReplica.validate(artifactLimits);

  size_t skippedProviderIndex = environment.providerCount();
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    if (environment.provider(index).getName() == skippedTarget.providerIdentity) {
      skippedProviderIndex = index;
      break;
    }
  }
  BOOST_REQUIRE(skippedProviderIndex < environment.providerCount());
  std::atomic<size_t> skippedProviderStatusSelections{0};
  auto& skippedProviderPubSub = environment.providerPubSub(skippedProviderIndex);
  const auto skippedSelectionSubscription = skippedProviderPubSub.subscribeToProducer(
    environment.profile().userNode,
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      const auto selection = parseServiceSelectionNameV2(publication.name);
      if (selection && selection->providerName == skippedTarget.providerIdentity &&
          selection->serviceName == statusService) {
        skippedProviderStatusSelections.fetch_add(1, std::memory_order_acq_rel);
      }
    }, false);
  ProducerSubscriptionGuard skippedSelectionGuard(
    skippedProviderPubSub, skippedSelectionSubscription);
  const auto availabilityWithSkippedReplica = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(handleWithSkippedReplica, "metadata", 8000);
  });
  skippedSelectionGuard.reset();
  BOOST_CHECK_EQUAL(skippedProviderStatusSelections.load(std::memory_order_acquire), 0U);
  BOOST_REQUIRE_EQUAL(availabilityWithSkippedReplica.locations.size(), 2U);
  BOOST_CHECK_EQUAL(availabilityWithSkippedReplica.locations[0].repoNode,
                    availability.locations[0].repoNode);
  BOOST_CHECK_EQUAL(availabilityWithSkippedReplica.locations[0].objectPresence,
                    "PRESENT");
  BOOST_CHECK_EQUAL(availabilityWithSkippedReplica.locations[1].repoNode,
                    skippedTarget.repoNode);
  BOOST_CHECK_EQUAL(availabilityWithSkippedReplica.locations[1].providerIdentity,
                    skippedTarget.providerIdentity.toUri());
  BOOST_CHECK_EQUAL(availabilityWithSkippedReplica.locations[1].generation, 0U);
  BOOST_CHECK_EQUAL(availabilityWithSkippedReplica.locations[1].reachability,
                    "UNKNOWN");
  BOOST_CHECK_EQUAL(availabilityWithSkippedReplica.locations[1].objectPresence,
                    "UNKNOWN");
  BOOST_CHECK_EQUAL(availabilityWithSkippedReplica.locations[1].integrity,
                    "NOT_CHECKED");
  BOOST_CHECK(availabilityWithSkippedReplica.locations[1].operationId.empty());
  BOOST_CHECK(availabilityWithSkippedReplica.locations[1].receiptId.empty());
  BOOST_CHECK_EQUAL(availabilityWithSkippedReplica.locations[1].locationVersion, 0U);

  using RequestPublisher = ndn_service_framework::ServiceUser::RequestPublisher;
  const auto runWithInjectedResponses = [&] (RequestPublisher publisher, auto&& action) {
    ScopedRequestPublisher publisherGuard(environment.user(), std::move(publisher));
    return runWithFacePump(environment, std::forward<decltype(action)>(action));
  };
  const auto makeCurrentLocationResponse = [&] {
    const auto& replica = availabilityPublished.handle->replicaCommits.front();
    const auto& target = availabilityPublished.handle->selectedTargets.front();
    repo::ArtifactLocationRecord location;
    location.repoNode = target.repoNode;
    location.providerIdentity = target.providerIdentity.toUri();
    location.operationId = replica.operationId;
    location.receiptId = replica.receiptId;
    location.reference = availabilityReference;
    location.generation = replica.generation;
    location.locationVersion = replica.locationVersion;
    location.committedAtMs = replica.completedAtMs;
    location.expiresAtMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) + 60000;
    location.routeState = replica.routeState;
    location.forwardingHint = target.servicePrefix.toUri();
    const auto wire = location.toJson();
    return makeSyntheticResponse(ndn::Buffer(wire.begin(), wire.end()));
  };

  const RequestPublisher wrongSignerPublisher =
    [&] (const ndn::Name& requestId, const ndn::Name&,
         const std::vector<ndn::Name>&, const ndn::Name& serviceName,
         const ndn_service_framework::RequestMessage&, size_t) {
      if (serviceName != statusService) {
        return;
      }
      wrongSignerInjected.store(true, std::memory_order_release);
      const ndn::Buffer payload{'w', 'r', 'o', 'n', 'g'};
      queueSyntheticResponse(environment, wrongSignerIndex, availabilityProvider,
                             serviceName, requestId, makeSyntheticResponse(payload));
    };
  const auto wrongSignerAvailability = runWithInjectedResponses(
    wrongSignerPublisher, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 1800);
  });
  BOOST_CHECK(wrongSignerInjected.load(std::memory_order_acquire));
  BOOST_REQUIRE_EQUAL(wrongSignerAvailability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(wrongSignerAvailability.locations[0].reachability, "UNREACHABLE");
  BOOST_CHECK_EQUAL(wrongSignerAvailability.locations[0].objectPresence, "UNKNOWN");
  BOOST_CHECK_EQUAL(wrongSignerAvailability.locations[0].integrity, "NOT_CHECKED");
  BOOST_CHECK_EQUAL(wrongSignerAvailability.locations[0].locationVersion, 0U);

  // An authenticated response attributed to Provider B still cannot answer a
  // LOCATION request pinned to Provider A; payload contents cannot override
  // the response Provider identity delivered by ServiceUser.
  std::atomic<bool> wrongProviderLocationInjected{false};
  const RequestPublisher wrongProviderLocationPublisher =
    [&] (const ndn::Name& requestId, const ndn::Name&,
         const std::vector<ndn::Name>&, const ndn::Name& serviceName,
         const ndn_service_framework::RequestMessage&, size_t) {
      if (serviceName != statusService) {
        return;
      }
      wrongProviderLocationInjected.store(true, std::memory_order_release);
      const ndn::Buffer payload{'w', 'r', 'o', 'n', 'g'};
      queueAuthenticatedResponseForApiTest(
        environment, wrongSignerIndex, wrongSignerIdentity, serviceName,
        requestId, makeSyntheticResponse(payload));
    };
  const auto wrongProviderLocation = runWithInjectedResponses(
    wrongProviderLocationPublisher, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 5000);
  });
  BOOST_CHECK(wrongProviderLocationInjected.load(std::memory_order_acquire));
  BOOST_REQUIRE_EQUAL(wrongProviderLocation.locations.size(), 1U);
  BOOST_CHECK_EQUAL(wrongProviderLocation.locations[0].reachability, "UNKNOWN");
  BOOST_CHECK_EQUAL(wrongProviderLocation.locations[0].objectPresence, "UNKNOWN");
  BOOST_CHECK_EQUAL(wrongProviderLocation.locations[0].integrity, "NOT_CHECKED");
  BOOST_CHECK_EQUAL(wrongProviderLocation.locations[0].locationVersion, 0U);

  // Let a valid LOCATION from A pass, then deliver AVAILABILITY attributed to
  // B. The accepted LOCATION remains evidence that A is reachable, but the
  // mismatched probe cannot establish object presence.
  std::atomic<size_t> statusRequestCount{0};
  std::atomic<bool> wrongProviderAvailabilityInjected{false};
  const RequestPublisher wrongProviderAvailabilityPublisher =
    [&] (const ndn::Name& requestId, const ndn::Name&,
         const std::vector<ndn::Name>&, const ndn::Name& serviceName,
         const ndn_service_framework::RequestMessage&, size_t) {
      if (serviceName != statusService) {
        return;
      }
      const auto ordinal = statusRequestCount.fetch_add(1, std::memory_order_acq_rel);
      if (ordinal == 0) {
        queueAuthenticatedResponseForApiTest(
          environment, availabilityProviderIndex, availabilityProvider,
          serviceName, requestId, makeCurrentLocationResponse());
      }
      else if (ordinal == 1) {
        wrongProviderAvailabilityInjected.store(true, std::memory_order_release);
        const ndn::Buffer payload{'w', 'r', 'o', 'n', 'g'};
        queueAuthenticatedResponseForApiTest(
          environment, wrongSignerIndex, wrongSignerIdentity, serviceName,
          requestId, makeSyntheticResponse(payload));
      }
    };
  const auto wrongProviderAvailability = runWithInjectedResponses(
    wrongProviderAvailabilityPublisher, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 8000);
  });
  BOOST_CHECK(wrongProviderAvailabilityInjected.load(std::memory_order_acquire));
  BOOST_CHECK_EQUAL(statusRequestCount.load(std::memory_order_acquire), 2U);
  BOOST_REQUIRE_EQUAL(wrongProviderAvailability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(wrongProviderAvailability.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(wrongProviderAvailability.locations[0].objectPresence, "UNKNOWN");
  BOOST_CHECK_EQUAL(wrongProviderAvailability.locations[0].integrity, "NOT_CHECKED");
  BOOST_CHECK_GT(wrongProviderAvailability.locations[0].locationVersion, 0U);

  // Receipt lookup is pinned to the immutable seed Provider. A different
  // authorized Provider cannot answer by returning the seed's receipt payload.
  std::atomic<bool> wrongProviderReceiptInjected{false};
  const RequestPublisher wrongProviderReceiptPublisher =
    [&] (const ndn::Name& requestId, const ndn::Name&,
         const std::vector<ndn::Name>&, const ndn::Name& serviceName,
         const ndn_service_framework::RequestMessage&, size_t) {
      if (serviceName != statusService) {
        return;
      }
      wrongProviderReceiptInjected.store(true, std::memory_order_release);
      const ndn::Buffer payload{'w', 'r', 'o', 'n', 'g'};
      queueAuthenticatedResponseForApiTest(
        environment, wrongSignerIndex, wrongSignerIdentity, serviceName,
        requestId, makeSyntheticResponse(payload));
    };
  bool wrongProviderReceiptRejected = false;
  try {
    (void)runWithInjectedResponses(wrongProviderReceiptPublisher, [&] {
      return persistent.getReceipt(availabilityReceiptId,
                                   availabilityPublished.handle, 5000);
    });
  }
  catch (const std::runtime_error&) {
    wrongProviderReceiptRejected = true;
  }
  BOOST_CHECK(wrongProviderReceiptInjected.load(std::memory_order_acquire));
  BOOST_CHECK(wrongProviderReceiptRejected);

  // Exercise the same RepoNode write mutex without changing the published
  // artifact whose receipt and location tuple the availability probe checks.
  const auto statusRaceObject =
    repo::artifactStorageObjectName(availabilityReference) + "/status-race-stress";
  std::atomic<bool> stopConcurrentMutation{false};
  std::exception_ptr mutationError;
  std::thread concurrentMutation([&] {
    try {
      while (!stopConcurrentMutation.load(std::memory_order_acquire)) {
        nodes[availabilityProviderIndex]->put(
          statusRaceObject,
          availabilityBytes, "artifact-v2-bytes", 1,
          availabilityReference.policyEpoch);
        std::this_thread::sleep_for(1ms);
      }
    }
    catch (...) {
      mutationError = std::current_exception();
      stopConcurrentMutation.store(true, std::memory_order_release);
    }
  });
  std::vector<repo::AvailabilityReport> concurrentAvailability;
  std::exception_ptr availabilityError;
  try {
    for (size_t iteration = 0; iteration < 2; ++iteration) {
      concurrentAvailability.push_back(runWithFacePump(environment, [&] {
        return persistent.checkAvailability(*availabilityPublished.handle,
                                            "metadata", 8000);
      }));
    }
  }
  catch (...) {
    availabilityError = std::current_exception();
  }
  stopConcurrentMutation.store(true, std::memory_order_release);
  concurrentMutation.join();
  BOOST_REQUIRE(!mutationError);
  BOOST_REQUIRE(!availabilityError);
  for (const auto& concurrentReport : concurrentAvailability) {
    BOOST_REQUIRE_EQUAL(concurrentReport.locations.size(), 1U);
    BOOST_CHECK_EQUAL(concurrentReport.locations[0].reachability, "REACHABLE");
    BOOST_CHECK_EQUAL(concurrentReport.locations[0].objectPresence, "PRESENT");
  }

  // Persistent availability spends one deadline waiting for the same API
  // operation lock used by fetch. A slow in-flight fetch must not make the
  // availability or begin call wait beyond its own timeout or start new work.
  auto boundedBeginConfig = clientConfig;
  boundedBeginConfig.ackCollectionTimeMs = 10;
  boundedBeginConfig.requestTimeoutMs = 500;
  auto boundedBeginApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), boundedBeginConfig);
  const auto boundedStatusService = repoService(servicePrefix, "STATUS");
  std::atomic<size_t> boundedStatusStarts{0};
  std::atomic<size_t> boundedOtherRequestStarts{0};
  const RequestPublisher suppressStatusPublisher =
    [&] (const ndn::Name&, const ndn::Name&,
         const std::vector<ndn::Name>&, const ndn::Name& serviceName,
         const ndn_service_framework::RequestMessage&, size_t strategy) {
      (void)strategy;
      if (serviceName == boundedStatusService) {
        boundedStatusStarts.fetch_add(1, std::memory_order_acq_rel);
      }
      else {
        boundedOtherRequestStarts.fetch_add(1, std::memory_order_acq_rel);
      }
    };
  // RequestPublisher suppresses the initial request, so no Provider ACK can
  // arrive. This checks that begin stops after its first STATUS attempt; the
  // lock-contention checks below cover operation-deadline enforcement.
  const auto boundedBeginStartedAt = std::chrono::steady_clock::now();
  std::string boundedBeginError;
  try {
    (void)runWithInjectedResponses(suppressStatusPublisher, [&] {
      return boundedBeginApi.beginPublishFile(
        availabilitySource.string(), availabilityReference, oneTarget,
        "spec192-bounded-selected-request", 1800);
    });
  }
  catch (const std::exception& error) {
    boundedBeginError = error.what();
  }
  const auto boundedBeginElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - boundedBeginStartedAt);
  BOOST_CHECK_EQUAL(boundedStatusStarts.load(std::memory_order_acquire), 1U);
  BOOST_CHECK_EQUAL(boundedOtherRequestStarts.load(std::memory_order_acquire), 0U);
  BOOST_CHECK_MESSAGE(
    boundedBeginError.find("repo-ack-authentication-failed:") == 0,
    "suppressed STATUS request error=" << boundedBeginError);
  BOOST_CHECK_LE(boundedBeginElapsed.count(), 1200);

  auto availabilityLockConfig = fetchConfig;
  availabilityLockConfig.ackCollectionTimeMs = 500;
  availabilityLockConfig.requestTimeoutMs = 2500;
  auto availabilityLockApi = repo::ArtifactRepositoryApi::makePersistent(
    environment.user(), availabilityLockConfig);
  std::atomic<bool> lockHolderStarted{false};
  std::atomic<bool> firstLocationDelayed{false};
  std::atomic<bool> availabilityCallStarted{false};
  std::atomic<bool> beginCallStarted{false};
  std::atomic<bool> statusCallStarted{false};
  std::atomic<bool> nearAckStatusCallStarted{false};
  std::chrono::steady_clock::time_point lockHolderFirstStatusStartedAt{};
  std::atomic<size_t> availabilityStatusStarts{0};
  std::atomic<size_t> availabilityProbeStarts{0};
  std::atomic<size_t> beginRequestStarts{0};
  std::atomic<size_t> statusRequestStarts{0};
  std::atomic<size_t> nearAckStatusRequestStarts{0};
  auto lockPublisher = makeFetchPublisher(fetchHandle, quorumBytes);
  std::thread delayedLocationResponse;
  ScopedThreadJoin delayedLocationJoin(delayedLocationResponse);
  FetchRequestPublisher delayedLockPublisher =
    [&, lockPublisher = std::move(lockPublisher)]
    (const ndn::Name& requestId, const ndn::Name& requester,
     const std::vector<ndn::Name>& providers, const ndn::Name& serviceName,
     const ndn_service_framework::RequestMessage& requestMessage,
     size_t timeoutMs) mutable {
      if (beginCallStarted.load(std::memory_order_acquire)) {
        beginRequestStarts.fetch_add(1, std::memory_order_relaxed);
      }
      if (statusCallStarted.load(std::memory_order_acquire)) {
        statusRequestStarts.fetch_add(1, std::memory_order_relaxed);
      }
      if (nearAckStatusCallStarted.load(std::memory_order_acquire) &&
          serviceName == fetchStatusService) {
        const auto payload = requestMessage.getPayload();
        const std::string requestWire(payload.begin(), payload.end());
        if (requestWire.find(
              availabilityPublished.operationHandle->operationId) != std::string::npos) {
          nearAckStatusRequestStarts.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (serviceName == fetchStatusService) {
        if (availabilityCallStarted.load(std::memory_order_acquire)) {
          availabilityStatusStarts.fetch_add(1, std::memory_order_relaxed);
        }
        const auto payload = requestMessage.getPayload();
        const std::string requestWire(payload.begin(), payload.end());
        if (requestWire.find("\"expectedLocation\"") != std::string::npos) {
          availabilityProbeStarts.fetch_add(1, std::memory_order_relaxed);
        }
        else if (!firstLocationDelayed.exchange(true, std::memory_order_acq_rel)) {
          lockHolderFirstStatusStartedAt = std::chrono::steady_clock::now();
          auto delayedRequestMessage = requestMessage;
          auto delayedResponder = lockPublisher;
          delayedLocationResponse = std::thread(
            [delayedResponder = std::move(delayedResponder), requestId, requester,
             providers, serviceName, delayedRequestMessage = std::move(delayedRequestMessage),
             timeoutMs] () mutable {
              std::this_thread::sleep_for(1800ms);
              delayedResponder(requestId, requester, providers, serviceName,
                               delayedRequestMessage, timeoutMs);
            });
          lockHolderStarted.store(true, std::memory_order_release);
          return;
        }
      }
      lockPublisher(requestId, requester, providers, serviceName,
                    requestMessage, timeoutMs);
    };
  std::future<bool> lockHolderFuture;
  std::future<repo::AvailabilityReport> lockDeadlineFuture;
  std::future<std::string> beginLockDeadlineFuture;
  std::future<std::string> statusLockDeadlineFuture;
  std::future<std::string> nearAckStatusDeadlineFuture;
  std::promise<void> nearAckStatusTimeoutPromise;
  auto nearAckStatusTimeoutGate = nearAckStatusTimeoutPromise.get_future();
  int nearAckStatusTimeoutBudgetMs = 0;
  bool nearAckStatusSetupReady = false;
  bool lockHolderStartedInTime = false;
  bool availabilityFinishedWithinBound = false;
  bool beginFinishedWithinBound = false;
  bool statusFinishedWithinBound = false;
  bool nearAckStatusFinishedWithinBound = false;
  std::chrono::milliseconds availabilityLockElapsed{0};
  std::chrono::milliseconds beginLockElapsed{0};
  std::chrono::milliseconds statusLockElapsed{0};
  constexpr int availabilityLockDeadlineToleranceMs = 250;
  std::chrono::steady_clock::time_point availabilityStartedAt{};
  std::chrono::steady_clock::time_point availabilityFinishedAt{};
  std::chrono::steady_clock::time_point beginStartedAt{};
  std::chrono::steady_clock::time_point beginFinishedAt{};
  std::chrono::steady_clock::time_point statusStartedAt{};
  std::chrono::steady_clock::time_point statusFinishedAt{};
  std::string nearAckStatusError;
  repo::AvailabilityReport lockTimeoutReport;
  {
    ScopedRequestPublisher publisherGuard(environment.user(),
      std::move(delayedLockPublisher));
    lockHolderFuture = std::async(std::launch::async, [&] {
      try {
        availabilityLockApi.fetchFile(fetchHandle,
          (temporary.path() / "availability-operation-lock-fetch.bin").string(),
          5000);
        return true;
      }
      catch (...) {
        return false;
      }
    });
    environment.pumpUntilWithAttributeAuthority([&] {
      return lockHolderStarted.load(std::memory_order_acquire) ||
             lockHolderFuture.wait_for(0ms) == std::future_status::ready;
    });
    lockHolderStartedInTime = lockHolderStarted.load(std::memory_order_acquire);
    if (lockHolderStartedInTime) {
      lockDeadlineFuture = std::async(std::launch::async, [&] {
        availabilityCallStarted.store(true, std::memory_order_release);
        availabilityStartedAt = std::chrono::steady_clock::now();
        auto result = availabilityLockApi.checkAvailability(
          fetchHandle, "metadata", 1200);
        availabilityFinishedAt = std::chrono::steady_clock::now();
        return result;
      });
      beginLockDeadlineFuture = std::async(std::launch::async, [&] {
        beginCallStarted.store(true, std::memory_order_release);
        beginStartedAt = std::chrono::steady_clock::now();
        try {
          (void)availabilityLockApi.beginPublishFile(
            availabilitySource.string(), availabilityReference, oneTarget,
            "spec192-begin-operation-lock-timeout", 1200);
          beginFinishedAt = std::chrono::steady_clock::now();
          return std::string{};
        }
        catch (const std::exception& error) {
          beginFinishedAt = std::chrono::steady_clock::now();
          return std::string(error.what());
        }
      });
      statusLockDeadlineFuture = std::async(std::launch::async, [&] {
        statusCallStarted.store(true, std::memory_order_release);
        statusStartedAt = std::chrono::steady_clock::now();
        try {
          (void)availabilityLockApi.getPublishStatus(
            *availabilityPublished.operationHandle, 1200);
          statusFinishedAt = std::chrono::steady_clock::now();
          return std::string{};
        }
        catch (const std::exception& error) {
          statusFinishedAt = std::chrono::steady_clock::now();
          return std::string(error.what());
        }
      });
      // The delayed lock holder releases 1.8 seconds after the first STATUS.
      // Set the absolute deadline 400ms later so the remaining request budget
      // is positive but still below this caller's 500ms ACK collection window.
      const auto nearAckDeadline = lockHolderFirstStatusStartedAt + 2200ms;
      nearAckStatusDeadlineFuture = std::async(
        std::launch::async,
        [&, nearAckStatusTimeoutGate = std::move(nearAckStatusTimeoutGate)]() mutable {
          // Arm only after the shorter lock contenders have completed so their
          // STATUS requests cannot be misattributed to this near-ACK caller.
          nearAckStatusTimeoutGate.get();
          if (!nearAckStatusSetupReady) {
            return std::string(
              "near-ACK status probe skipped because earlier calls are still active");
          }
          // Compute against the fixed lock-holder deadline after waking. Passing
          // a main-thread remainder would extend this API call's own deadline
          // by however long this worker waited to be scheduled.
          const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            nearAckDeadline - std::chrono::steady_clock::now()).count();
          nearAckStatusTimeoutBudgetMs = static_cast<int>(
            std::max<int64_t>(0, remainingMs));
          if (nearAckStatusTimeoutBudgetMs <=
              availabilityLockConfig.ackCollectionTimeMs) {
            return std::string(
              "near-ACK status probe missed its bounded setup window");
          }
          nearAckStatusCallStarted.store(true, std::memory_order_release);
          try {
            (void)availabilityLockApi.getPublishStatus(
              *availabilityPublished.operationHandle,
              nearAckStatusTimeoutBudgetMs);
            return std::string{};
          }
          catch (const std::exception& error) {
            return std::string(error.what());
          }
        });
      const auto contentionDeadline = std::chrono::steady_clock::now() +
        1200ms + std::chrono::milliseconds(availabilityLockDeadlineToleranceMs);
      availabilityFinishedWithinBound =
        lockDeadlineFuture.wait_until(contentionDeadline) ==
        std::future_status::ready;
      beginFinishedWithinBound =
        beginLockDeadlineFuture.wait_until(contentionDeadline) ==
        std::future_status::ready;
      statusFinishedWithinBound =
        statusLockDeadlineFuture.wait_until(contentionDeadline) ==
        std::future_status::ready;
      const auto earlierContentionCallsReady = [&] {
        return lockDeadlineFuture.wait_for(0ms) == std::future_status::ready &&
               beginLockDeadlineFuture.wait_for(0ms) == std::future_status::ready &&
               statusLockDeadlineFuture.wait_for(0ms) == std::future_status::ready;
      };
      for (int attempt = 0; attempt < 5 && !earlierContentionCallsReady(); ++attempt) {
        // pumpUntilWithAttributeAuthority is bounded to 200 face-pump rounds;
        // repeat it if that bound elapsed before every contender completed.
        environment.pumpUntilWithAttributeAuthority([&] {
          return earlierContentionCallsReady();
        });
      }
      nearAckStatusSetupReady = earlierContentionCallsReady();
      BOOST_CHECK_MESSAGE(
        nearAckStatusSetupReady,
        "short API lock contenders did not finish before near-ACK attribution");
      availabilityCallStarted.store(false, std::memory_order_release);
      beginCallStarted.store(false, std::memory_order_release);
      statusCallStarted.store(false, std::memory_order_release);
      nearAckStatusTimeoutPromise.set_value();
      const auto allLockCallsReady = [&] {
        return lockHolderFuture.wait_for(0ms) == std::future_status::ready &&
               lockDeadlineFuture.wait_for(0ms) == std::future_status::ready &&
               beginLockDeadlineFuture.wait_for(0ms) == std::future_status::ready &&
               statusLockDeadlineFuture.wait_for(0ms) == std::future_status::ready &&
               nearAckStatusDeadlineFuture.wait_for(0ms) == std::future_status::ready;
      };
      const auto nearAckFinishDeadline = nearAckDeadline +
        std::chrono::milliseconds(availabilityLockDeadlineToleranceMs);
      const auto allCallsReadyOrNearAckBoundExpired = [&] {
        const bool nearAckReady = nearAckStatusDeadlineFuture.wait_for(0ms) ==
                                  std::future_status::ready;
        if (nearAckReady && std::chrono::steady_clock::now() <= nearAckFinishDeadline) {
          nearAckStatusFinishedWithinBound = true;
        }
        return allLockCallsReady() ||
               std::chrono::steady_clock::now() >= nearAckFinishDeadline;
      };
      while (!allCallsReadyOrNearAckBoundExpired()) {
        environment.pumpUntilWithAttributeAuthority(
          allCallsReadyOrNearAckBoundExpired);
        if (!allCallsReadyOrNearAckBoundExpired()) {
          std::this_thread::sleep_for(1ms);
        }
      }
      // Continue pumping after recording a missed bound so every async caller
      // can unwind before the test fixture and its Faces are destroyed.
      while (!allLockCallsReady()) {
        environment.pumpUntilWithAttributeAuthority(allLockCallsReady);
        if (!allLockCallsReady()) {
          std::this_thread::sleep_for(1ms);
        }
      }
      BOOST_CHECK_MESSAGE(allLockCallsReady(),
                          "bounded API lock contention calls did not finish");
      lockTimeoutReport = lockDeadlineFuture.get();
      availabilityLockElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        availabilityFinishedAt - availabilityStartedAt);
      const auto beginLockError = beginLockDeadlineFuture.get();
      beginLockElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        beginFinishedAt - beginStartedAt);
      BOOST_CHECK_MESSAGE(
        beginLockError.find("repo-operation-timeout:") == 0 &&
        beginLockError.find("waiting for API operation") != std::string::npos,
        "begin API lock error=" << beginLockError);
      const auto statusLockError = statusLockDeadlineFuture.get();
      statusLockElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        statusFinishedAt - statusStartedAt);
      nearAckStatusError = nearAckStatusDeadlineFuture.get();
      BOOST_CHECK_MESSAGE(
        statusLockError.find("repo-operation-timeout:") == 0 &&
          statusLockError.find("waiting for API operation") != std::string::npos,
        "status API lock error=" << statusLockError);
    }
    else {
      environment.pumpUntilWithAttributeAuthority([&] {
        return lockHolderFuture.wait_for(0ms) == std::future_status::ready;
      });
    }
    if (delayedLocationResponse.joinable()) {
      delayedLocationResponse.join();
    }
  }
  BOOST_CHECK(lockHolderStartedInTime);
  BOOST_CHECK(lockHolderFuture.get());
  BOOST_CHECK(availabilityFinishedWithinBound);
  BOOST_CHECK(beginFinishedWithinBound);
  BOOST_CHECK(statusFinishedWithinBound);
  BOOST_CHECK_LE(availabilityLockElapsed.count(),
                 1200 + availabilityLockDeadlineToleranceMs);
  BOOST_CHECK_LE(beginLockElapsed.count(),
                 1200 + availabilityLockDeadlineToleranceMs);
  BOOST_CHECK_LE(statusLockElapsed.count(),
                 1200 + availabilityLockDeadlineToleranceMs);
  BOOST_CHECK_EQUAL(availabilityStatusStarts.load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EQUAL(availabilityProbeStarts.load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EQUAL(beginRequestStarts.load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EQUAL(statusRequestStarts.load(std::memory_order_relaxed), 0U);
  BOOST_CHECK(nearAckStatusFinishedWithinBound);
  BOOST_CHECK_GT(nearAckStatusTimeoutBudgetMs,
                 availabilityLockConfig.ackCollectionTimeMs);
  BOOST_CHECK_MESSAGE(
    nearAckStatusError.find(
      "repo-operation-timeout: status deadline leaves insufficient ACK window after API lock wait") == 0,
    "near-ACK status lock error=" << nearAckStatusError);
  BOOST_CHECK_EQUAL(nearAckStatusRequestStarts.load(std::memory_order_relaxed), 0U);
  BOOST_REQUIRE_EQUAL(lockTimeoutReport.locations.size(),
                      fetchHandle.replicaCommits.size());
  for (const auto& location : lockTimeoutReport.locations) {
    BOOST_CHECK_EQUAL(location.reachability, "UNKNOWN");
    BOOST_CHECK_EQUAL(location.objectPresence, "UNKNOWN");
    BOOST_CHECK_EQUAL(location.integrity, "NOT_CHECKED");
    BOOST_CHECK(location.reason.find("waiting for API operation lock") !=
                std::string::npos);
  }

  const auto originalLocation = currentLocationRecord(
    availabilityMetadata, availabilityReference, availabilityRepo,
    availabilityProvider.toUri());
  const auto nowMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  BOOST_REQUIRE_GT(nowMs, originalLocation.committedAtMs + 1);
  const auto expiredAtMs = nowMs - 1;
  mutateCurrentLocation(availabilityMetadata, availabilityReference,
    availabilityRepo, availabilityProvider.toUri(),
    "UPDATE artifact_locations SET expires_at_ms=? WHERE repo_node=? AND provider_identity=? AND logical_name=? AND digest_algorithm=? AND content_digest=? AND size_bytes=? AND format_version=? AND root_manifest_name=? AND publisher_identity=? AND policy_epoch=?",
    [expiredAtMs] (sqlite3_stmt* statement, int index) {
      return sqlite3_bind_int64(statement, index,
                                static_cast<sqlite3_int64>(expiredAtMs));
    });
  const auto expiredAvailability = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 8000);
  });
  BOOST_REQUIRE_EQUAL(expiredAvailability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(expiredAvailability.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(expiredAvailability.locations[0].objectPresence, "UNKNOWN");
  BOOST_CHECK_EQUAL(expiredAvailability.locations[0].providerIdentity,
                    availabilityProvider.toUri());
  mutateCurrentLocation(availabilityMetadata, availabilityReference,
    availabilityRepo, availabilityProvider.toUri(),
    "UPDATE artifact_locations SET expires_at_ms=? WHERE repo_node=? AND provider_identity=? AND logical_name=? AND digest_algorithm=? AND content_digest=? AND size_bytes=? AND format_version=? AND root_manifest_name=? AND publisher_identity=? AND policy_epoch=?",
    [&originalLocation] (sqlite3_stmt* statement, int index) {
      return sqlite3_bind_int64(statement, index,
        static_cast<sqlite3_int64>(originalLocation.expiresAtMs));
    });

  BOOST_REQUIRE(selectedResult.receipt);
  BOOST_REQUIRE_EQUAL(selectedResult.receipt->replicaCommits.size(), 1U);
  const auto wrongReceiptId = selectedResult.receipt->replicaCommits.front().receiptId;
  BOOST_REQUIRE_NE(wrongReceiptId, availabilityLocalReceiptId);
  mutateCurrentLocation(availabilityMetadata, availabilityReference,
    availabilityRepo, availabilityProvider.toUri(),
    "UPDATE artifact_locations SET receipt_id=? WHERE repo_node=? AND provider_identity=? AND logical_name=? AND digest_algorithm=? AND content_digest=? AND size_bytes=? AND format_version=? AND root_manifest_name=? AND publisher_identity=? AND policy_epoch=?",
    [&wrongReceiptId] (sqlite3_stmt* statement, int index) {
      return sqlite3_bind_text(statement, index, wrongReceiptId.c_str(), -1,
                               SQLITE_TRANSIENT);
    });
  const auto mismatchedReceiptAvailability = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 8000);
  });
  BOOST_REQUIRE_EQUAL(mismatchedReceiptAvailability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(mismatchedReceiptAvailability.locations[0].objectPresence,
                    "UNKNOWN");
  BOOST_CHECK_EQUAL(mismatchedReceiptAvailability.locations[0].providerIdentity,
                    availabilityProvider.toUri());
  mutateCurrentLocation(availabilityMetadata, availabilityReference,
    availabilityRepo, availabilityProvider.toUri(),
    "UPDATE artifact_locations SET receipt_id=? WHERE repo_node=? AND provider_identity=? AND logical_name=? AND digest_algorithm=? AND content_digest=? AND size_bytes=? AND format_version=? AND root_manifest_name=? AND publisher_identity=? AND policy_epoch=?",
    [&availabilityLocalReceiptId] (sqlite3_stmt* statement, int index) {
      return sqlite3_bind_text(statement, index, availabilityLocalReceiptId.c_str(), -1,
                               SQLITE_TRANSIENT);
    });

  const auto availabilityObject = committedPayloadPath(
    repoRoots.at(availabilityRepo), availabilityReference);
  std::vector<uint8_t> corruptedBytes = availabilityBytes;
  corruptedBytes.front() ^= 0xff;
  writeFile(availabilityObject, corruptedBytes);
  const auto failedIntegrity = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "verify_content", 8000);
  });
  BOOST_REQUIRE_EQUAL(failedIntegrity.locations.size(), 1U);
  BOOST_CHECK_EQUAL(failedIntegrity.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(failedIntegrity.locations[0].objectPresence, "PRESENT");
  BOOST_CHECK_EQUAL(failedIntegrity.locations[0].integrity, "FAILED");

  fs::resize_file(availabilityObject, 1);
  const auto partialAvailability = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 8000);
  });
  BOOST_REQUIRE_EQUAL(partialAvailability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(partialAvailability.locations[0].objectPresence, "PARTIAL");
  BOOST_CHECK_EQUAL(partialAvailability.locations[0].integrity, "NOT_CHECKED");

  BOOST_REQUIRE(nodes[availabilityProviderIndex]->core().remove(
    repo::artifactStorageObjectName(availabilityReference)));
  const auto absentAvailability = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 8000);
  });
  BOOST_REQUIRE_EQUAL(absentAvailability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(absentAvailability.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(absentAvailability.locations[0].objectPresence, "ABSENT");
  BOOST_CHECK_EQUAL(absentAvailability.locations[0].integrity, "NOT_CHECKED");
  const auto immutableReceiptAfterMutation = runWithFacePump(environment, [&] {
    return persistent.getReceipt(availabilityReceiptId,
                                 availabilityPublished.handle, 8000);
  });
  BOOST_CHECK_EQUAL(immutableReceiptAfterMutation.toJson(),
                    availabilityReceipt.toJson());

  // Simulate a caller losing the seed commit response: the seed commits its
  // receipt and ACCEPTED state, but the caller discards that response. Once a
  // selected replica is unavailable, replaying the retained key must recover
  // the seed-owned operation before requiring fresh discovery of that replica.
  const auto staleReplayReplica = std::find_if(
    discovered.candidates.begin(), discovered.candidates.end(),
    [&oneTarget] (const auto& candidate) {
      return candidate.repoNode != oneTarget.targets.front().repoNode;
    });
  BOOST_REQUIRE(staleReplayReplica != discovered.candidates.end());
  repo::SelectedRepoSet staleReplayPlacement = oneTarget;
  staleReplayPlacement.targets.push_back({
    staleReplayReplica->repoNode, staleReplayReplica->providerIdentity,
    staleReplayReplica->servicePrefix});
  staleReplayPlacement.consistency = repo::RepoWriteConsistency::One;
  // Keep the direct seed commit within the fixture's single-STORE chunk cap.
  const std::vector<uint8_t> staleReplayBytes{0x72, 0x65, 0x73};
  const auto staleReplayReference = makeReference(
    "accepted-stale-replica-replay", staleReplayBytes);
  BOOST_REQUIRE_EQUAL(staleReplayReference.sizeBytes, staleReplayBytes.size());
  BOOST_REQUIRE(staleReplayBytes.size() <= artifactLimits.maxChunkBytes);
  const auto staleReplaySource = temporary.path() / "accepted-stale-replica.bin";
  writeFile(staleReplaySource, staleReplayBytes);
  const std::string staleReplayKey = "spec192-stale-replay-key";
  const repo::RepoArtifactTarget staleReplaySeed{
    oneTarget.targets.front().repoNode,
    oneTarget.targets.front().providerIdentity.toUri(),
    oneTarget.targets.front().servicePrefix.toUri()};
  const repo::RepoArtifactTarget staleReplayReplicaTarget{
    staleReplayReplica->repoNode,
    staleReplayReplica->providerIdentity.toUri(),
    staleReplayReplica->servicePrefix.toUri()};
  repo::PublishOperationIntent staleReplayIntent;
  staleReplayIntent.idempotencyKeyDigest =
    repo::digestPublishIdempotencyKey(staleReplayKey);
  staleReplayIntent.reference = staleReplayReference;
  staleReplayIntent.generation = 1;
  staleReplayIntent.seed = staleReplaySeed;
  staleReplayIntent.consistency = "ONE";
  staleReplayIntent.targets = {staleReplaySeed, staleReplayReplicaTarget};
  std::sort(staleReplayIntent.targets.begin(), staleReplayIntent.targets.end(),
    [] (const auto& lhs, const auto& rhs) {
      if (lhs.repoNode != rhs.repoNode) return lhs.repoNode < rhs.repoNode;
      if (lhs.providerIdentity != rhs.providerIdentity)
        return lhs.providerIdentity < rhs.providerIdentity;
      return lhs.servicePrefix < rhs.servicePrefix;
    });
  staleReplayIntent.requestDigest = repo::makePublishRequestDigest(
    staleReplayIntent.reference, staleReplayIntent.generation,
    staleReplayIntent.seed, staleReplayIntent.consistency,
    staleReplayIntent.targets);
  const auto staleReplayOperationId = repo::makePublishOperationId(
    environment.user().getName().toUri(), staleReplaySeed,
    staleReplayIntent.idempotencyKeyDigest);
  size_t staleReplaySeedIndex = environment.providerCount();
  size_t staleReplayReplicaIndex = environment.providerCount();
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    const auto identity = environment.provider(index).getName();
    if (identity == oneTarget.targets.front().providerIdentity)
      staleReplaySeedIndex = index;
    if (identity == staleReplayReplica->providerIdentity)
      staleReplayReplicaIndex = index;
  }
  BOOST_REQUIRE(staleReplaySeedIndex < environment.providerCount());
  BOOST_REQUIRE(staleReplayReplicaIndex < environment.providerCount());
  const auto staleReplayStoreService = repoService(servicePrefix, "STORE");
  const auto staleReplayIntentJson = staleReplayIntent.toJson();
  const std::vector<uint8_t> staleReplayIntentWire(
    staleReplayIntentJson.begin(), staleReplayIntentJson.end());
  const auto staleReplayIntentResponse = dispatchRepoRequest(
    environment.provider(staleReplaySeedIndex), environment.user().getName(),
    staleReplayStoreService, staleReplayIntentWire,
    "lost-seed-response-intent");
  BOOST_REQUIRE_MESSAGE(staleReplayIntentResponse.getStatus(),
                        staleReplayIntentResponse.getErrorInfo());
  repo::ArtifactStoreRequest staleReplayStore;
  staleReplayStore.operationId = staleReplayOperationId;
  staleReplayStore.generation = staleReplayIntent.generation;
  staleReplayStore.reference = staleReplayReference;
  staleReplayStore.bytes = staleReplayBytes;
  const auto staleReplayStoreWire = staleReplayStore.toWire(artifactLimits);
  (void)dispatchRepoRequest(
    environment.provider(staleReplaySeedIndex), environment.user().getName(),
    staleReplayStoreService, staleReplayStoreWire,
    "lost-seed-response-commit-discarded");

  repo::PublishOperationHandle staleReplayHandle{
    1, staleReplayOperationId, staleReplaySeed};
  const auto staleReplayBeforeDisconnect = waitForTerminalPublish(
    environment, selectedWriteApi, staleReplayHandle);
  BOOST_CHECK_EQUAL(staleReplayBeforeDisconnect.state, "COMMITTED");
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    metadataDatabases.at(staleReplaySeed.repoNode), staleReplaySeed.repoNode,
    staleReplaySeed.providerIdentity, staleReplayReference), 1);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    metadataDatabases.at(staleReplayReplicaTarget.repoNode),
    staleReplayReplicaTarget.repoNode, staleReplayReplicaTarget.providerIdentity,
    staleReplayReference), 0);
  environment.disconnectProviderTransportForTest(staleReplayReplicaIndex);
  const auto staleReplayRecovered = runWithFacePump(environment, [&] {
    return selectedWriteApi.beginPublishFile(
      staleReplaySource.string(), staleReplayReference, staleReplayPlacement,
      staleReplayKey, 60000);
  });
  BOOST_CHECK_EQUAL(staleReplayRecovered.toJson(), staleReplayHandle.toJson());
  const auto staleReplayRecoveredStatus = runWithFacePump(environment, [&] {
    return selectedWriteApi.getPublishStatus(staleReplayRecovered, 8000);
  });
  BOOST_CHECK_EQUAL(staleReplayRecoveredStatus.receiptId,
                    staleReplayBeforeDisconnect.receiptId);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    metadataDatabases.at(staleReplaySeed.repoNode), staleReplaySeed.repoNode,
    staleReplaySeed.providerIdentity, staleReplayReference), 1);
  BOOST_CHECK_EQUAL(persistentReceiptRows(
    metadataDatabases.at(staleReplayReplicaTarget.repoNode),
    staleReplayReplicaTarget.repoNode, staleReplayReplicaTarget.providerIdentity,
    staleReplayReference), 0);

  std::atomic<size_t> availabilityTimeoutStatusRequestCount{0};
  std::atomic<bool> availabilityResponseSuppressed{false};
  const auto timeoutStatusService = repoService(servicePrefix, "STATUS");
  const RequestPublisher availabilityTimeoutPublisher =
    [&] (const ndn::Name& requestId, const ndn::Name&,
         const std::vector<ndn::Name>&, const ndn::Name& serviceName,
         const ndn_service_framework::RequestMessage&, size_t) {
      if (serviceName != timeoutStatusService) {
        return;
      }
      const auto ordinal = availabilityTimeoutStatusRequestCount.fetch_add(
        1, std::memory_order_acq_rel);
      if (ordinal == 0) {
        queueAuthenticatedResponseForApiTest(
          environment, availabilityProviderIndex, availabilityProvider,
          serviceName, requestId, makeCurrentLocationResponse());
      }
      else if (ordinal == 1) {
        // Preserve the authenticated LOCATION, but withhold AVAILABILITY so
        // ServiceUser takes the ordinary request-timeout path.
        availabilityResponseSuppressed.store(true, std::memory_order_release);
      }
    };
  const auto timedOutAvailability = runWithInjectedResponses(
    availabilityTimeoutPublisher, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 5000);
  });
  BOOST_CHECK(availabilityResponseSuppressed.load(std::memory_order_acquire));
  BOOST_CHECK_EQUAL(availabilityTimeoutStatusRequestCount.load(
                      std::memory_order_acquire), 2U);
  BOOST_REQUIRE_EQUAL(timedOutAvailability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(timedOutAvailability.locations[0].reachability, "REACHABLE");
  BOOST_CHECK_EQUAL(timedOutAvailability.locations[0].objectPresence, "UNKNOWN");
  BOOST_CHECK_EQUAL(timedOutAvailability.locations[0].integrity, "NOT_CHECKED");
  BOOST_CHECK_GT(timedOutAvailability.locations[0].locationVersion, 0U);

  size_t selectedProviderIndex = environment.providerCount();
  const auto selectedProvider = oneTarget.targets.front().providerIdentity;
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    if (environment.provider(index).getName() == selectedProvider) {
      selectedProviderIndex = index;
      break;
    }
  }
  BOOST_REQUIRE(selectedProviderIndex < environment.providerCount());
  environment.disconnectProviderTransportForTest(selectedProviderIndex);
  environment.disconnectProviderPeerTransportForTest();
  const std::vector<uint8_t> unavailableBytes{0x75, 0x6e, 0x61};
  const auto unavailableReference = makeReference(
    "unavailable-selected-seed", unavailableBytes);
  const auto unavailablePath = temporary.path() / "unavailable-selected-seed.bin";
  writeFile(unavailablePath, unavailableBytes);
  BOOST_CHECK_EXCEPTION(
    runWithFacePump(environment, [&] {
      return persistent.beginPublishFile(
        unavailablePath.string(), unavailableReference, oneTarget,
        "spec192-unavailable-seed-key", 8000);
    }),
    std::exception,
    [] (const std::exception& error) {
      const std::string message = error.what();
      return message.find("repo-selected-target-stale:") != std::string::npos ||
             message.find("repo-ack-authentication-failed:") != std::string::npos ||
             message.find("repo-request-timeout:") != std::string::npos ||
             message.find("repo-request-start-failed:") != std::string::npos;
    });
  for (const auto& candidate : discovered.candidates) {
    BOOST_CHECK_EQUAL(persistentReceiptRows(
      metadataDatabases.at(candidate.repoNode), candidate.repoNode,
      candidate.providerIdentity.toUri(), unavailableReference), 0);
    BOOST_CHECK(!fs::exists(committedPayloadPath(
      repoRoots.at(candidate.repoNode), unavailableReference)));
  }
  const auto offlineAvailability = runWithFacePump(environment, [&] {
    return persistent.checkAvailability(*availabilityPublished.handle,
                                        "metadata", 1500);
  });
  BOOST_REQUIRE_EQUAL(offlineAvailability.locations.size(), 1U);
  BOOST_CHECK_EQUAL(offlineAvailability.locations[0].reachability, "UNREACHABLE");
  BOOST_CHECK_EQUAL(offlineAvailability.locations[0].objectPresence, "UNKNOWN");
  BOOST_CHECK_EQUAL(offlineAvailability.locations[0].integrity, "NOT_CHECKED");
}

BOOST_AUTO_TEST_CASE(PersistentApiFetchHealthStateSerializesConcurrentCalls)
{
  TempRoot temporary;
  const ndn::Name servicePrefix(repo::RepoClient::DEFAULT_SERVICE_NAME);
  BootstrapProfile profile;
  profile.groupPrefix = "/ndnsf/spec192/fetch-health/group";
  profile.syncPrefix = "/ndnsf/spec192/fetch-health/sync";
  profile.userNode = "/ndnsf/spec192/fetch-health/user";
  profile.providerNode = "/ndnsf/spec192/fetch-health/provider";
  profile.userIdentity = "/test/user/spec192/fetch-health";
  profile.providerIdentity = "/test/provider/spec192/fetch-health";
  profile.attributeAuthority = "/test/aa/spec192/fetch-health";
  profile.providerCount = 2;
  profile.providerCanActAsUser = true;
  const std::vector<std::string> operations{"STATUS", "FETCH_RANGE"};
  profile.serviceName = repoService(servicePrefix, operations.front());
  profile.additionalServices.push_back(repoService(servicePrefix, operations.back()));

  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() == EnvironmentStatus::Ready);
  std::vector<std::shared_ptr<ndn_service_framework::ServiceUser>> publisherUsers;
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    publisherUsers.push_back(makeRepoPublisherUser(
      environment, index, servicePrefix, operations));
  }
  environment.enableProductionIngressForTest();
  environment.pumpUntilWithAttributeAuthority([&] {
    return std::all_of(publisherUsers.begin(), publisherUsers.end(), [] (const auto& user) {
      return user->isNacConsumerReadyForTest() && user->isNacProducerReadyForTest();
    });
  });

  const std::vector<uint8_t> bytes{0x73, 0x70, 0x65, 0x63, 0x31, 0x39, 0x32};
  repo::ArtifactHandle handle;
  handle.reference = makeReference("fetch-health-concurrency", bytes);
  handle.lifecycleMode = "PERSISTENT";
  handle.seedRepoId = "/logical/spec192/fetch-health/repo-0";
  handle.writeConsistency = "ALL";
  const auto now = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    const auto repoNode = "/logical/spec192/fetch-health/repo-" +
                          std::to_string(index);
    const auto receiptId = "spec192-fetch-health-receipt-" +
                           std::to_string(index);
    const auto operationId = "spec192-fetch-health-operation-" +
                             std::to_string(index);
    const auto providerIdentity = environment.provider(index).getName();
    handle.commitReceiptIds.push_back(receiptId);
    handle.selectedTargets.push_back({repoNode, providerIdentity, servicePrefix});

    repo::ArtifactReplicaCommit replica;
    replica.repoNode = repoNode;
    replica.providerIdentity = providerIdentity.toUri();
    replica.receiptId = receiptId;
    replica.operationId = operationId;
    replica.reference = handle.reference;
    replica.generation = 1;
    replica.contentDigest = handle.reference.contentDigest;
    replica.persistedBytes = handle.reference.sizeBytes;
    replica.completedAtMs = now;
    replica.locationVersion = 1;
    replica.routeState = "ROUTABLE";
    replica.state = "COMMITTED";
    replica.forwardingHint = servicePrefix.toUri();
    handle.replicaCommits.push_back(std::move(replica));
  }
  handle.validate();

  repo::PersistentRepoClientConfig config;
  config.servicePrefix = servicePrefix;
  config.ackCollectionTimeMs = 10;
  config.requestTimeoutMs = 120;
  config.fetchFailureCooldownMs = 0;
  config.maxFetchHealthEntries = 2;
  auto api = repo::ArtifactRepositoryApi::makePersistent(environment.user(), config);

  using FetchRequestPublisher = ndn_service_framework::ServiceUser::RequestPublisher;
  const auto statusService = repoService(servicePrefix, "STATUS");
  auto firstRequestStarted = std::make_shared<std::atomic<bool>>(false);
  auto firstRequestSignal = std::make_shared<std::promise<void>>();
  auto statusRequestCount = std::make_shared<std::atomic<size_t>>(0);
  const FetchRequestPublisher noResponsePublisher{
    [firstRequestStarted, firstRequestSignal, statusRequestCount, statusService]
    (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
     const ndn::Name& serviceName,
     const ndn_service_framework::RequestMessage&, size_t) {
      if (serviceName != statusService) return;
      statusRequestCount->fetch_add(1, std::memory_order_acq_rel);
      if (!firstRequestStarted->exchange(true, std::memory_order_acq_rel)) {
        firstRequestSignal->set_value();
      }
    }};
  ScopedRequestPublisher publisherGuard(environment.user(), noResponsePublisher);

  const auto fetchFailure = [&] (const std::string& path) {
    try {
      (void)api.fetchFile(handle, path, 2000);
      throw std::runtime_error("Spec192 no-response fetch unexpectedly succeeded");
    }
    catch (const repo::ArtifactFetchError& error) {
      return error.result();
    }
  };
  const auto concurrentResults = runWithFacePump(environment, [&] {
    auto firstCall = std::async(std::launch::async, [&] {
      return fetchFailure((temporary.path() / "fetch-health-first.bin").string());
    });
    auto requestStarted = firstRequestSignal->get_future();
    if (requestStarted.wait_for(1s) != std::future_status::ready) {
      throw std::runtime_error("Spec192 first health-state fetch did not issue LOCATION");
    }
    auto secondCall = std::async(std::launch::async, [&] {
      return fetchFailure((temporary.path() / "fetch-health-second.bin").string());
    });
    return std::make_pair(firstCall.get(), secondCall.get());
  });
  BOOST_REQUIRE_EQUAL(concurrentResults.first.attempts.size(), 2U);
  BOOST_REQUIRE_EQUAL(concurrentResults.second.attempts.size(), 2U);
  BOOST_CHECK_EQUAL(statusRequestCount->load(std::memory_order_acquire), 4U);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
