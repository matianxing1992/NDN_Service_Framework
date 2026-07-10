#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace ndnsf_distributed_repo;

namespace {

void
require(bool condition, const std::string& message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

} // namespace

int
main()
{
  require(requiredWriteAcks(3, RepoWriteConsistency::One) == 1,
          "ONE acknowledgement threshold mismatch");
  require(requiredWriteAcks(3, RepoWriteConsistency::Quorum) == 2,
          "QUORUM acknowledgement threshold mismatch");
  require(requiredWriteAcks(3, RepoWriteConsistency::All) == 3,
          "ALL acknowledgement threshold mismatch");
  require(normalizeRepoOperationState("committed") == "COMMITTED",
          "operation state normalization mismatch");

  RepoObjectManifest manifest;
  manifest.objectName = "/publisher/versioned";
  manifest.objectType = "mutable-alias";
  manifest.sha256 = std::string(64, 'a');
  manifest.size = 42;
  manifest.replicationFactor = 3;
  manifest.replicaNodes = {"/repo/A", "/repo/B", "/repo/C"};
  manifest.generation = 7;
  manifest.parentGeneration = 6;
  manifest.writeConsistency = "QUORUM";
  manifest.requiredWriteAcks = 2;
  manifest.confirmedReplicaNodes = {"/repo/A", "/repo/C"};
  manifest.operationId = "op-7";

  const auto parsed = parseManifestJson(manifest.toJson());
  require(parsed.objectName == manifest.objectName, "manifest object name mismatch");
  require(parsed.generation == 7 && parsed.parentGeneration == 6,
          "manifest generation mismatch");
  require(parsed.writeConsistency == "QUORUM" && parsed.requiredWriteAcks == 2,
          "manifest consistency mismatch");
  require(parsed.confirmedReplicaNodes == manifest.confirmedReplicaNodes,
          "manifest confirmed replica mismatch");
  require(parsed.operationId == manifest.operationId,
          "manifest operation ID mismatch");

  const auto legacy = parseManifestJson(
    "{\"objectName\":\"/legacy\",\"objectType\":\"artifact\","
    "\"sha256\":\"aa\",\"size\":1,\"replicationFactor\":2,"
    "\"replicaNodes\":[\"/repo/A\",\"/repo/B\"]}");
  require(legacy.generation == 0 && legacy.parentGeneration == -1,
          "legacy generation defaults mismatch");
  require(legacy.requiredWriteAcks == 2,
          "legacy acknowledgement default mismatch");
  require(legacy.confirmedReplicaNodes == legacy.replicaNodes,
          "legacy confirmed replica default mismatch");

  RepoWriteIntent intent;
  intent.operationId = "op-7";
  intent.objectName = manifest.objectName;
  intent.generation = 7;
  intent.expectedGeneration = 6;
  intent.digest = manifest.sha256;
  intent.replicationFactor = 3;
  intent.requiredAcks = 2;
  intent.consistency = "QUORUM";
  intent.selectedReplicas = manifest.replicaNodes;
  require(intent.toJson().find("\"requiredWriteAcks\":2") != std::string::npos,
          "write intent JSON missing acknowledgement contract");

  RepoWriteReceipt receipt;
  receipt.operationId = intent.operationId;
  receipt.repoNode = "/repo/A";
  receipt.objectName = intent.objectName;
  receipt.generation = intent.generation;
  receipt.digest = intent.digest;
  receipt.persistedBytes = 42;
  require(receipt.toJson().find("\"state\":\"COMMITTED\"") != std::string::npos,
          "write receipt JSON missing committed state");

  std::cout << "DISTRIBUTED_REPO_HA_CONTRACT_TEST_OK" << std::endl;
  return 0;
}
