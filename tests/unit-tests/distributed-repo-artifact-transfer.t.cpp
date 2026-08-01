#include "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTransfer.hpp"
#include "tests/boost-test.hpp"

#include <set>

namespace ndnsf_distributed_repo::test {

namespace {

ArtifactReference
makeReference()
{
  return ArtifactReference{
    "/artifact/spec164/t007",
    "sha256",
    std::string(64, 'a'),
    8192,
    "artifact-manifest-v2",
    "/publisher/manifests/spec164/t007",
    "/publisher",
    "policy-epoch-1",
  };
}

ArtifactUploadLease
makeLease(const std::string& provider, const std::string& suffix)
{
  return ArtifactUploadLease{
    "lease-" + suffix,
    "operation-1",
    provider,
    makeReference(),
    8192,
    1000,
    10000,
    "replay-" + suffix,
  };
}

std::vector<ArtifactChunk>
makeChunks()
{
  return {
    ArtifactChunk{0, 0, 4096, "sha256", std::string(64, 'b'), 0, 0},
    ArtifactChunk{1, 4096, 4096, "sha256", std::string(64, 'c'), 0, 0},
  };
}

ArtifactResumeIdentity
makeResumeIdentity()
{
  return ArtifactResumeIdentity{
    makeReference(),
    std::string(64, 'd'),
    4096,
    4096,
  };
}

ArtifactUploadLease
makeRenewedLease(const std::string& suffix, uint64_t issuedAtMs,
                 uint64_t expiresAtMs)
{
  auto lease = makeLease("/repo/a", suffix);
  lease.issuedAtMs = issuedAtMs;
  lease.expiresAtMs = expiresAtMs;
  return lease;
}

} // namespace

BOOST_AUTO_TEST_SUITE(DistributedRepoArtifactTransfer)

BOOST_AUTO_TEST_CASE(AcceptsReorderingSuppressesDuplicatesAndAppliesBackpressure)
{
  AdaptiveTransferOptions options;
  options.initialWindow = 4;
  options.maximumWindow = 8;
  options.verificationBacklogLimit = 2;
  options.segmentTimeoutMs = 100;
  AdaptiveArtifactTransfer transfer(5, options);

  const auto first = transfer.poll(1000);
  BOOST_REQUIRE_EQUAL(first.size(), 2);
  BOOST_CHECK_EQUAL(first[0].segmentNo, 0);
  BOOST_CHECK_EQUAL(first[1].segmentNo, 1);

  BOOST_CHECK(
    transfer.receive(1, 100, 120, 1010) ==
    ArtifactSegmentDisposition::Accepted);
  BOOST_CHECK(
    transfer.receive(0, 100, 120, 1011) ==
    ArtifactSegmentDisposition::Accepted);
  BOOST_CHECK(
    transfer.receive(1, 100, 120, 1012) ==
    ArtifactSegmentDisposition::Duplicate);
  BOOST_CHECK(transfer.poll(1013).empty());
  auto snapshot = transfer.snapshot();
  BOOST_CHECK_EQUAL(snapshot.verificationBacklog, 2);
  BOOST_CHECK_EQUAL(snapshot.duplicateCount, 1);

  transfer.markVerified(1);
  const auto second = transfer.poll(1020);
  BOOST_REQUIRE_EQUAL(second.size(), 1);
  BOOST_CHECK_EQUAL(second[0].segmentNo, 2);
  transfer.markVerified(0);
  BOOST_CHECK(
    transfer.receive(2, 100, 120, 1025) ==
    ArtifactSegmentDisposition::Accepted);
  transfer.markVerified(2);

  const auto remaining = transfer.poll(1030);
  BOOST_REQUIRE_EQUAL(remaining.size(), 2);
  BOOST_CHECK_EQUAL(remaining[0].segmentNo, 3);
  BOOST_CHECK_EQUAL(remaining[1].segmentNo, 4);
  BOOST_CHECK(
    transfer.receive(4, 100, 120, 1031) ==
    ArtifactSegmentDisposition::Accepted);
  BOOST_CHECK(
    transfer.receive(3, 100, 120, 1032) ==
    ArtifactSegmentDisposition::Accepted);
  transfer.markVerified(3);
  transfer.markVerified(4);

  snapshot = transfer.snapshot();
  BOOST_CHECK(snapshot.complete);
  BOOST_CHECK(!snapshot.failed);
  BOOST_CHECK_EQUAL(snapshot.verifiedSegments, 5);
  BOOST_CHECK_EQUAL(snapshot.logicalBytes, 500);
  BOOST_CHECK_EQUAL(snapshot.wireBytes, 600);
  BOOST_CHECK_EQUAL(snapshot.interestCount, 5);
}

BOOST_AUTO_TEST_CASE(RetransmitsTimedOutSegmentsWithBoundedAimdWindow)
{
  AdaptiveTransferOptions options;
  options.initialWindow = 2;
  options.minimumWindow = 1;
  options.maximumWindow = 4;
  options.verificationBacklogLimit = 4;
  options.maximumRetries = 2;
  options.segmentTimeoutMs = 50;
  AdaptiveArtifactTransfer transfer(2, options);

  BOOST_REQUIRE_EQUAL(transfer.poll(100).size(), 2);
  transfer.expire(151);
  auto snapshot = transfer.snapshot();
  BOOST_CHECK_EQUAL(snapshot.timeoutCount, 2);
  BOOST_CHECK_EQUAL(snapshot.congestionWindow, 1);

  const auto retryOne = transfer.poll(152);
  BOOST_REQUIRE_EQUAL(retryOne.size(), 1);
  BOOST_CHECK(retryOne[0].retransmission);
  BOOST_CHECK_EQUAL(retryOne[0].attempt, 2);
  BOOST_CHECK(
    transfer.receive(retryOne[0].segmentNo, 4096, 4200, 153) ==
    ArtifactSegmentDisposition::Accepted);
  transfer.markVerified(retryOne[0].segmentNo);

  const auto retryTwo = transfer.poll(154);
  BOOST_REQUIRE_EQUAL(retryTwo.size(), 1);
  BOOST_CHECK(retryTwo[0].retransmission);
  BOOST_CHECK(
    transfer.receive(retryTwo[0].segmentNo, 4096, 4200, 155) ==
    ArtifactSegmentDisposition::Accepted);
  transfer.markVerified(retryTwo[0].segmentNo);

  snapshot = transfer.snapshot();
  BOOST_CHECK(snapshot.complete);
  BOOST_CHECK_EQUAL(snapshot.retransmissionCount, 2);
  BOOST_CHECK_EQUAL(snapshot.retransmittedBytes, 8400);
  BOOST_CHECK(snapshot.congestionWindow <= options.maximumWindow);
}

BOOST_AUTO_TEST_CASE(FailsClosedWhenRetryBudgetIsExhausted)
{
  AdaptiveTransferOptions options;
  options.initialWindow = 1;
  options.maximumWindow = 1;
  options.verificationBacklogLimit = 1;
  options.maximumRetries = 1;
  options.segmentTimeoutMs = 10;
  AdaptiveArtifactTransfer transfer(1, options);
  BOOST_REQUIRE_EQUAL(transfer.poll(0).size(), 1);
  transfer.expire(10);
  BOOST_REQUIRE_EQUAL(transfer.poll(11).size(), 1);
  transfer.expire(21);
  const auto snapshot = transfer.snapshot();
  BOOST_CHECK(snapshot.failed);
  BOOST_CHECK(snapshot.failureReason.find("repo-transfer-retry-exhausted:") == 0);
  BOOST_CHECK(transfer.poll(22).empty());
}

BOOST_AUTO_TEST_CASE(CollaborationControlCountDependsOnReplicasNotSegments)
{
  const std::vector<ArtifactUploadLease> leases{
    makeLease("/repo/a", "a"),
    makeLease("/repo/b", "b"),
  };
  ReplicaLeaseControlFlow smallArtifact;
  smallArtifact.beginCollaboration("request-small");
  smallArtifact.closeAcks(3);
  smallArtifact.commitPlan(leases, 2000);

  ReplicaLeaseControlFlow largeArtifact;
  largeArtifact.beginCollaboration("request-large");
  largeArtifact.closeAcks(3);
  largeArtifact.commitPlan(leases, 2000);

  const auto small = smallArtifact.snapshot();
  const auto large = largeArtifact.snapshot();
  BOOST_CHECK_EQUAL(toString(small.state), "PLAN_COMMITTED");
  BOOST_CHECK_EQUAL(small.controlOperationCount, 3);
  BOOST_CHECK_EQUAL(large.controlOperationCount, small.controlOperationCount);
  BOOST_CHECK_EQUAL(small.selectedReplicaCount, 2);
  BOOST_CHECK_EQUAL(small.candidateCount, 3);
}

BOOST_AUTO_TEST_CASE(LeasePlanRejectsExpiredOrDuplicateSelections)
{
  {
    ReplicaLeaseControlFlow flow;
    flow.beginCollaboration("request-expired");
    flow.closeAcks(1);
    BOOST_CHECK_THROW(
      flow.commitPlan({makeLease("/repo/a", "a")}, 10000),
      ArtifactValidationError);
  }
  {
    ReplicaLeaseControlFlow flow;
    flow.beginCollaboration("request-duplicate");
    flow.closeAcks(2);
    BOOST_CHECK_EXCEPTION(
      flow.commitPlan(
        {makeLease("/repo/a", "a"), makeLease("/repo/a", "b")}, 2000),
      std::invalid_argument,
      [] (const std::invalid_argument& error) {
        return std::string(error.what()).find(
          "repo-lease-control-duplicate-selection:") == 0;
      });
  }
}

BOOST_AUTO_TEST_CASE(ResumePlansOnlyMissingChunksAndProgressIsMonotonic)
{
  ArtifactResumeSession session(
    makeResumeIdentity(), makeLease("/repo/a", "initial"), makeChunks(), 2000);
  session.restoreVerified({0});
  const auto missing = session.missingChunks(2001);
  BOOST_REQUIRE_EQUAL(missing.size(), 1);
  BOOST_CHECK_EQUAL(missing.front(), 1);

  BOOST_CHECK(!session.markVerified(0, 2001));
  BOOST_CHECK(session.markVerified(1, 2001));
  BOOST_CHECK(!session.markVerified(1, 2001));
  session.complete(2002);
  const auto snapshot = session.snapshot();
  BOOST_CHECK_EQUAL(toString(snapshot.state), "COMPLETED");
  BOOST_CHECK_EQUAL(snapshot.verifiedChunks, 2);
  BOOST_CHECK_EQUAL(snapshot.newlyVerifiedBytes, 4096);
  BOOST_CHECK_EQUAL(snapshot.avoidedRetransmissionBytes, 8192);
}

BOOST_AUTO_TEST_CASE(ExpiredAndCancelledSessionsRequireExactFreshLease)
{
  ArtifactResumeSession expired(
    makeResumeIdentity(), makeLease("/repo/a", "initial"), makeChunks(), 2000);
  expired.restoreVerified({0});
  BOOST_CHECK(expired.expire(10000));
  auto renewed = makeRenewedLease("renewed", 10000, 20000);
  expired.resume(makeResumeIdentity(), renewed, 10001);
  BOOST_CHECK_EQUAL(expired.missingChunks(10002).front(), 1);

  ArtifactResumeSession cancelled(
    makeResumeIdentity(), makeLease("/repo/a", "cancel"), makeChunks(), 2000);
  cancelled.restoreVerified({0});
  cancelled.cancel(true);
  auto wrongIdentity = makeResumeIdentity();
  wrongIdentity.manifestRootDigest = std::string(64, 'e');
  BOOST_CHECK_THROW(
    cancelled.resume(
      wrongIdentity, makeRenewedLease("wrong", 10000, 21000), 10001),
    ArtifactValidationError);
  cancelled.resume(
    makeResumeIdentity(), makeRenewedLease("resume", 10000, 21000), 10001);
  BOOST_CHECK_EQUAL(cancelled.snapshot().verifiedChunks, 1);
}

BOOST_AUTO_TEST_CASE(LeaseRenewalAndDestructiveCancellationFailClosed)
{
  ArtifactResumeSession session(
    makeResumeIdentity(), makeLease("/repo/a", "initial"), makeChunks(), 2000);
  session.markVerified(0, 2001);
  BOOST_CHECK_THROW(
    session.renewLease(
      makeRenewedLease("same-expiry", 2000, 10000), 2001),
    ArtifactValidationError);
  session.renewLease(makeRenewedLease("renewed", 9000, 20000), 9001);
  BOOST_CHECK_EQUAL(session.snapshot().expiresAtMs, 20000);
  session.cancel(false);
  const auto snapshot = session.snapshot();
  BOOST_CHECK_EQUAL(toString(snapshot.state), "FAILED");
  BOOST_CHECK(!snapshot.preservesProgress);
  BOOST_CHECK_EQUAL(snapshot.verifiedChunks, 0);
  BOOST_CHECK_THROW(
    session.missingChunks(9002), std::logic_error);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf_distributed_repo::test
