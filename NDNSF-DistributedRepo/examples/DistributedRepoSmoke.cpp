#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include "ndn-service-framework/LocalServiceRegistry.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int
main()
{
  using namespace ndnsf_distributed_repo;

  const std::vector<uint8_t> payload = {'n', 'd', 'n', 's', 'f', '-', 'r', 'e', 'p', 'o'};
  const std::vector<StorageCapability> candidates = {
    {"/repo/A", 1024 * 1024, 0, 0.10, 0.99, "rack-a", {"model", "intermediate"}},
    {"/repo/B", 512 * 1024, 0, 0.05, 0.98, "rack-b", {"model"}},
    {"/repo/C", 256, 0, 0.01, 1.00, "rack-c", {"intermediate"}},
  };

  PlacementPolicy policy;
  policy.replicationFactor = 2;

  if (parseRepoDeploymentMode("") != RepoDeploymentMode::Remote ||
      parseRepoDeploymentMode("embedded") != RepoDeploymentMode::Embedded ||
      parseRepoDeploymentMode("local") != RepoDeploymentMode::Embedded ||
      parseRepoDeploymentMode("both") != RepoDeploymentMode::Both ||
      !enablesRemote(RepoDeploymentMode::Both) ||
      !enablesEmbedded(RepoDeploymentMode::Both) ||
      toString(RepoDeploymentMode::Embedded) != "embedded") {
    std::cerr << "repo deployment mode helpers mismatch\n";
    return 1;
  }

  const auto replicas = selectReplicas(candidates, policy, payload.size());
  if (replicas.size() != 2) {
    std::cerr << "expected two replicas, got " << replicas.size() << "\n";
    return 1;
  }

  RepoNode node(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
                {"/repo/A", 1024 * 1024, 0, 0.10, 0.99, "rack-a",
                 {"model", "intermediate"}});
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
  const bool removed = RepoClient::remove(node, manifest.objectName);

  if (fetched != payload) {
    std::cerr << "stored object not found\n";
    return 1;
  }
  if (toString(manifestResponse).find(manifest.objectName) == std::string::npos ||
      listed.empty() || listed.front().objectName != manifest.objectName ||
      toString(capabilityResponse).find("/repo/A") == std::string::npos ||
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
      makeSqliteRepoStore(sqlitePath.string()));
    persistentCore.put(persistentObjectName, payload, "persistent-object");
  }
  {
    RepoCore restartedCore(
      {"/repo/persistent", 1024 * 1024, 0, 0.0, 1.0, "local", {"persistent"}},
      makeSqliteRepoStore(sqlitePath.string()));
    if (restartedCore.get(persistentObjectName) != payload ||
        restartedCore.getManifest(persistentObjectName).objectType !=
          "persistent-object") {
      std::cerr << "sqlite repo restart fetch mismatch\n";
      return 1;
    }
  }

  RepoNode embeddedNode(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
                        {"/repo/embedded-node", 1024 * 1024, 0, 0.0, 1.0,
                         "local", {"embedded"}});
  ndn_service_framework::LocalServiceRegistry localRegistry;
  embeddedNode.registerDeploymentServices(nullptr,
                                          &localRegistry,
                                          RepoDeploymentMode::Embedded);
  StoreOptions localOptions;
  localOptions.objectType = "embedded-object";
  localOptions.replicationFactor = 1;
  localOptions.replicaNodes = {"/repo/embedded-node"};
  localOptions.policyEpoch = "/Policy/demo/v1";
  const auto localManifest = RepoClient::localPut(
    localRegistry,
    ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/local/local-service-object",
    payload,
    localOptions);
  const auto localFetchedPayload = RepoClient::localGet(
    localRegistry, ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
    localManifest.objectName);
  const auto localManifestAgain = RepoClient::localGetManifest(
    localRegistry, ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
    localManifest.objectName);
  const auto localInventory = RepoClient::localList(
    localRegistry, ndn::Name(RepoClient::DEFAULT_SERVICE_NAME));
  const auto localRemoved = RepoClient::localRemove(
    localRegistry, ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
    localManifest.objectName);
  if (localFetchedPayload != payload ||
      localManifestAgain.sha256 != localManifest.sha256 ||
      localInventory.empty() ||
      localInventory.front().objectName != localManifest.objectName ||
      !localRemoved) {
    std::cerr << "repo local service invocation mismatch\n";
    return 1;
  }
  const auto localSegmentedManifest = RepoClient::localPutSegmented(
    localRegistry,
    ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
    "/example/repo/user/NDNSF-DISTRIBUTED-REPO/OBJECT/local/segmented-object",
    largePayload,
    localOptions,
    7);
  if (localSegmentedManifest.segmentCount <= 1 ||
      RepoClient::localGetSegmented(
        localRegistry,
        ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
        localSegmentedManifest) != largePayload ||
      RepoClient::localGetObject(
        localRegistry,
        ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
        localSegmentedManifest) != largePayload) {
    std::cerr << "repo local segmented invocation mismatch\n";
    return 1;
  }

  auto appVerifiedManifest = localSegmentedManifest;
  appVerifiedManifest.sha256 = std::string(64, '0');
  try {
    (void)RepoClient::localGetSegmented(
      localRegistry,
      ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
      appVerifiedManifest);
    std::cerr << "repo segmented APP-side hash verification did not fail\n";
    return 1;
  }
  catch (const std::runtime_error&) {
  }

  ndn_service_framework::LocalServiceRegistry bothRegistry;
  RepoNode bothNode(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME),
                    {"/repo/both-node", 1024 * 1024, 0, 0.0, 1.0,
                     "local", {"embedded"}});
  try {
    bothNode.registerDeploymentServices(nullptr, &bothRegistry, RepoDeploymentMode::Both);
    std::cerr << "both mode accepted missing ServiceProvider\n";
    return 1;
  }
  catch (const std::invalid_argument&) {
  }
  try {
    bothNode.registerDeploymentServices(nullptr, nullptr, RepoDeploymentMode::Embedded);
    std::cerr << "embedded mode accepted missing LocalServiceRegistry\n";
    return 1;
  }
  catch (const std::invalid_argument&) {
  }
  bothNode.registerDeploymentServices(nullptr, &bothRegistry, RepoDeploymentMode::Embedded);
  if (!bothRegistry.hasService(
        makeRepoServiceName(ndn::Name(RepoClient::DEFAULT_SERVICE_NAME), "STORE"))) {
    std::cerr << "embedded mode did not register local STORE\n";
    return 1;
  }

  std::cout << "DISTRIBUTED_REPO_SMOKE_OK "
            << toString(manifestResponse) << "\n";
  return 0;
}
