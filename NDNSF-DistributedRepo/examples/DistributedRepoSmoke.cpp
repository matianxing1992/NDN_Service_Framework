#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/segmenter.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

int
main()
{
  using namespace ndnsf_distributed_repo;

  ndn::KeyChain keyChain;
  ndn::Name signerIdentity("/example/repo/user/smoke");
  signerIdentity.appendNumber(static_cast<uint64_t>(getpid()));
  const auto signingIdentity = keyChain.createIdentity(signerIdentity);

  const std::vector<uint8_t> payload = {'n', 'd', 'n', 's', 'f', '-', 'r', 'e', 'p', 'o'};
  std::vector<StorageCapability> candidates = {
    {"/repo/A", 1024 * 1024, 0, 0.10, 0.99, "rack-a", {"model", "intermediate"}},
    {"/repo/B", 512 * 1024, 0, 0.05, 0.98, "rack-b", {"model"}},
    {"/repo/C", 256, 0, 0.01, 1.00, "rack-c", {"intermediate"}},
  };
  StorageCapability localOnlyRepo;
  localOnlyRepo.repoNode = "/repo/in-app-local-only";
  localOnlyRepo.freeBytes = 4 * 1024 * 1024;
  localOnlyRepo.availabilityScore = 1.0;
  localOnlyRepo.repoMode = "in-app";
  localOnlyRepo.acceptsBackupReplica = false;
  candidates.push_back(localOnlyRepo);

  PlacementPolicy policy;
  policy.replicationFactor = 2;

  const auto replicas = selectReplicas(candidates, policy, payload.size());
  if (replicas.size() != 2) {
    std::cerr << "expected two replicas, got " << replicas.size() << "\n";
    return 1;
  }
  for (const auto& replica : replicas) {
    if (replica.repoNode == localOnlyRepo.repoNode) {
      std::cerr << "in-app local-only repo must not be selected as backup replica\n";
      return 1;
    }
  }

  RepoNode node(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
                {"/repo/A", 1024 * 1024, 0, 0.10, 0.99, "rack-a",
                 {"model", "intermediate"}});
  StorageCapability persistentModeProbe;
  persistentModeProbe.repoNode = "/repo/A";
  persistentModeProbe.repoMode = "persistent";
  StorageCapability inAppModeProbe;
  inAppModeProbe.repoNode = "/repo/in-app";
  inAppModeProbe.repoMode = "in-app";
  inAppModeProbe.acceptsBackupReplica = false;
  if (!isPersistentRepo(persistentModeProbe) ||
      !isInAppRepo(inAppModeProbe)) {
    std::cerr << "repo mode helpers mismatch\n";
    return 1;
  }
  StoreOptions options;
  options.objectType = "model";
  options.replicationFactor = policy.replicationFactor;
  options.policyEpoch = "/Policy/demo/v1";
  for (const auto& replica : replicas) {
    options.replicaNodes.push_back(replica.repoNode);
  }
  const auto manifest = RepoClient::put(node,
                                        "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/"
                                        "NDNSF-DI/ARTIFACT/demo/object",
                                        payload,
                                        options);
  const auto fetched = RepoClient::get(node, manifest.objectName);
  const auto manifestResponse = node.handleManifest(toBytes(manifest.objectName));
  const auto capabilityResponse = node.handleCapability();
  const auto listed = RepoClient::list(node);
  const auto catalogStatus = RepoClient::catalogStatus(node);
  const auto catalogSnapshot = RepoClient::catalogSnapshot(node);
  const auto catalogLookup = RepoClient::catalogLookup(node, manifest.objectName);
  const bool removed = RepoClient::remove(node, manifest.objectName);
  const auto catalogDeltaAfterDelete = RepoClient::catalogDelta(node,
                                                                catalogStatus.catalogEpoch);

  if (fetched != payload) {
    std::cerr << "stored object not found\n";
    return 1;
  }
  if (toString(manifestResponse).find(manifest.objectName) == std::string::npos ||
      listed.empty() || listed.front().objectName != manifest.objectName ||
      toString(capabilityResponse).find("/repo/A") == std::string::npos ||
      catalogStatus.repoNode != "/repo/A" ||
      catalogSnapshot.entries.empty() ||
      catalogLookup.manifest.objectName != manifest.objectName ||
      catalogDeltaAfterDelete.entries.empty() ||
      catalogDeltaAfterDelete.entries.back().state != "DELETED" ||
      !removed) {
    std::cerr << "repo node response mismatch\n";
    return 1;
  }

  const auto clientManifest = RepoClient::makeManifest(
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/"
    "NDNSF-DI/ARTIFACT/demo/client-object",
    "model",
    payload,
    policy.replicationFactor,
    {"/repo/A", "/repo/B"},
    "/Policy/demo/v1");
  if (clientManifest.sha256 != manifest.sha256) {
    std::cerr << "client manifest hash mismatch\n";
    return 1;
  }

  std::vector<uint8_t> largePayload;
  for (int i = 0; i < 37; ++i) {
    largePayload.push_back(static_cast<uint8_t>('A' + (i % 26)));
  }
  StoreOptions segmentedOptions;
  segmentedOptions.objectType = "large-model";
  segmentedOptions.replicationFactor = 1;
  segmentedOptions.replicaNodes = {"/repo/A"};
  segmentedOptions.policyEpoch = "/Policy/demo/v1";
  const auto segmentedManifest = RepoClient::putSegmented(
    node,
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/large/direct-object",
    largePayload,
    segmentedOptions,
    8);
  if (segmentedManifest.segmentCount <= 1 ||
      RepoClient::getSegmented(node, segmentedManifest) != largePayload ||
      RepoClient::getObject(node, segmentedManifest) != largePayload ||
      node.getManifest(segmentedManifest.objectName).segmentCount !=
        segmentedManifest.segmentCount) {
    std::cerr << "direct segmented repo object mismatch\n";
    return 1;
  }

  StoreOptions insertPayloadOptions;
  insertPayloadOptions.objectType = "signed-payload-data";
  const auto insertPayloadStatus = RepoClient::insertPayload(
    node,
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/app-owned/payload-api",
    largePayload,
    keyChain,
    ndn::security::SigningInfo(signingIdentity),
    insertPayloadOptions,
    8);
  const auto insertPayloadManifest = node.getManifest(
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/app-owned/payload-api");
  if (insertPayloadStatus.state != "DONE" ||
      insertPayloadStatus.completedSegments <= 1 ||
      insertPayloadManifest.segmentCount != insertPayloadStatus.completedSegments ||
      insertPayloadManifest.packetNames.size() != insertPayloadStatus.completedSegments ||
      node.getDataPacket(insertPayloadManifest.packetNames.front()).empty()) {
    std::cerr << "repo payload insert did not produce signed Data segments\n";
    return 1;
  }

  RepoDataReference noFetcherReference;
  noFetcherReference.objectName =
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/app-owned/no-fetcher";
  noFetcherReference.dataPrefix =
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/UPLOAD/DATA/no-fetcher";
  noFetcherReference.hasFinalSegment = true;
  noFetcherReference.finalSegment = 1;
  const auto noFetcherStatus = RepoClient::insert(node, noFetcherReference);
  if (noFetcherStatus.state != "FAILED" ||
      noFetcherStatus.message.find("SegmentFetcher") == std::string::npos) {
    std::cerr << "repo store-from-reference missing fetcher did not fail visibly\n";
    return 1;
  }

  const std::string fetchedDataPrefix =
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/UPLOAD/DATA/model";
  ndn::Segmenter segmenter(keyChain, ndn::security::SigningInfo(signingIdentity));
  const auto fetchedData = segmenter.segment(
    ndn::span<const uint8_t>(largePayload.data(), largePayload.size()),
    ndn::Name(fetchedDataPrefix),
    8,
    ndn::time::hours(1));
  std::vector<std::vector<uint8_t>> fakeWirePackets;
  for (const auto& data : fetchedData) {
    const auto wire = data->wireEncode();
    fakeWirePackets.emplace_back(wire.begin(), wire.end());
  }
  std::vector<uint8_t> fakeConcatenated;
  for (const auto& packet : fakeWirePackets) {
    fakeConcatenated.insert(fakeConcatenated.end(), packet.begin(), packet.end());
  }
  node.setDataReferenceFetcher([&] (const RepoDataReference& reference) {
    if (reference.dataPrefix != fetchedDataPrefix) {
      throw std::runtime_error("unexpected fake data prefix");
    }
    return fakeWirePackets;
  });
  RepoDataReference reference;
  reference.objectName =
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/app-owned/model";
  reference.dataPrefix =
    fetchedDataPrefix;
  reference.hasFinalSegment = true;
  reference.finalSegment = 1;
  reference.expectedSize = fakeConcatenated.size();
  reference.expectedSha256 = sha256Hex(fakeConcatenated);
  reference.objectType = "app-owned-segmented-data";
  const auto referenceStatus = RepoClient::insert(node, reference);
  const auto queriedReferenceStatus = RepoClient::status(node, referenceStatus.operationId);
  const auto referenceManifest = node.getManifest(reference.objectName);
  if (referenceStatus.state != "DONE" ||
      queriedReferenceStatus.state != "DONE" ||
      queriedReferenceStatus.completedSegments != fakeWirePackets.size() ||
      referenceManifest.segmentCount != fakeWirePackets.size() ||
      referenceManifest.packetNames.size() != fakeWirePackets.size() ||
      node.getDataPacket(referenceManifest.packetNames[0]) != fakeWirePackets[0] ||
      node.getDataPacket(referenceManifest.packetNames[1]) != fakeWirePackets[1]) {
    std::cerr << "repo store-from-reference wire packet path mismatch\n";
    return 1;
  }

  RepoCore embeddedCore({"/repo/embedded", 1024 * 1024, 0, 0.0, 1.0,
                         "local", {"embedded"}});
  const auto embeddedManifest = embeddedCore.put(
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/local/core-object",
    payload,
    "embedded-object");
  if (embeddedCore.get(embeddedManifest.objectName) != payload ||
      embeddedCore.getManifest(embeddedManifest.objectName).sha256 !=
        embeddedManifest.sha256 ||
      embeddedCore.list().empty()) {
    std::cerr << "repo core direct API mismatch\n";
    return 1;
  }

  ndn_service_framework::LocalServiceRegistry localRegistry;
  RepoNode embeddedNode(
    ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
    {"/repo/in-app", 1024 * 1024, 0, 0.0, 1.0, "local", {"embedded"}});
  embeddedNode.registerLocalServices(localRegistry);
  if (!localRegistry.hasService(
        makeRepoServiceName(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), "STORE")) ||
      !localRegistry.hasService(
        makeRepoServiceName(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), "CATALOG_STATUS")) ||
      !localRegistry.hasService(
        makeRepoServiceName(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), "CACHE_STATUS"))) {
    std::cerr << "embedded repo block local services not registered\n";
    return 1;
  }
  const auto localManifest = RepoClient::localPut(
    localRegistry,
    ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/local/block-object",
    payload,
    segmentedOptions);
  const auto localCatalogStatus = RepoClient::localCatalogStatus(
    localRegistry, ndn::Name(RepoClient::DEFAULT_SERVICE_NAME));
  const auto localCacheStatus = RepoClient::localCacheStatus(
    localRegistry, ndn::Name(RepoClient::DEFAULT_SERVICE_NAME));
  const auto localCatalogLookup = RepoClient::localCatalogLookup(
    localRegistry, ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
    localManifest.objectName);
  if (RepoClient::localGet(localRegistry,
                           ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
                           localManifest.objectName) != payload ||
      RepoClient::localGetManifest(localRegistry,
                                   ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
                                   localManifest.objectName).sha256 != localManifest.sha256 ||
      RepoClient::localList(localRegistry,
                            ndn::Name(RepoClient::DEFAULT_SERVICE_NAME)).empty() ||
      localCatalogStatus.repoNode != "/repo/in-app" ||
      localCacheStatus.storageBackend != "memory" ||
      localCatalogLookup.manifest.objectName != localManifest.objectName ||
      !RepoClient::localRemove(localRegistry,
                               ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
                               localManifest.objectName)) {
    std::cerr << "embedded repo block local API mismatch\n";
    return 1;
  }

  const auto sqlitePath = std::filesystem::temp_directory_path() /
    "ndnsf-distributed-repo-smoke.sqlite3";
  std::filesystem::remove(sqlitePath);
  std::filesystem::remove(sqlitePath.string() + "-wal");
  std::filesystem::remove(sqlitePath.string() + "-shm");
  const std::string persistentObjectName =
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/local/persistent-object";
  {
    RepoCore persistentCore(
      {"/repo/persistent", 1024 * 1024, 0, 0.0, 1.0, "local", {"persistent"}},
      makeTieredRepoStore(sqlitePath.string(), 4096));
    persistentCore.put(persistentObjectName, payload, "persistent-object");
    if (persistentCore.cacheStatus().authoritativeBackend != "sqlite" ||
        persistentCore.cacheStatus().entryCount != 1) {
      std::cerr << "tiered repo initial cache status mismatch\n";
      return 1;
    }
  }
  {
    RepoCore restartedCore(
      {"/repo/persistent", 1024 * 1024, 0, 0.0, 1.0, "local", {"persistent"}},
      makeTieredRepoStore(sqlitePath.string(), 4096));
    if (restartedCore.get(persistentObjectName) != payload ||
        restartedCore.getManifest(persistentObjectName).objectType !=
          "persistent-object" ||
        restartedCore.cacheStatus().misses != 1 ||
        restartedCore.cacheStatus().hits != 1) {
      std::cerr << "sqlite repo restart fetch mismatch\n";
      return 1;
    }
  }

  std::cout << "DISTRIBUTED_REPO_SMOKE_OK "
            << toString(manifestResponse) << "\n";
  return 0;
}
