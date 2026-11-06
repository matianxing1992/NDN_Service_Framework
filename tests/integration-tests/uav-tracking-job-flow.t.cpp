#include "tests/fixtures/uav-tracking-core-flow.hpp"

namespace ndnsf::test {
using namespace tracking_flow;

BOOST_AUTO_TEST_SUITE(UavTrackingJobCoreFlow)

BOOST_FIXTURE_TEST_CASE(RealAckSelectionAndSignedResultCompleteWithoutStoppingUserFace, JobFlowFixture)
{
  install();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  BOOST_CHECK_EQUAL(executions.load(), 4);
  BOOST_CHECK_EQUAL(latest.completedWindows, 1);
  BOOST_CHECK_EQUAL(latest.selectedProviders.size(), 4);
  BOOST_CHECK_EQUAL(latest.missionId, definition.missionId);
  BOOST_CHECK_EQUAL(latest.wireMissionId, "tracking-" + definition.runId);
  BOOST_CHECK(!latest.lastResultDigest.empty());
}

BOOST_FIXTURE_TEST_CASE(MissingComputeRoleCancelsPendingCallAndLateTimeoutCannotReopenJob,
                        JobFlowFixture)
{
  install(true);
  definition.requestTimeoutMs = 1500;
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.state == TrackingJobState::FAILED; });
  const auto generation = latest.generation;
  const auto reason = latest.reason;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1700);
  environment->pumpUntilWithAttributeAuthority([deadline] {
    return std::chrono::steady_clock::now() >= deadline;
  });
  BOOST_CHECK_EQUAL(executions.load(), 0);
  BOOST_CHECK(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(latest.generation, generation);
  BOOST_CHECK_EQUAL(latest.reason, reason);
}

BOOST_FIXTURE_TEST_CASE(TwoWindowsReleaseEveryRequestScopeBeforeTheNextRun, JobFlowFixture)
{
  configureWindows(2);
  install();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  BOOST_CHECK_EQUAL(latest.completedWindows, 2);
  BOOST_CHECK_EQUAL(executions.load(), 8);
  BOOST_REQUIRE_EQUAL(completedRequests.size(), 2);
  BOOST_CHECK(completedRequests[0] != completedRequests[1]);
  for (const auto& request : completedRequests) {
    for (const auto* scope : {TRACKING_INPUT_KEY_SCOPE, "uav-tracking-observation",
                              "uav-tracking-result"}) {
      const auto records = environment->user().waitForVerifiedCollaborationData(
        request, scope, ndn::Name("/"), 1, 1, false);
      BOOST_CHECK(records.empty());
      // A syntactically valid control publication cannot find a released key.
      BOOST_CHECK(!environment->user().publishCollaborationData(definition.computeIdentity,
        request, scope, ndn::Name("/test/scope-probe"), asBuffer("abc")));
    }
  }
  config["jobId"] = "job-2";
  config["runId"] = "run-2";
  config["outputDirectory"] = (root / "output-run-2").string();
  for (auto& source : config["sources"]) source["sourceEpoch"] = 2;
  configureWindows(1);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  BOOST_CHECK(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_CHECK(latest.state == TrackingJobState::CANCELLED);
}

BOOST_FIXTURE_TEST_CASE(EligibleIdentityWithWrongModeFeedbackCannotCommit, JobFlowFixture)
{
  install(false, false, true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.state == TrackingJobState::FAILED; });
  BOOST_CHECK(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(executions.load(), 0);
}

BOOST_FIXTURE_TEST_CASE(RevocationAckWhileNativeWorkIsHeldNeverReleasesAdmission, JobFlowFixture)
{
  install(false, true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return computeEntered->load(); });
  BOOST_REQUIRE(computeEntered->load());
  BOOST_REQUIRE(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return latest.cancelState == "REMOTE_CONFIRMED";
  });
  BOOST_CHECK_EQUAL(latest.cleanupState, "DRAINING");
  BOOST_CHECK(job->start(definition) == TrackingAdmission::BUSY);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "INCOMPLETE"; });
  BOOST_CHECK_EQUAL(latest.cancelState, "REMOTE_CONFIRMED");
  BOOST_CHECK_EQUAL(latest.cleanupState, "INCOMPLETE");
  BOOST_CHECK(job->start(definition) == TrackingAdmission::BUSY);
  releaseCompute->store(true);
}

BOOST_FIXTURE_TEST_CASE(AllSelectedRoleDrainProofsReleaseAdmissionOnlyAfterWorkEnds, JobFlowFixture)
{
  install(false, true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return computeEntered->load(); });
  BOOST_REQUIRE(computeEntered->load());
  BOOST_REQUIRE(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return latest.cancelState == "REMOTE_CONFIRMED";
  });
  BOOST_CHECK_EQUAL(latest.cleanupState, "DRAINING");
  releaseCompute->store(true);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_CHECK(latest.state == TrackingJobState::CANCELLED);
  BOOST_CHECK_EQUAL(latest.cleanupState, "CLEAN");
}

BOOST_FIXTURE_TEST_CASE(MissingSelectedSourceProofCannotClaimClean, JobFlowFixture)
{
  install(false, true, false, true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return computeEntered->load(); });
  BOOST_REQUIRE(computeEntered->load());
  BOOST_REQUIRE(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return latest.cancelState == "REMOTE_CONFIRMED";
  });
  releaseCompute->store(true);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "INCOMPLETE"; });
  BOOST_CHECK_EQUAL(latest.cancelState, "REMOTE_CONFIRMED");
  BOOST_CHECK_EQUAL(latest.cleanupState, "INCOMPLETE");
  BOOST_CHECK(job->start(definition) == TrackingAdmission::BUSY);
}

BOOST_FIXTURE_TEST_CASE(WrongEpochProofCannotClaimClean, JobFlowFixture)
{
  install(false, true, false, false, "source_epoch");
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return computeEntered->load(); });
  BOOST_REQUIRE(computeEntered->load());
  BOOST_REQUIRE(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return latest.cancelState == "REMOTE_CONFIRMED";
  });
  releaseCompute->store(true);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "INCOMPLETE"; });
  BOOST_CHECK_EQUAL(latest.cancelState, "REMOTE_CONFIRMED");
  BOOST_CHECK_EQUAL(latest.cleanupState, "INCOMPLETE");
  BOOST_CHECK(job->start(definition) == TrackingAdmission::BUSY);
}

BOOST_FIXTURE_TEST_CASE(StopDuringNativeWorkCompletesOnceAndKeepsTheFaceRunning, JobFlowFixture)
{
  install(false, true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return computeEntered->load(); });
  BOOST_REQUIRE(computeEntered->load());
  auto completions = std::make_shared<size_t>(0);
  job->stop([completions] (const TrackingCleanupResult& result) {
    ++*completions;
    BOOST_CHECK_EQUAL(result.cleanupState, "CLEAN");
  });
  environment->pumpUntilWithAttributeAuthority([this] {
    return latest.cancelState == "REMOTE_CONFIRMED";
  });
  BOOST_CHECK_EQUAL(*completions, 0);
  releaseCompute->store(true);
  environment->pumpUntilWithAttributeAuthority([completions] { return *completions != 0; });
  BOOST_REQUIRE_EQUAL(*completions, 1);
  auto unrelated = std::make_shared<bool>(false);
  boost::asio::post(environment->userFace().getIoContext(), [unrelated] { *unrelated = true; });
  environment->pumpUntilWithAttributeAuthority([unrelated] { return *unrelated; });
  BOOST_CHECK(*unrelated);
  BOOST_CHECK_EQUAL(*completions, 1);
  BOOST_CHECK(latest.state == TrackingJobState::CANCELLED);
}

BOOST_FIXTURE_TEST_CASE(SuccessCannotAdvanceOrReleaseAdmissionWhileASourceIsStillWorking, JobFlowFixture)
{
  install(false, false, false, false, {}, 0, 4, true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] {
    return sourceHeld->load() && computeResponseReady->load();
  });
  BOOST_REQUIRE(sourceHeld->load());
  BOOST_REQUIRE(computeResponseReady->load());
  environment->pumpUntilWithAttributeAuthority([this] { return latest.waitingForWorkDrain; });
  BOOST_REQUIRE(latest.waitingForWorkDrain);
  BOOST_CHECK(latest.state == TrackingJobState::RUNNING);
  BOOST_CHECK_EQUAL(latest.completedWindows, 0);
  BOOST_CHECK(job->start(definition) == TrackingAdmission::BUSY);
  releaseSource->store(true);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  BOOST_CHECK_EQUAL(latest.completedWindows, 1);
}

BOOST_FIXTURE_TEST_CASE(CancelAtTheResultDrainBarrierPreservesTheAlreadyReceivedResponse, JobFlowFixture)
{
  install(false, false, false, false, {}, 0, 4, true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.waitingForWorkDrain; });
  BOOST_REQUIRE(latest.waitingForWorkDrain);
  BOOST_REQUIRE(sourceHeld->load());
  BOOST_CHECK(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  releaseSource->store(true);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  // Core already delivered the result: terminal response wins this existing
  // race, while proof collection still fences the outstanding source work.
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  BOOST_CHECK_EQUAL(latest.completedWindows, 1);
  BOOST_CHECK_EQUAL(latest.cleanupState, "CLEAN");
  BOOST_CHECK(!latest.waitingForWorkDrain);
}

BOOST_FIXTURE_TEST_CASE(ReceivedFinalResponseWithMissingSourceProofFailsWithinOneDrainBudget,
                        JobFlowFixture)
{
  install(false, false, false, true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.waitingForWorkDrain; });
  BOOST_REQUIRE(latest.waitingForWorkDrain);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  const auto done = [&] {
    return latest.cleanupState == "INCOMPLETE" || std::chrono::steady_clock::now() >= deadline;
  };
  while (!done()) environment->pumpUntilWithAttributeAuthority(done);
  BOOST_REQUIRE(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(latest.failureStage, "drain");
  BOOST_CHECK_EQUAL(latest.cleanupState, "INCOMPLETE");
  BOOST_CHECK_EQUAL(latest.completedWindows, 0);
  BOOST_CHECK_EQUAL(executions.load(), 4);
  BOOST_CHECK(job->requestCancel(definition.jobId) == TrackingCancelResult::ALREADY_TERMINAL);
  BOOST_CHECK(job->start(definition) == TrackingAdmission::BUSY);
}

BOOST_FIXTURE_TEST_CASE(Spec231UavTerminalCleanJobSeparatesOldWireFromNewIdentity, JobFlowFixture)
{
  install();
  std::vector<ndn::Data> oldPackets;
  ndn::signal::ScopedConnection observation = environment->providerFace(3).onSendData.connect(
    [&oldPackets](const ndn::Data& data) {
      if (oldPackets.size() < 64) oldPackets.push_back(data);
    });
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  BOOST_REQUIRE_EQUAL(completedRequests.size(), 1U);
  BOOST_REQUIRE(!oldPackets.empty());
  observation.disconnect();
  const auto oldRequest = completedRequests.front();
  const auto oldGeneration = latest.generation;
  const auto oldResult = latest.lastResultDigest;
  config["jobId"] = "spec231-new-job";
  config["runId"] = "spec231-new-run";
  config["outputDirectory"] = (root / "spec231-new-output").string();
  for (auto& source : config["sources"]) source["sourceEpoch"] = 2;
  configureWindows(1);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  BOOST_CHECK_EQUAL(latest.generation, oldGeneration + 1);
  BOOST_CHECK(!environment->user().publishCollaborationData(definition.computeIdentity,
    oldRequest, TRACKING_INPUT_KEY_SCOPE, TRACKING_CANCEL_TOPIC, asBuffer("old-control")));
  // Original signed packets retain their old names. This verifies transport
  // isolation, not decoded-payload rejection under a current packet name.
  for (const auto& packet : oldPackets) environment->userFace().receive(packet);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  BOOST_CHECK_EQUAL(latest.runId, definition.runId);
  BOOST_CHECK_EQUAL(latest.completedWindows, 1U);
  BOOST_REQUIRE_EQUAL(completedRequests.size(), 2U);
  BOOST_CHECK(completedRequests.back() != oldRequest);
  BOOST_CHECK(latest.lastResultDigest != oldResult);
  BOOST_CHECK_EQUAL(executions.load(), 8U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test
