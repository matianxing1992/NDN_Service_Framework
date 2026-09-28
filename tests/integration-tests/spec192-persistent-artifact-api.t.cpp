#include "tests/boost-test.hpp"
#include "ndnsf-integration-fixture.hpp"

#include "ndnsf-distributed-repo/ArtifactRepositoryApi.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"

#include <openssl/sha.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedRepoPersistentArtifactApi)

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
    node->registerServices(environment.provider(index));
    nodes.push_back(std::move(node));
    repoRoots.emplace(logicalRepo, objectRoot);
    metadataDatabases.emplace(logicalRepo, metadataPath);
    expectedManifestCounts.emplace(logicalRepo, 0);
  }
  environment.enableProductionIngressForTest();

  repo::PersistentRepoClientConfig clientConfig;
  clientConfig.servicePrefix = servicePrefix;
  clientConfig.ackCollectionTimeMs = 1000;
  clientConfig.requestTimeoutMs = 8000;
  clientConfig.limits = artifactLimits;
  auto persistent = repo::ArtifactRepositoryApi::makePersistent(environment.user(), clientConfig);

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
    return persistent.discoverRepoCandidates(requirements, 8000);
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
      return persistent.publishFile(sourcePath.string(), reference, placement, 60000);
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
        BOOST_CHECK_EQUAL(replica->state, "SKIPPED");
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
    return persistent.publishFile(selectedPath.string(), selectedReference,
                                  oneTarget, 60000);
  });
  BOOST_REQUIRE(selectedResult.receipt);
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
    return persistent.publishFile(selectedPath.string(), selectedReference,
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
    const bool wasSelected =
      candidate.repoNode == oneTarget.targets.front().repoNode ||
      candidate.repoNode == otherCandidate->repoNode;
    const auto payloadPath = committedPayloadPath(
      repoRoots.at(candidate.repoNode), selectedReference);
    const auto rowCount = persistentReceiptRows(
      metadataDatabases.at(candidate.repoNode), candidate.repoNode,
      candidate.providerIdentity.toUri(), selectedReference);
    BOOST_CHECK_EQUAL(rowCount, wasSelected ? 1 : 0);
    BOOST_CHECK_EQUAL(fs::exists(payloadPath), wasSelected);
    if (wasSelected) {
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
    return persistent.publishFile(quorumPath.string(), quorumReference,
                                  quorumTargets, 60000);
  });
  const auto permutedQuorumResult = runWithFacePump(environment, [&] {
    return persistent.publishFile(quorumPath.string(), quorumReference,
                                  permutedQuorumTargets, 60000);
  });
  BOOST_REQUIRE(quorumResult.receipt);
  BOOST_REQUIRE(permutedQuorumResult.receipt);
  BOOST_REQUIRE(quorumResult.handle);
  BOOST_REQUIRE(permutedQuorumResult.handle);
  BOOST_CHECK_EQUAL(quorumResult.operationId, permutedQuorumResult.operationId);
  BOOST_CHECK_EQUAL(quorumResult.receipt->receiptId,
                    permutedQuorumResult.receipt->receiptId);
  BOOST_CHECK_EQUAL(quorumResult.handle->toJson(),
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
  BOOST_CHECK(firstCommittedRepos ==
              committedRepos(*permutedQuorumResult.receipt));
  for (const auto& repoNode : firstCommittedRepos) {
    ++expectedManifestCounts[repoNode];
  }
  for (const auto& candidate : discovered.candidates) {
    const bool wasCommitted = firstCommittedRepos.count(candidate.repoNode) != 0;
    const auto payloadPath = committedPayloadPath(
      repoRoots.at(candidate.repoNode), quorumReference);
    const auto rowCount = persistentReceiptRows(
      metadataDatabases.at(candidate.repoNode), candidate.repoNode,
      candidate.providerIdentity.toUri(), quorumReference);
    BOOST_CHECK_EQUAL(rowCount, wasCommitted ? 1 : 0);
    BOOST_CHECK_EQUAL(fs::exists(payloadPath), wasCommitted);
    if (wasCommitted) {
      BOOST_CHECK(readFile(payloadPath) == quorumBytes);
    }
    BOOST_CHECK_EQUAL(repoManifestCount(repoRoots.at(candidate.repoNode)),
                      expectedManifestCounts[candidate.repoNode]);
  }

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
  const auto archived = runWithFacePump(environment, [&] {
    return persistent.archive(reopenedInApp, *localHandle, archivePlacement, 60000);
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
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
