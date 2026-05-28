#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <iostream>
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

  const auto replicas = selectReplicas(candidates, policy, payload.size());
  if (replicas.size() != 2) {
    std::cerr << "expected two replicas, got " << replicas.size() << "\n";
    return 1;
  }

  RepoObjectManifest manifest;
  manifest.objectName = "/NDNSF-DI/ARTIFACT/demo/object";
  manifest.objectType = "model";
  manifest.sha256 = sha256Hex(payload);
  manifest.size = payload.size();
  manifest.replicationFactor = policy.replicationFactor;
  manifest.policyEpoch = "/Policy/demo/v1";
  for (const auto& replica : replicas) {
    manifest.replicaNodes.push_back(replica.repoNode);
  }

  RepoNode node(ndn::Name("/NDNSF/DistributedRepo"),
                {"/repo/A", 1024 * 1024, 0, 0.10, 0.99, "rack-a",
                 {"model", "intermediate"}});
  const auto storeRequest = encodeStoreRequest(manifest, payload);
  const auto storeResponse = node.handleStore(storeRequest);
  const auto fetched = node.handleFetch(toBytes(manifest.objectName));
  const auto manifestResponse = node.handleManifest(toBytes(manifest.objectName));
  const auto inventoryResponse = node.handleInventory();
  const auto capabilityResponse = node.handleCapability();
  const auto deleteResponse = node.handleDelete(toBytes(manifest.objectName));

  if (fetched != payload) {
    std::cerr << "stored object not found\n";
    return 1;
  }
  if (toString(storeResponse).find(manifest.sha256) == std::string::npos ||
      toString(manifestResponse).find(manifest.objectName) == std::string::npos ||
      toString(inventoryResponse).find(manifest.objectName) == std::string::npos ||
      toString(capabilityResponse).find("/repo/A") == std::string::npos ||
      toString(deleteResponse) != "deleted") {
    std::cerr << "repo node response mismatch\n";
    return 1;
  }

  const auto clientManifest = RepoClient::makeManifest(
    "/NDNSF-DI/ARTIFACT/demo/client-object",
    "model",
    payload,
    policy.replicationFactor,
    {"/repo/A", "/repo/B"},
    "/Policy/demo/v1");
  if (clientManifest.sha256 != manifest.sha256) {
    std::cerr << "client manifest hash mismatch\n";
    return 1;
  }

  std::cout << "DISTRIBUTED_REPO_SMOKE_OK "
            << toString(manifestResponse) << "\n";
  return 0;
}
