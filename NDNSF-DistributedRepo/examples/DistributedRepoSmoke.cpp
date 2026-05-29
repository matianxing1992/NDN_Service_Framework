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

  std::cout << "DISTRIBUTED_REPO_SMOKE_OK "
            << toString(manifestResponse) << "\n";
  return 0;
}
