#include "tests/boost-test.hpp"

#include "ndn-service-framework/ExecutionLease.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericExecutionLeaseTable)

static ndn::Buffer
textBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

static GenericExecutionLease
makeLease()
{
  GenericExecutionLease lease;
  lease.providerName = "/provider/A";
  lease.requesterName = "/user/A";
  lease.requestId = "request-1";
  lease.serviceName = "/Inference/NativeTracer";
  lease.planDigest = "plan-sha256";
  lease.resourceBindingSchema = "ndnsf-di-binding-v1";
  lease.resourceBindingProof = textBuffer("role=/Backbone;fragment=f1");
  lease.conflictKeys = {"compute-slot:0"};
  lease.expiresAtMs = 5000;
  lease.idempotencyKey = "prepare-request-1";
  return lease;
}

static ExecutionLeaseBinding
makeBinding()
{
  ExecutionLeaseBinding binding;
  binding.requesterName = "/user/A";
  binding.requestId = "request-1";
  binding.serviceName = "/Inference/NativeTracer";
  binding.planDigest = "plan-sha256";
  binding.resourceBindingSchema = "ndnsf-di-binding-v1";
  binding.resourceBindingProof = textBuffer("role=/Backbone;fragment=f1");
  return binding;
}

BOOST_AUTO_TEST_CASE(PrepareCommitActivateRelease)
{
  ProviderExecutionLeaseTable table("epoch-A");

  auto prepared = table.prepare(makeLease(), 1000);
  BOOST_REQUIRE(prepared.status);
  BOOST_CHECK_EQUAL(prepared.reasonCode, "OK");
  BOOST_CHECK(prepared.lease.state == ExecutionLeaseState::Prepared);
  BOOST_CHECK_EQUAL(prepared.lease.providerEpoch, "epoch-A");
  BOOST_CHECK(!prepared.lease.leaseId.empty());

  auto committed = table.commit(prepared.lease.leaseId, "epoch-A",
                                "commit-request-1", 1100);
  BOOST_REQUIRE(committed.status);
  BOOST_CHECK(committed.lease.state == ExecutionLeaseState::Committed);
  BOOST_CHECK(table.hasPinnedBindingProof(
    textBuffer("role=/Backbone;fragment=f1"), 1100));

  auto activated = table.validateAndActivate(
    prepared.lease.leaseId, "epoch-A", makeBinding(),
    "activate-request-1", 1200, 10000);
  BOOST_REQUIRE(activated.status);
  BOOST_CHECK(activated.lease.state == ExecutionLeaseState::Executing);
  BOOST_CHECK_EQUAL(activated.lease.executionDeadlineMs, 10000);

  auto released = table.release(prepared.lease.leaseId, "epoch-A",
                                "release-request-1", 1300);
  BOOST_REQUIRE(released.status);
  BOOST_CHECK(released.lease.state == ExecutionLeaseState::Released);
  BOOST_CHECK(!table.hasPinnedBindingProof(
    textBuffer("role=/Backbone;fragment=f1"), 1300));

  const auto counters = table.counters(1300);
  BOOST_CHECK_EQUAL(counters.prepared, 1);
  BOOST_CHECK_EQUAL(counters.committed, 1);
  BOOST_CHECK_EQUAL(counters.activated, 1);
  BOOST_CHECK_EQUAL(counters.released, 1);
  BOOST_CHECK_EQUAL(counters.activeExecuting, 0);
}

BOOST_AUTO_TEST_CASE(ConflictKeyIsExclusiveUntilOneTimeExpiryCleanup)
{
  ProviderExecutionLeaseTable table("epoch-A");
  auto first = table.prepare(makeLease(), 1000);
  BOOST_REQUIRE(first.status);

  auto competingLease = makeLease();
  competingLease.requestId = "request-2";
  competingLease.idempotencyKey = "prepare-request-2";
  auto conflicting = table.prepare(competingLease, 1100);
  BOOST_CHECK(!conflicting.status);
  BOOST_CHECK_EQUAL(conflicting.reasonCode, "LEASE_CAPACITY_REJECTED");

  BOOST_CHECK_EQUAL(table.cleanupExpired(5000), 1);
  BOOST_CHECK_EQUAL(table.cleanupExpired(5000), 0);

  competingLease.expiresAtMs = 9000;
  auto admitted = table.prepare(competingLease, 5000);
  BOOST_REQUIRE(admitted.status);
  BOOST_CHECK(table.hasActiveConflictKey("compute-slot:0", 5001));

  const auto counters = table.counters(5001);
  BOOST_CHECK_EQUAL(counters.conflict, 1);
  BOOST_CHECK_EQUAL(counters.expired, 1);
}

BOOST_AUTO_TEST_CASE(IdempotencyReplaysOnlyIdenticalOperation)
{
  ProviderExecutionLeaseTable table("epoch-A");
  auto lease = makeLease();
  auto first = table.prepare(lease, 1000);
  BOOST_REQUIRE(first.status);

  auto replay = table.prepare(lease, 1001);
  BOOST_REQUIRE(replay.status);
  BOOST_CHECK(replay.idempotentReplay);
  BOOST_CHECK_EQUAL(replay.lease.leaseId, first.lease.leaseId);

  lease.planDigest = "different-plan";
  auto conflict = table.prepare(lease, 1002);
  BOOST_CHECK(!conflict.status);
  BOOST_CHECK_EQUAL(conflict.reasonCode, "LEASE_IDEMPOTENCY_CONFLICT");

  auto committed = table.commit(first.lease.leaseId, "epoch-A", "commit-1", 1100);
  BOOST_REQUIRE(committed.status);
  auto commitReplay = table.commit(first.lease.leaseId, "epoch-A", "commit-1", 1101);
  BOOST_REQUIRE(commitReplay.status);
  BOOST_CHECK(commitReplay.idempotentReplay);
}

BOOST_AUTO_TEST_CASE(ValidationRejectsStaleEpochAndBindingMismatch)
{
  ProviderExecutionLeaseTable table("epoch-current");
  auto prepared = table.prepare(makeLease(), 1000);
  BOOST_REQUIRE(prepared.status);
  auto committed = table.commit(prepared.lease.leaseId, "epoch-current",
                                "commit-1", 1100);
  BOOST_REQUIRE(committed.status);

  auto stale = table.validate(prepared.lease.leaseId, "epoch-old",
                              makeBinding(), 1200);
  BOOST_CHECK(!stale.status);
  BOOST_CHECK_EQUAL(stale.reasonCode, "LEASE_STALE_EPOCH");

  auto wrongRequester = makeBinding();
  wrongRequester.requesterName = "/user/other";
  auto mismatch = table.validate(prepared.lease.leaseId, "epoch-current",
                                 wrongRequester, 1200);
  BOOST_CHECK(!mismatch.status);
  BOOST_CHECK_EQUAL(mismatch.reasonCode, "LEASE_REQUESTER_MISMATCH");

  auto wrongRequest = makeBinding();
  wrongRequest.requestId = "request-other";
  BOOST_CHECK_EQUAL(table.validate(prepared.lease.leaseId, "epoch-current",
                                   wrongRequest, 1200).reasonCode,
                    "LEASE_REQUEST_MISMATCH");

  auto wrongService = makeBinding();
  wrongService.serviceName = "/Inference/Other";
  BOOST_CHECK_EQUAL(table.validate(prepared.lease.leaseId, "epoch-current",
                                   wrongService, 1200).reasonCode,
                    "LEASE_SERVICE_MISMATCH");

  auto wrongPlan = makeBinding();
  wrongPlan.planDigest = "different-plan";
  BOOST_CHECK_EQUAL(table.validate(prepared.lease.leaseId, "epoch-current",
                                   wrongPlan, 1200).reasonCode,
                    "LEASE_PLAN_MISMATCH");

  auto wrongProof = makeBinding();
  wrongProof.resourceBindingProof = textBuffer("different-binding");
  BOOST_CHECK_EQUAL(table.validate(prepared.lease.leaseId, "epoch-current",
                                   wrongProof, 1200).reasonCode,
                    "LEASE_BINDING_MISMATCH");

  auto valid = table.validate(prepared.lease.leaseId, "epoch-current",
                              makeBinding(), 1200);
  BOOST_REQUIRE(valid.status);
  BOOST_CHECK(valid.lease.state == ExecutionLeaseState::Committed);
}

BOOST_AUTO_TEST_CASE(AbortRenewAndInvalidTransitionsFailClosed)
{
  ProviderExecutionLeaseTable table("epoch-A");
  auto prepared = table.prepare(makeLease(), 1000);
  BOOST_REQUIRE(prepared.status);

  auto renewed = table.renew(prepared.lease.leaseId, "epoch-A", "renew-1",
                             1100, 7000);
  BOOST_REQUIRE(renewed.status);
  BOOST_CHECK_EQUAL(renewed.lease.expiresAtMs, 7000);

  auto aborted = table.abort(prepared.lease.leaseId, "epoch-A", "abort-1", 1200);
  BOOST_REQUIRE(aborted.status);
  BOOST_CHECK(aborted.lease.state == ExecutionLeaseState::Aborted);

  auto commitAfterAbort = table.commit(prepared.lease.leaseId, "epoch-A",
                                       "commit-after-abort", 1300);
  BOOST_CHECK(!commitAfterAbort.status);
  BOOST_CHECK_EQUAL(commitAfterAbort.reasonCode, "LEASE_INVALID_TRANSITION");
}

BOOST_AUTO_TEST_CASE(ExecutingLeasePinsConflictUntilHardDeadline)
{
  ProviderExecutionLeaseTable table("epoch-A");
  auto prepared = table.prepare(makeLease(), 1000);
  BOOST_REQUIRE(prepared.status);
  BOOST_REQUIRE(table.commit(prepared.lease.leaseId, "epoch-A", "commit-1", 1100).status);
  BOOST_REQUIRE(table.validateAndActivate(
    prepared.lease.leaseId, "epoch-A", makeBinding(), "activate-1",
    1200, 3000).status);

  BOOST_CHECK(table.hasActiveConflictKey("compute-slot:0", 2999));
  BOOST_CHECK_EQUAL(table.cleanupExpired(3000), 1);
  BOOST_CHECK(!table.hasActiveConflictKey("compute-slot:0", 3000));

  const auto stored = table.find(prepared.lease.leaseId);
  BOOST_REQUIRE(stored.has_value());
  BOOST_CHECK(stored->state == ExecutionLeaseState::Expired);
}

BOOST_AUTO_TEST_CASE(MissingExpiryAndReplayBoundariesAreExplicit)
{
  ProviderExecutionLeaseTable table("epoch-A");
  auto missing = table.commit("missing", "epoch-A", "commit-missing", 1000);
  BOOST_CHECK(!missing.status);
  BOOST_CHECK_EQUAL(missing.reasonCode, "LEASE_NOT_FOUND");

  auto lease = makeLease();
  lease.expiresAtMs = 1500;
  auto prepared = table.prepare(lease, 1000);
  BOOST_REQUIRE(prepared.status);
  auto expired = table.commit(prepared.lease.leaseId, "epoch-A", "commit-expired", 1500);
  BOOST_CHECK(!expired.status);
  BOOST_CHECK_EQUAL(expired.reasonCode, "LEASE_EXPIRED");

  ProviderExecutionLeaseTable activeTable("epoch-B");
  auto active = activeTable.prepare(makeLease(), 1000);
  BOOST_REQUIRE(active.status);
  BOOST_REQUIRE(activeTable.commit(active.lease.leaseId, "epoch-B", "commit-1", 1100).status);
  auto activated = activeTable.validateAndActivate(
    active.lease.leaseId, "epoch-B", makeBinding(), "activate-1", 1200, 3000);
  BOOST_REQUIRE(activated.status);

  auto changedBinding = makeBinding();
  changedBinding.planDigest = "changed-plan";
  auto replayConflict = activeTable.validateAndActivate(
    active.lease.leaseId, "epoch-B", changedBinding, "activate-1", 1201, 3000);
  BOOST_CHECK(!replayConflict.status);
  BOOST_CHECK_EQUAL(replayConflict.reasonCode, "LEASE_IDEMPOTENCY_CONFLICT");

  auto beyondHardDeadline = activeTable.renew(
    active.lease.leaseId, "epoch-B", "renew-too-far", 1300, 3001);
  BOOST_CHECK(!beyondHardDeadline.status);
  BOOST_CHECK_EQUAL(beyondHardDeadline.reasonCode, "LEASE_EXPIRED");

  auto released = activeTable.release(
    active.lease.leaseId, "epoch-B", "release-1", 1400);
  BOOST_REQUIRE(released.status);
  auto releaseReplay = activeTable.release(
    active.lease.leaseId, "epoch-B", "release-1", 1401);
  BOOST_REQUIRE(releaseReplay.status);
  BOOST_CHECK(releaseReplay.idempotentReplay);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
