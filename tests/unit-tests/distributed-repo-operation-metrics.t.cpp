#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp"
#include "tests/boost-test.hpp"

namespace ndnsf_distributed_repo::test {

BOOST_AUTO_TEST_SUITE(DistributedRepoOperationMetrics)

BOOST_AUTO_TEST_CASE(CanonicalMetricsHaveStableMachineReadableSemantics)
{
  RepoOperationMetrics metrics;
  metrics.operationId = "publish-01HZZ9Q5";
  metrics.startedAtMs = 1000;
  metrics.completedAtMs = 1500;
  metrics.phaseTimingsMs = {
    {"discovery", 2.5},
    {"ackCollection", 1.0},
    {"planning", 1.0},
    {"queueWait", 1.0},
    {"sessionStart", 1.0},
    {"transfer", 400.0},
    {"verification", 20.0},
    {"persistence", 35.0},
    {"replication", 25.0},
    {"commit", 8.0},
    {"activation", 5.5},
  };
  metrics.logicalPayloadBytes = 1024;
  metrics.dataWireBytes = 1100;
  metrics.interestWireBytes = 100;
  metrics.wireBytes = 1200;
  metrics.retransmittedBytes = 100;
  metrics.payloadStoreBytesRead = 1024;
  metrics.payloadStoreBytesWritten = 1024;
  metrics.metadataStoreBytesRead = 64;
  metrics.metadataStoreBytesWritten = 64;
  metrics.storageBytesRead = 1088;
  metrics.storageBytesWritten = 1088;
  metrics.asymmetricVerifications = 1;
  metrics.digestVerifications = 4;
  metrics.asymmetricVerificationMs = 0.8;
  metrics.digestVerificationMs = 1.2;
  metrics.controlOperations = 3;
  metrics.metadataOperations = 7;
  metrics.metadataRecordCount = 5;
  metrics.requestedReplicaCount = 3;
  metrics.selectedReplicaCount = 3;
  metrics.committedReplicaCount = 2;
  metrics.rejectedReplicaReceiptCount = 1;

  BOOST_CHECK_NO_THROW(metrics.validate());
  const auto json = metrics.toJson();
  BOOST_CHECK(json.find("\"operationId\":\"publish-01HZZ9Q5\"") != std::string::npos);
  BOOST_CHECK(json.find("\"phaseTimingsMs\":{") != std::string::npos);
  BOOST_CHECK(json.find("\"logicalPayloadBytes\":1024") != std::string::npos);
  BOOST_CHECK(json.find("\"dataWireBytes\":1100") != std::string::npos);
  BOOST_CHECK(json.find("\"metadataStoreBytesWritten\":64") != std::string::npos);
  BOOST_CHECK(json.find("\"committedReplicaCount\":2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(MalformedIdentityTimingAndReceiptCountsFailClosed)
{
  RepoOperationMetrics metrics;
  metrics.operationId = "operation-1";
  metrics.requestedReplicaCount = 2;
  metrics.selectedReplicaCount = 2;
  metrics.committedReplicaCount = 1;
  BOOST_CHECK_NO_THROW(metrics.validate());

  metrics.operationId.assign(RepoOperationMetrics::MAX_OPERATION_ID_BYTES + 1, 'x');
  BOOST_CHECK_THROW(metrics.validate(), std::invalid_argument);

  metrics.operationId = "operation-1";
  metrics.phaseTimingsMs = {{"packet-loop", 1.0}};
  BOOST_CHECK_THROW(metrics.validate(), std::invalid_argument);

  metrics.phaseTimingsMs = {{"transfer", -0.1}};
  BOOST_CHECK_THROW(metrics.validate(), std::invalid_argument);

  metrics.phaseTimingsMs.clear();
  metrics.committedReplicaCount = 3;
  BOOST_CHECK_THROW(metrics.validate(), std::invalid_argument);

  metrics.committedReplicaCount = 1;
  metrics.dataWireBytes = 100;
  metrics.interestWireBytes = 10;
  metrics.wireBytes = 109;
  BOOST_CHECK_THROW(metrics.validate(), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
