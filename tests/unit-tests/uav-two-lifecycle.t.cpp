#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/shared/UavMissionSession.hpp"
#include "NDNSF-UAV-APP/shared/UavNames.hpp"
#include "NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp"
#include "NDNSF-UAV-APP/shared/UavDetectorProvider.hpp"
#include "NDNSF-UAV-APP/shared/UavDiagnostics.hpp"
#include "NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp"

#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <string>

namespace ndn_service_framework::test {
namespace {

using namespace ndnsf::examples::uav;

UavEvidenceReference
makeEvidence(const ndn::Name& producer, const std::string& mission,
             const std::string& incident, const std::string& id = "frame-1")
{
  UavEvidenceReference value;
  value.producerIdentity = producer;
  value.streamId = "video-A";
  value.streamSessionEpoch = 3;
  value.firstSequence = 10;
  value.lastSequence = 12;
  value.windowStartMs = 1000;
  value.windowEndMs = 1100;
  value.version = 1;
  value.exactDataName = makeUavEvidenceName(producer, mission, incident, id,
                                            value.version);
  value.contentDigest = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  value.contentType = "application/octet-stream";
  value.retentionDeadlineMs = 5000;
  return value;
}

UavCollaborationJobRecord
makeJob(const ndn::Name& terminal, const std::string& mission,
        const std::string& incident, const std::string& attempt)
{
  UavCollaborationJobRecord job;
  job.requestId = ndn::Name("/request/1");
  job.missionId = mission;
  job.incidentId = incident;
  job.attemptId = attempt;
  job.ackDeadlineMs = 100;
  job.globalDeadlineMs = 1000;
  job.planDigest = "sha256:plan";
  job.terminalOwner = terminal;
  UavRoleAssignment evidenceRole;
  evidenceRole.role = "EvidenceSource";
  evidenceRole.providerIdentity = ndn::Name("/example/uav/drone/A");
  UavRoleAssignment detectorRole;
  detectorRole.role = "DetectorReporter";
  detectorRole.providerIdentity = terminal;
  detectorRole.terminalResponseOwner = true;
  job.assignments = {evidenceRole, detectorRole};
  return job;
}

BOOST_AUTO_TEST_SUITE(UavTwoLifecycle)

BOOST_AUTO_TEST_CASE(MissionAndCollaborationHaveSeparateBoundedLifecycles)
{
  auto session = UavMissionSession::create(
    "mission-1", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_CHECK(session.record().state == UavMissionSessionState::Planned);
  BOOST_CHECK(session.start());
  BOOST_CHECK(session.canIssueFlightControl());

  UavMissionPartRecord part;
  part.partId = "sector-A";
  part.sector = "A";
  BOOST_REQUIRE(session.addPart(part));
  BOOST_REQUIRE(session.assignPart("sector-A", ndn::Name("/example/uav/drone/A"), "attempt-1"));
  BOOST_REQUIRE(session.markPartExecuting("sector-A"));
  BOOST_REQUIRE(session.completePart("sector-A", "attempt-1", "sha256:response", {1, 2}));
  BOOST_CHECK(session.record().parts.front().state == UavMissionPartState::Completed);

  UavMissionPartRecord retryPart;
  retryPart.partId = "sector-B";
  BOOST_REQUIRE(session.addPart(retryPart));
  BOOST_REQUIRE(session.assignPart("sector-B", ndn::Name("/example/uav/drone/B"), "attempt-2"));
  BOOST_CHECK(!session.completePart("sector-B", "attempt-old", "sha256:late", {1}));

  UavIncidentRecord incident;
  incident.incidentId = "incident-1";
  incident.missionId = "mission-1";
  incident.triggerKind = "operator";
  incident.triggerTimeMs = 1000;
  incident.requestedCapability = "detector/high-quality";
  incident.currentAttemptId = "attempt-1";
  incident.evidence.push_back(makeEvidence(
    ndn::Name("/example/uav/drone/A"), "mission-1", "incident-1"));
  BOOST_REQUIRE(session.recordIncident(incident));

  const auto terminal = ndn::Name("/example/uav/drone/C");
  BOOST_REQUIRE(session.addJob(makeJob(terminal, "mission-1", "incident-1", "attempt-1")));
  BOOST_CHECK(session.record().jobs.front().state == UavCollaborationJobState::Created);
  BOOST_CHECK(session.record().state == UavMissionSessionState::Active);

  auto cancelled = UavMissionSession::create(
    "mission-cancel", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(cancelled.start());
  BOOST_REQUIRE(cancelled.cancel());
  BOOST_CHECK(cancelled.record().state == UavMissionSessionState::Cancelled);
  BOOST_CHECK(!cancelled.canIssueFlightControl());
}

BOOST_AUTO_TEST_CASE(MissionRequestTimeoutDoesNotEndMissionOrStreams)
{
  auto session = UavMissionSession::create(
    "mission-timeout", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(session.start());

  UavStreamBinding videoA;
  videoA.streamId = "video-A";
  videoA.producerIdentity = ndn::Name("/example/uav/drone/A");
  videoA.sessionEpoch = 1;
  videoA.firstCursor = 10;
  videoA.lastCursor = 20;
  videoA.active = true;
  BOOST_REQUIRE(session.bindStream(videoA));

  UavStreamBinding telemetryB = videoA;
  telemetryB.streamId = "telemetry-B";
  telemetryB.producerIdentity = ndn::Name("/example/uav/drone/B");
  telemetryB.sessionEpoch = 4;
  BOOST_REQUIRE(session.bindStream(telemetryB));

  UavMissionPartRecord completedPart;
  completedPart.partId = "sector-A";
  completedPart.sector = "A";
  BOOST_REQUIRE(session.addPart(completedPart));
  BOOST_REQUIRE(session.assignPart(
    "sector-A", ndn::Name("/example/uav/drone/A"), "part-attempt-1"));
  BOOST_REQUIRE(session.markPartExecuting("sector-A"));
  BOOST_REQUIRE(session.completePart(
    "sector-A", "part-attempt-1", "sha256:response", {1, 2}));

  UavMissionPartRecord pendingPart;
  pendingPart.partId = "sector-B";
  pendingPart.sector = "B";
  BOOST_REQUIRE(session.addPart(pendingPart));

  UavIncidentRecord incident;
  incident.incidentId = "incident-timeout";
  incident.missionId = "mission-timeout";
  incident.triggerKind = "telemetry";
  incident.triggerTimeMs = 100;
  incident.currentAttemptId = "attempt-timeout";
  incident.evidence.push_back(makeEvidence(
    ndn::Name("/example/uav/drone/A"), "mission-timeout", "incident-timeout"));
  BOOST_REQUIRE(session.recordIncident(incident));
  BOOST_REQUIRE(session.addJob(makeJob(
    ndn::Name("/example/uav/drone/C"), "mission-timeout", "incident-timeout",
    "attempt-timeout")));
  BOOST_REQUIRE(session.updateJob(
    ndn::Name("/request/1"), UavCollaborationJobState::AckCollecting));
  std::string reason;
  BOOST_REQUIRE(session.updateJob(
    ndn::Name("/request/1"), UavCollaborationJobState::TimedOut,
    "delivery", "request deadline exceeded", &reason));

  BOOST_CHECK(session.record().state == UavMissionSessionState::Active);
  BOOST_CHECK(session.record().parts[0].state == UavMissionPartState::Completed);
  BOOST_CHECK(session.record().streams.size() == 2);
  BOOST_CHECK(std::all_of(session.record().streams.begin(), session.record().streams.end(),
                          [](const auto& stream) { return stream.active; }));
  BOOST_CHECK(session.record().jobs.front().state == UavCollaborationJobState::TimedOut);
}

BOOST_AUTO_TEST_CASE(CompensationPreservesCompletedWorkAndCommandTimeoutNeedsReconciliation)
{
  auto session = UavMissionSession::create(
    "mission-reconcile", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(session.start());

  UavStreamBinding telemetry;
  telemetry.streamId = "telemetry-A";
  telemetry.producerIdentity = ndn::Name("/example/uav/drone/A");
  telemetry.sessionEpoch = 2;
  telemetry.active = true;
  BOOST_REQUIRE(session.bindStream(telemetry));

  UavMissionPartRecord completed;
  completed.partId = "sector-complete";
  completed.sector = "north";
  BOOST_REQUIRE(session.addPart(completed));
  BOOST_REQUIRE(session.assignPart(
    completed.partId, ndn::Name("/example/uav/drone/A"), "attempt-complete"));
  BOOST_REQUIRE(session.markPartExecuting(completed.partId));
  BOOST_REQUIRE(session.completePart(
    completed.partId, "attempt-complete", "sha256:complete", {1, 2, 3}));

  UavMissionPartRecord missing;
  missing.partId = "sector-missing";
  missing.sector = "south";
  missing.completedWaypoints = {10};
  BOOST_REQUIRE(session.addPart(missing));
  BOOST_REQUIRE(session.markPartMissing(missing.partId));
  BOOST_REQUIRE(session.transition(UavMissionSessionState::Degraded));
  BOOST_REQUIRE(session.transition(UavMissionSessionState::Compensating));
  BOOST_REQUIRE(session.compensateMissingParts());
  BOOST_REQUIRE(session.transition(UavMissionSessionState::Active));

  BOOST_CHECK(session.record().parts[0].state == UavMissionPartState::Completed);
  BOOST_CHECK_EQUAL(session.record().parts[0].responseDigest, "sha256:complete");
  BOOST_CHECK(session.record().parts[1].state == UavMissionPartState::Compensated);
  BOOST_CHECK(session.record().parts[1].completedWaypoints == std::vector<uint64_t>{10});

  const auto timeout = FlightCommandState::makeTimeout(
    "A", "takeoff", 1000, 2000, 1000);
  BOOST_CHECK(!timeout.isAccepted());
  BOOST_CHECK(timeout.isTimeout());

  TelemetryState telemetryState;
  telemetryState.droneId = "A";
  telemetryState.telemetryFreshness = "fresh";
  telemetryState.flightControllerState = "armed";
  telemetryState.landedStateName = "on-ground";
  SafetyState safety;
  safety.droneId = "A";
  safety.manualNeutralSent = "true";
  safety.manualReplayActive = "false";
  const auto stability = UavStabilityState::fromStates(
    timeout, std::nullopt, std::nullopt, telemetryState, safety, true, true);
  BOOST_CHECK_EQUAL(stability.commandTimeoutHandling, "operator-decision");
  BOOST_CHECK_EQUAL(safety.manualReplayActive, "false");

  // A timed-out physical command does not authorize replay. Recovery requires
  // an explicit MissionSession reconciliation pass before control is enabled.
  BOOST_REQUIRE(session.markRecovering());
  BOOST_CHECK(!session.canIssueFlightControl());
  BOOST_REQUIRE(session.reconcileVehicleAndStreams());
  BOOST_CHECK(session.canIssueFlightControl());
}

BOOST_AUTO_TEST_CASE(NamedEvidenceRejectsEndpointLikeOrMismatchedData)
{
  const auto producer = ndn::Name("/example/uav/drone/A");
  auto evidence = makeEvidence(producer, "mission-1", "incident-1");
  std::string reason;
  BOOST_CHECK(evidence.isValid(&reason));
  BOOST_CHECK(isUavProducerDataNameForContext(
    producer, evidence.exactDataName, "EVIDENCE", "mission-1", "incident-1"));
  BOOST_CHECK(!isUavProducerDataNameForContext(
    producer, evidence.exactDataName, "EVIDENCE", "mission-other", "incident-1"));

  evidence.exactDataName = ndn::Name("/10.0.0.1:8080/frame");
  BOOST_CHECK(!evidence.isValid(&reason));

  const auto endpointProducer = ndn::Name("/10.0.0.1:8080");
  BOOST_CHECK(!isUavProducerDataName(
    endpointProducer,
    makeUavEvidenceName(endpointProducer, "mission-1", "incident-1", "frame", 1),
    "EVIDENCE"));

  evidence = makeEvidence(producer, "mission-1", "incident-1");
  evidence.version = 2;
  BOOST_CHECK(!evidence.isValid(&reason));

  evidence = makeEvidence(producer, "mission-1", "incident-1");
  evidence.contentType.clear();
  BOOST_CHECK(!evidence.isValid(&reason));
}

BOOST_AUTO_TEST_CASE(EvidenceManifestIsDeterministicAndLineageBound)
{
  const auto producer = ndn::Name("/example/uav/drone/A");
  auto evidence = makeEvidence(producer, "mission-manifest", "incident-manifest", "window-1");
  const ndn::Buffer content = ndn::Buffer{
    reinterpret_cast<const uint8_t*>("fixture"), 7};
  (void)content;
  // The content excludes contentDigest, so the producer can derive the
  // immutable reference in one deterministic pre-publication pass.
  evidence.contentDigest =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const auto provisional = makeUavIncidentEvidenceContent(
    "mission-manifest", "incident-manifest", evidence);
  ndn::util::Sha256 digest;
  digest << std::string(reinterpret_cast<const char*>(provisional.data()), provisional.size());
  evidence.contentDigest = "sha256:" + digest.toString();
  const auto first = makeUavIncidentEvidenceContent(
    "mission-manifest", "incident-manifest", evidence);
  const auto second = makeUavIncidentEvidenceContent(
    "mission-manifest", "incident-manifest", evidence);
  BOOST_CHECK_EQUAL_COLLECTIONS(first.begin(), first.end(),
                                second.begin(), second.end());
  BOOST_CHECK(!first.empty());
  BOOST_CHECK(std::string(reinterpret_cast<const char*>(first.data()), first.size()).find(
                "mission_id=mission-manifest") != std::string::npos);
  ndn::util::Sha256 finalDigest;
  finalDigest << std::string(reinterpret_cast<const char*>(first.data()), first.size());
  BOOST_CHECK_EQUAL(evidence.contentDigest, "sha256:" + finalDigest.toString());

  auto wrong = evidence;
  wrong.exactDataName = makeUavEvidenceName(
    producer, "other-mission", "incident-manifest", "window-1", 1);
  BOOST_CHECK_THROW(makeUavIncidentEvidenceContent(
    "mission-manifest", "incident-manifest", wrong), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(TerminalReportNameMustMatchMissionAndIncident)
{
  const auto producer = ndn::Name("/example/uav/drone/A");
  const auto terminal = ndn::Name("/example/uav/drone/C");
  auto evidence = makeEvidence(producer, "mission-report", "incident-report", "window-1");
  UavTerminalReport report;
  report.missionId = "mission-report";
  report.incidentId = "incident-report";
  report.attemptId = "attempt-1";
  report.requestId = ndn::Name("/uav/incident/incident-report/attempt-1");
  report.planDigest = "sha256:plan";
  report.terminalOwner = terminal;
  report.selectedProvider = terminal;
  report.modelId = "detector-hq";
  report.modelDigest = "sha256:model";
  report.resultDigest = "sha256:result";
  report.status = "success";
  report.evidence.push_back(evidence);
  report.reportName = makeUavReportName(
    terminal, "mission-report", "incident-report", "attempt-1", 1);
  BOOST_CHECK(report.isValid());

  report.reportName = makeUavReportName(
    terminal, "other-mission", "incident-report", "attempt-1", 1);
  std::string reason;
  BOOST_CHECK(!report.isValid(&reason));
  BOOST_CHECK(!reason.empty());
}

BOOST_AUTO_TEST_CASE(SnapshotRestoreEntersRecoveryAndBlocksCommands)
{
  auto session = UavMissionSession::create(
    "mission-restore", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(session.start());
  const auto snapshot = session.snapshot();
  auto restored = UavMissionSession::restore(snapshot, 8);
  BOOST_REQUIRE(restored.has_value());
  BOOST_CHECK(restored->record().state == UavMissionSessionState::Recovering);
  BOOST_CHECK(!restored->canIssueFlightControl());
  BOOST_REQUIRE(restored->reconcileVehicleAndStreams());
  BOOST_CHECK(restored->canIssueFlightControl());

  auto tampered = snapshot;
  tampered.replace(tampered.find("mission-restore"), std::string("mission-restore").size(),
                   "mission-tampered");
  BOOST_CHECK(!UavMissionSession::restore(tampered, 8).has_value());
  BOOST_CHECK(!UavMissionSession::restore("", 8).has_value());
}

BOOST_AUTO_TEST_CASE(MalformedStatesAndDuplicateTerminalRolesAreRejected)
{
  BOOST_CHECK(!parseUavMissionSessionState("not-a-state").has_value());
  BOOST_CHECK(!parseUavMissionPartState("not-a-state").has_value());
  BOOST_CHECK(!parseUavCollaborationJobState("not-a-state").has_value());

  auto session = UavMissionSession::create(
    "mission-roles", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(session.start());
  UavIncidentRecord incident;
  incident.incidentId = "incident-roles";
  incident.missionId = "mission-roles";
  incident.triggerKind = "operator";
  incident.triggerTimeMs = 1;
  incident.currentAttemptId = "attempt-1";
  BOOST_REQUIRE(session.recordIncident(incident));
  auto invalid = makeJob(ndn::Name("/example/uav/drone/C"),
                         "mission-roles", "incident-roles", "attempt-1");
  invalid.assignments.front().terminalResponseOwner = true;
  BOOST_CHECK(!session.addJob(std::move(invalid)));
}

BOOST_AUTO_TEST_CASE(DetectorSelectionUsesVerifiedCapabilityAndExplicitFallback)
{
  UavDetectorRequirement requirement;
  requirement.modelId = "detector-hq";
  requirement.qualityProfile = "high";
  requirement.preferredDeviceClass = "gpu";
  requirement.maxAckAgeMs = 100;
  requirement.maxQueueDepth = 8;

  UavDetectorCandidate stale;
  stale.ackVerified = true;
  stale.capability.providerIdentity = ndn::Name("/example/uav/drone/A");
  stale.capability.modelId = "detector-hq";
  stale.capability.modelDigest = "sha256:model";
  stale.capability.qualityProfile = "high";
  stale.capability.deviceClass = "gpu";
  stale.capability.ready = true;
  stale.capability.evidenceAccess = true;
  stale.capability.snapshotTimeMs = 100;
  stale.capability.queueDepth = 1;

  UavDetectorCandidate ready = stale;
  ready.capability.providerIdentity = ndn::Name("/example/uav/drone/C");
  ready.capability.snapshotTimeMs = 950;
  ready.capability.queueDepth = 2;
  ready.capability.estimatedStartMs = 1000;

  auto decision = selectUavDetector(requirement, {stale, ready}, 1000);
  BOOST_CHECK(decision.selected);
  BOOST_CHECK(decision.providerIdentity == ready.capability.providerIdentity);
  BOOST_CHECK(!decision.fallbackUsed);

  auto unverified = ready;
  unverified.ackVerified = false;
  decision = selectUavDetector(requirement, {unverified}, 1000);
  BOOST_CHECK(!decision.selected);

  ready.capability.ready = false;
  UavDetectorCandidate fallback;
  fallback.ackVerified = true;
  fallback.capability.providerIdentity = ndn::Name("/example/uav/gs");
  fallback.capability.modelId = requirement.modelId;
  fallback.capability.modelDigest = "sha256:model";
  fallback.capability.qualityProfile = requirement.qualityProfile;
  fallback.capability.deviceClass = "cpu";
  fallback.capability.ready = true;
  fallback.capability.evidenceAccess = true;
  fallback.capability.snapshotTimeMs = 950;
  fallback.capability.estimatedStartMs = 1000;
  decision = selectUavDetector(requirement, {ready}, 1000,
                               fallback.capability.providerIdentity, fallback);
  BOOST_CHECK(decision.selected);
  BOOST_CHECK(decision.fallbackUsed);
  BOOST_CHECK(decision.providerIdentity == ndn::Name("/example/uav/gs"));

  BOOST_CHECK(!selectUavDetector(requirement, {ready}, 1000,
                                 ndn::Name("/example/uav/gs")).selected);

  decision = selectUavDetector(requirement, {ready}, 1000);
  BOOST_CHECK(!decision.selected);
}

BOOST_AUTO_TEST_CASE(DetectorProviderBindsCapabilityAndVerifiesNamedEvidence)
{
  const auto producer = ndn::Name("/example/uav/drone/A");
  auto evidence = makeEvidence(producer, "mission-1", "incident-1", "frame-2");
  const std::string bytes = "frame-bytes";
  ndn::util::Sha256 digest;
  digest << bytes;
  evidence.contentDigest = "sha256:" + digest.toString();

  UavDetectorProvider provider(UavDetectorProviderConfig{
    ndn::Name("/example/uav/drone/C"), "detector-hq", "sha256:model", "high", "gpu", true});
  const auto capability = provider.capability(1000, 2, true);
  BOOST_CHECK(capability.ready);
  BOOST_CHECK(capability.providerIdentity == ndn::Name("/example/uav/drone/C"));
  BOOST_CHECK_EQUAL(capability.modelId, "detector-hq");
  BOOST_CHECK_EQUAL(capability.deviceClass, "gpu");

  ndn::KeyChain keyChain("pib-memory:uav-detector", "tpm-memory:uav-detector");
  keyChain.createIdentity(producer);
  const auto producerCertificate = keyChain.getPib().getIdentity(producer)
                                     .getDefaultKey().getDefaultCertificate();
  ndn::Data fetchedData(evidence.exactDataName);
  fetchedData.setContent(ndn::Buffer(bytes.begin(), bytes.end()));
  keyChain.sign(fetchedData);
  const auto verified = provider.verifyFetchedData(
    evidence, fetchedData, producerCertificate);
  BOOST_REQUIRE(verified.has_value());
  auto invalidEvidence = evidence;
  invalidEvidence.contentType.clear();
  BOOST_CHECK(!provider.verifyFetchedData(
    invalidEvidence, fetchedData, producerCertificate));
  const auto contextValidated = provider.acceptValidatedContent(
    evidence, ndn::Buffer(bytes.begin(), bytes.end()), producer);
  BOOST_REQUIRE(contextValidated.has_value());
  BOOST_CHECK(provider.execute(*contextValidated).success);
  auto wrongContextDigest = *contextValidated;
  wrongContextDigest.content[0] = 'X';
  BOOST_CHECK(!provider.execute(wrongContextDigest).success);
  const auto execution = provider.execute(*verified);
  BOOST_CHECK(execution.success);
  BOOST_CHECK(!execution.resultDigest.empty());

  auto tampered = *verified;
  tampered.content[0] = 'X';
  const auto rejected = provider.execute(tampered);
  BOOST_CHECK(!rejected.success);

  UavVerifiedEvidence descriptorOnly;
  descriptorOnly.reference = evidence;
  descriptorOnly.content = ndn::Buffer(bytes.begin(), bytes.end());
  BOOST_CHECK(!provider.execute(descriptorOnly).success);
}

BOOST_AUTO_TEST_CASE(ConfigUsesLogicalNamesAndIncidentPermission)
{
  const auto config = loadUavRuntimeConfig("NDNSF-UAV-APP/configs/uav_runtime.conf");
  BOOST_CHECK(config.serviceIncidentAnalyze == ndn::Name("/UAV/Incident/Analyze"));
  BOOST_CHECK(config.serviceIncidentAnalyze.toUri().find("10.") == std::string::npos);
  BOOST_CHECK(config.serviceIncidentAnalyze.toUri().find(":") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(DiagnosticTracePreservesRegisteredLineageAndRejectsEndpoints)
{
  UavTraceRecorder recorder(4, 2);
  recorder.registerIncident("incident-1");
  UavTraceEvent event;
  event.timestampMs = 1;
  event.missionId = "mission-1";
  event.incidentId = "incident-1";
  event.requestId = ndn::Name("/request/trace");
  event.stage = UavTraceStage::EvidenceDataVerified;
  event.requestedDataName = makeUavEvidenceName(
    ndn::Name("/example/uav/drone/A"), "mission-1", "incident-1", "frame", 1);
  event.returnedDataName = event.requestedDataName;
  event.producerIdentity = ndn::Name("/example/uav/drone/A");
  event.signerIdentity = event.producerIdentity;
  event.version = 1;
  BOOST_REQUIRE(recorder.record(event));
  BOOST_CHECK(UavTraceRecorder::hasCompleteLineage(recorder.events().front()));

  event.detail = "host=10.0.0.1 port=6363";
  std::string reason;
  BOOST_CHECK(!recorder.record(event, &reason));
  BOOST_CHECK(!reason.empty());
}

BOOST_AUTO_TEST_CASE(BoundedWorkQueueMakesOverloadExplicit)
{
  UavBoundedWorkQueue queue(2);
  BOOST_REQUIRE(queue.tryPush("control-independent-1"));
  BOOST_REQUIRE(queue.tryPush("control-independent-2"));
  BOOST_CHECK(!queue.tryPush("detector-overload"));
  BOOST_CHECK_EQUAL(queue.size(), 2U);
  BOOST_CHECK_EQUAL(queue.dropped(), 1U);
  BOOST_CHECK(queue.overloaded());
  BOOST_CHECK_EQUAL(*queue.tryPop(), "control-independent-1");
  BOOST_REQUIRE(queue.tryPush("detector-after-pop"));
  BOOST_CHECK_EQUAL(queue.size(), 2U);
}

BOOST_AUTO_TEST_CASE(ResourcePressureDoesNotBlockCommandOrJobLifecycle)
{
  UavBoundedWorkQueue detectorQueue(4);
  for (size_t i = 0; i < 1024; ++i) {
    (void)detectorQueue.tryPush("detector-job-" + std::to_string(i));
  }
  BOOST_CHECK_EQUAL(detectorQueue.size(), detectorQueue.capacity());
  BOOST_CHECK_EQUAL(detectorQueue.dropped(), 1020U);
  BOOST_CHECK(detectorQueue.overloaded());

  auto session = UavMissionSession::create(
    "mission-pressure", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(session.start());
  UavIncidentRecord incident;
  incident.incidentId = "incident-pressure";
  incident.missionId = "mission-pressure";
  incident.triggerKind = "queue-pressure";
  incident.triggerTimeMs = 1;
  incident.currentAttemptId = "attempt-pressure";
  BOOST_REQUIRE(session.recordIncident(incident));
  auto job = makeJob(ndn::Name("/example/uav/drone/C"),
                     "mission-pressure", "incident-pressure", "attempt-pressure");
  BOOST_REQUIRE(session.addJob(job));

  // Control/job state remains independently progressable while detector work
  // is rejected at the bounded queue boundary.
  BOOST_REQUIRE(session.updateJob(
    job.requestId, UavCollaborationJobState::AckCollecting));
  std::string reason;
  BOOST_REQUIRE(session.updateJob(
    job.requestId, UavCollaborationJobState::TimedOut,
    "execution", "detector queue overloaded", &reason));
  const auto command = FlightCommandState::makeTimeout(
    "A", "land", 100, 200, 100);
  BOOST_CHECK(command.isTimeout());
  BOOST_CHECK(!command.isAccepted());
  BOOST_CHECK(session.record().state == UavMissionSessionState::Active);
  BOOST_CHECK(session.record().jobs.front().state == UavCollaborationJobState::TimedOut);
}

BOOST_AUTO_TEST_CASE(OperatorStateKeepsMissionAndCollaborationViewsSeparate)
{
  GroundStationRuntimeState state;
  state.selectedDroneId = "C";
  state.selectedDroneLocked = true;
  state.updatedMs = 42;

  auto& drone = state.ensureDrone("C");
  drone.connection = RuntimeConnectionState::Online;
  drone.missionReady = RuntimeAvailability::Available;
  drone.mission = MissionState{};
  drone.mission->missionId = "mission-176";
  drone.mission->partId = "sector-2";
  drone.mission->phase = "executing";
  drone.missionProgress = MissionProgressState{};
  drone.missionProgress->taskId = "mission-176";
  drone.missionProgress->phase = "part-2/4";
  drone.commandStates.emplace("takeoff", RuntimeCommandSnapshot{
    "takeoff", CommandLifecycle::Success, "acknowledged", 41, 18, 500});
  drone.appendCommandHistory(RuntimeCommandSnapshot{
    "land", CommandLifecycle::Timeout, "awaiting reconciliation", 40, 0, 500}, 4);

  state.collaboration.missionId = "mission-176";
  state.collaboration.incidentId = "incident-9";
  state.collaboration.attemptId = "attempt-2";
  state.collaboration.requestId = "/example/request/9";
  state.collaboration.stage = "response_received";
  state.collaboration.selectedProvider = "/example/uav/drone/C";
  state.collaboration.terminalOwner = "/example/uav/drone/C";
  state.collaboration.failureStage = "";
  state.collaboration.fallbackMode = "disabled";
  state.collaboration.deadlineMs = 5000;

  BOOST_CHECK(state.findDrone("C") != nullptr);
  BOOST_CHECK_EQUAL(state.findDrone("C")->mission->missionId, "mission-176");
  BOOST_CHECK_EQUAL(state.findDrone("C")->missionProgress->phase, "part-2/4");
  BOOST_CHECK(state.findDrone("C")->commandStates.at("takeoff").lifecycle ==
              CommandLifecycle::Success);
  BOOST_CHECK_EQUAL(state.findDrone("C")->commandHistory.size(), 1U);
  BOOST_CHECK(state.findDrone("C")->commandHistory.front().lifecycle ==
              CommandLifecycle::Timeout);
  BOOST_CHECK_EQUAL(state.collaboration.stage, "response_received");
  BOOST_CHECK_EQUAL(state.collaboration.incidentId, "incident-9");
  BOOST_CHECK_EQUAL(state.collaboration.attemptId, "attempt-2");
  BOOST_CHECK_EQUAL(state.collaboration.terminalOwner, "/example/uav/drone/C");
  BOOST_CHECK_EQUAL(state.collaboration.deadlineMs, 5000U);
  BOOST_CHECK(state.selectedDroneLocked);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndn_service_framework::test
