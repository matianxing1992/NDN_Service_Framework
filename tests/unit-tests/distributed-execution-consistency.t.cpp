#include "NDNSF-DistributedInference/cpp/ndnsf-di/DistributedExecutionConsistency.hpp"

#include <boost/test/unit_test.hpp>

namespace nsf = ndn_service_framework;
namespace di = ndnsf::di;

namespace {

nsf::GenericExecutionLease
makeLease()
{
  nsf::GenericExecutionLease lease;
  lease.providerName = "/p1";
  lease.requesterName = "/u";
  lease.requestId = "req";
  lease.serviceName = "/Inference/Test";
  lease.planDigest = "sha256:plan";
  lease.resourceBindingSchema = "ndnsf-di-binding-v1";
  lease.resourceBindingProof = ndn::Buffer{1, 2, 3};
  lease.expiresAtMs = 5000;
  lease.idempotencyKey = "prepare:req";
  return lease;
}

nsf::ExecutionLeaseBinding
makeBinding()
{
  nsf::ExecutionLeaseBinding binding;
  binding.requesterName = "/u";
  binding.requestId = "req";
  binding.serviceName = "/Inference/Test";
  binding.planDigest = "sha256:plan";
  binding.resourceBindingSchema = "ndnsf-di-binding-v1";
  binding.resourceBindingProof = ndn::Buffer{1, 2, 3};
  return binding;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedExecutionConsistency)

BOOST_AUTO_TEST_CASE(CompleteCertificateValidatesCommittedCanonicalLease)
{
  nsf::ProviderExecutionLeaseTable table("boot-1");
  auto prepared = table.prepare(makeLease(), 1000);
  BOOST_REQUIRE(prepared.status);
  BOOST_REQUIRE(table.commit(prepared.lease.leaseId, "boot-1", "/u",
                             "commit:req", 1100).status);
  di::DistributedExecutionCertificateView certificate;
  certificate.schema = di::EXECUTION_COMMIT_CERTIFICATE_SCHEMA;
  certificate.digest = "sha256:certificate";
  certificate.requestId = "req";
  certificate.attemptEpoch = 1;
  certificate.planDigest = "sha256:plan";
  certificate.expectedCount = 1;
  certificate.receiptCount = 1;
  certificate.memberKeys = {"r1|/p1|boot-1|" + prepared.lease.leaseId + "|sha256:b"};
  certificate.localMemberKey = certificate.memberKeys.front();
  const auto result = di::validateDistributedExecutionCertificate(
    certificate, table, makeBinding(), prepared.lease.leaseId, "boot-1", 1200);
  BOOST_CHECK(result.status);
  BOOST_CHECK_EQUAL(result.reason, "OK");
}

BOOST_AUTO_TEST_CASE(PartialDuplicateStaleAndNonMemberCertificatesFailClosed)
{
  nsf::ProviderExecutionLeaseTable table("boot-1");
  auto prepared = table.prepare(makeLease(), 1000);
  BOOST_REQUIRE(prepared.status);
  BOOST_REQUIRE(table.commit(prepared.lease.leaseId, "boot-1", "/u",
                             "commit:req", 1100).status);
  di::DistributedExecutionCertificateView certificate;
  certificate.schema = di::EXECUTION_COMMIT_CERTIFICATE_SCHEMA;
  certificate.digest = "sha256:certificate";
  certificate.requestId = "req";
  certificate.attemptEpoch = 1;
  certificate.planDigest = "sha256:plan";
  certificate.expectedCount = 2;
  certificate.receiptCount = 1;
  certificate.memberKeys = {"member"};
  certificate.localMemberKey = "member";
  BOOST_CHECK_EQUAL(di::validateDistributedExecutionCertificate(
    certificate, table, makeBinding(), prepared.lease.leaseId, "boot-1", 1200).reason,
    "DI_CERTIFICATE_RECEIPT_SET_MISMATCH");
  certificate.expectedCount = certificate.receiptCount = 2;
  certificate.memberKeys = {"member", "member"};
  BOOST_CHECK_EQUAL(di::validateDistributedExecutionCertificate(
    certificate, table, makeBinding(), prepared.lease.leaseId, "boot-1", 1200).reason,
    "DI_CERTIFICATE_RECEIPT_SET_MISMATCH");
  certificate.expectedCount = certificate.receiptCount = 1;
  certificate.memberKeys = {"other"};
  BOOST_CHECK_EQUAL(di::validateDistributedExecutionCertificate(
    certificate, table, makeBinding(), prepared.lease.leaseId, "boot-1", 1200).reason,
    "DI_CERTIFICATE_PROVIDER_NOT_MEMBER");
  certificate.localMemberKey = "other";
  BOOST_CHECK_EQUAL(di::validateDistributedExecutionCertificate(
    certificate, table, makeBinding(), prepared.lease.leaseId, "boot-old", 1200).reason,
    "LEASE_STALE_EPOCH");
}

BOOST_AUTO_TEST_CASE(CompactEnvelopeDerivesRedundantCountsAndAttemptEpoch)
{
  const std::map<std::string, std::string> fields{
    {"executionCertificateSchema", di::EXECUTION_COMMIT_CERTIFICATE_SCHEMA},
    {"executionCertificateDigest", "sha256:certificate"},
    {"executionRequestId", "req"},
    {"executionAttemptEpoch", "3"},
    {"executionLeasePlanDigest", "sha256:plan"},
    {"executionCertificateMembers", "member-a,member-b"},
    {"executionCertificateLocalMember", "member-a"},
  };
  const auto certificate = di::distributedExecutionCertificateFromFields(fields);
  BOOST_CHECK_EQUAL(certificate.attemptEpoch, 3);
  BOOST_CHECK_EQUAL(certificate.expectedCount, 2);
  BOOST_CHECK_EQUAL(certificate.receiptCount, 2);
}

BOOST_AUTO_TEST_SUITE_END()
