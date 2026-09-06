#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp"
#include "NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp"
#include "ndnsf-integration-fixture.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <boost/asio/io_context.hpp>

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <set>
#include <thread>

namespace ndnsf::examples::uav::tests {
namespace {

BOOST_AUTO_TEST_SUITE(UavCollaborationFlow)

BOOST_AUTO_TEST_CASE(MissionSessionOutlivesTimedOutRequestAndKeepsStreamsBound)
{
  auto mission = UavMissionSession::create(
    "mission-integration-timeout", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(mission.start());

  UavStreamBinding video;
  video.streamId = "video-scout-A";
  video.producerIdentity = ndn::Name("/example/uav/drone/A");
  video.sessionEpoch = 1;
  video.firstCursor = 1;
  video.lastCursor = 20;
  video.active = true;
  BOOST_REQUIRE(mission.bindStream(video));

  UavStreamBinding telemetry = video;
  telemetry.streamId = "telemetry-scout-B";
  telemetry.producerIdentity = ndn::Name("/example/uav/drone/B");
  telemetry.sessionEpoch = 2;
  telemetry.firstCursor = 100;
  telemetry.lastCursor = 120;
  BOOST_REQUIRE(mission.bindStream(telemetry));

  UavMissionPartRecord partA;
  partA.partId = "sector-A";
  partA.sector = "A";
  BOOST_REQUIRE(mission.addPart(partA));
  BOOST_REQUIRE(mission.assignPart(
    "sector-A", ndn::Name("/example/uav/drone/A"), "part-attempt-A"));
  BOOST_REQUIRE(mission.markPartExecuting("sector-A"));
  BOOST_REQUIRE(mission.completePart(
    "sector-A", "part-attempt-A", "sha256:sector-a", {1, 2, 3}));

  UavMissionPartRecord partB;
  partB.partId = "sector-B";
  partB.sector = "B";
  BOOST_REQUIRE(mission.addPart(partB));

  UavIncidentRecord incident;
  incident.incidentId = "incident-timeout";
  incident.missionId = "mission-integration-timeout";
  incident.triggerKind = "telemetry";
  incident.triggerTimeMs = 100;
  incident.currentAttemptId = "attempt-timeout";
  BOOST_REQUIRE(mission.recordIncident(incident));

  UavCollaborationJobRecord job;
  job.requestId = ndn::Name("/request/timeout");
  job.missionId = incident.missionId;
  job.incidentId = incident.incidentId;
  job.attemptId = incident.currentAttemptId;
  job.ackDeadlineMs = 1100;
  job.globalDeadlineMs = 5100;
  job.planDigest = "sha256:plan";
  job.terminalOwner = ndn::Name("/example/uav/drone/C");
  UavRoleAssignment terminal;
  terminal.role = "DetectorReporter";
  terminal.providerIdentity = job.terminalOwner;
  terminal.terminalResponseOwner = true;
  job.assignments.push_back(terminal);
  BOOST_REQUIRE(mission.addJob(job));
  BOOST_REQUIRE(mission.updateJob(
    job.requestId, UavCollaborationJobState::AckCollecting));
  std::string reason;
  BOOST_REQUIRE(mission.updateJob(
    job.requestId, UavCollaborationJobState::TimedOut,
    "delivery", "request deadline exceeded", &reason));

  BOOST_CHECK(mission.record().state == UavMissionSessionState::Active);
  BOOST_CHECK(mission.record().parts[0].state == UavMissionPartState::Completed);
  BOOST_CHECK(mission.record().parts[1].state == UavMissionPartState::Pending);
  BOOST_CHECK_EQUAL(mission.record().streams.size(), 2U);
  BOOST_CHECK(std::all_of(mission.record().streams.begin(), mission.record().streams.end(),
                          [](const auto& stream) { return stream.active; }));
  BOOST_CHECK(mission.record().jobs.front().state == UavCollaborationJobState::TimedOut);
}

BOOST_AUTO_TEST_CASE(CompensationAndCommandTimeoutRequireAuthoritativeRecovery)
{
  auto mission = UavMissionSession::create(
    "mission-integration-reconcile", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(mission.start());

  UavStreamBinding stream;
  stream.streamId = "video-scout-A";
  stream.producerIdentity = ndn::Name("/example/uav/drone/A");
  stream.sessionEpoch = 1;
  stream.active = true;
  BOOST_REQUIRE(mission.bindStream(stream));

  UavMissionPartRecord complete;
  complete.partId = "sector-complete";
  complete.sector = "north";
  BOOST_REQUIRE(mission.addPart(complete));
  BOOST_REQUIRE(mission.assignPart(
    complete.partId, ndn::Name("/example/uav/drone/A"), "attempt-complete"));
  BOOST_REQUIRE(mission.markPartExecuting(complete.partId));
  BOOST_REQUIRE(mission.completePart(
    complete.partId, "attempt-complete", "sha256:sector-a", {1, 2}));

  UavMissionPartRecord missing;
  missing.partId = "sector-missing";
  missing.sector = "south";
  missing.completedWaypoints = {7};
  BOOST_REQUIRE(mission.addPart(missing));
  BOOST_REQUIRE(mission.markPartMissing(missing.partId));
  BOOST_REQUIRE(mission.transition(UavMissionSessionState::Degraded));
  BOOST_REQUIRE(mission.transition(UavMissionSessionState::Compensating));
  BOOST_REQUIRE(mission.compensateMissingParts());
  BOOST_REQUIRE(mission.transition(UavMissionSessionState::Active));

  BOOST_CHECK(mission.record().parts[0].state == UavMissionPartState::Completed);
  BOOST_CHECK_EQUAL(mission.record().parts[0].responseDigest, "sha256:sector-a");
  BOOST_CHECK(mission.record().parts[1].state == UavMissionPartState::Compensated);

  const auto timeout = FlightCommandState::makeTimeout(
    "A", "takeoff", 1000, 2000, 1000);
  TelemetryState vehicle;
  vehicle.droneId = "A";
  vehicle.telemetryFreshness = "fresh";
  vehicle.flightControllerState = "armed";
  SafetyState safety;
  safety.droneId = "A";
  safety.manualReplayActive = "false";
  safety.manualNeutralSent = "true";
  const auto stability = UavStabilityState::fromStates(
    timeout, std::nullopt, std::nullopt, vehicle, safety, true, true);
  BOOST_CHECK(timeout.isTimeout());
  BOOST_CHECK(!timeout.isAccepted());
  BOOST_CHECK_EQUAL(stability.commandTimeoutHandling, "operator-decision");
  BOOST_CHECK_EQUAL(safety.manualReplayActive, "false");

  BOOST_REQUIRE(mission.markRecovering());
  BOOST_CHECK(!mission.canIssueFlightControl());
  BOOST_REQUIRE(mission.reconcileVehicleAndStreams());
  BOOST_CHECK(mission.canIssueFlightControl());
}

BOOST_AUTO_TEST_CASE(CpuFixtureRunsTwoLifecyclesWithNamedEvidence)
{
  ndn::KeyChain keyChain("pib-memory:uav-cpu", "tpm-memory:uav-cpu");
  const auto producer = ndn::Name("/example/uav/drone/A");
  keyChain.createIdentity(producer);
  const auto producerCertificate = keyChain.getPib().getIdentity(producer)
                                     .getDefaultKey().getDefaultCertificate();
  auto mission = UavMissionSession::create(
    "mission-cpu", ndn::Name("/example/uav/gs"), 60000, 16);
  BOOST_REQUIRE(mission.start());

  UavCollaborationParticipant scout(ndn::Name("/example/uav/drone/A"),
                                    "EvidenceSource");
  const ndn::Buffer frame{'f', 'r', 'a', 'm', 'e'};
  std::string reason;
  const auto evidence = scout.freezeEvidence(
    "mission-cpu", "incident-cpu", "window-1", 1, "video-A", 3,
    10, 12, 1000, 1100, frame, 5000, "image/jpeg", &reason);
  BOOST_REQUIRE_MESSAGE(evidence.has_value(), reason);

  UavIncidentRecord incident;
  incident.incidentId = "incident-cpu";
  incident.missionId = "mission-cpu";
  incident.triggerKind = "operator";
  incident.triggerTimeMs = 1000;
  incident.requestedCapability = "detector/high";
  incident.currentAttemptId = "attempt-1";
  incident.evidence.push_back(*evidence);
  BOOST_REQUIRE(mission.recordIncident(incident, &reason));

  UavIncidentCoordinator coordinator(
    mission, incident, UavIncidentCoordinatorConfig{1000, 5000, 4, 4, 1, {}});
  BOOST_REQUIRE(coordinator.begin(ndn::Name("/request/incident-cpu"), 100, &reason));
  BOOST_CHECK(coordinator.job().state == UavCollaborationJobState::AckCollecting);
  BOOST_REQUIRE(coordinator.closeAcks(1100, &reason));

  UavRoleAssignment evidenceRole;
  evidenceRole.role = "EvidenceSource";
  evidenceRole.providerIdentity = scout.providerIdentity();
  evidenceRole.evidence.push_back(*evidence);

  UavDetectorProvider detector({ndn::Name("/example/uav/drone/C"),
                                "detector-hq", "sha256:model", "high", "gpu", true});
  const auto detectorIdentity = detector.config().providerIdentity;
  UavCollaborationParticipant compute(detectorIdentity, "DetectorReporter", std::move(detector));
  UavRoleAssignment detectorRole;
  detectorRole.role = "DetectorReporter";
  detectorRole.providerIdentity = compute.providerIdentity();
  detectorRole.terminalResponseOwner = true;
  detectorRole.evidence.push_back(*evidence);

  BOOST_REQUIRE(coordinator.commitPlan(
    {evidenceRole, detectorRole}, compute.providerIdentity(), "sha256:plan", &reason));
  BOOST_REQUIRE(coordinator.selectProvider(compute.providerIdentity(), &reason));
  BOOST_REQUIRE(coordinator.markEvidenceReady(&reason));
  BOOST_REQUIRE(coordinator.markExecuting(&reason));
  BOOST_REQUIRE(coordinator.markReporting(&reason));

  ndn::Data evidenceData(evidence->exactDataName);
  evidenceData.setContent(frame);
  keyChain.sign(evidenceData);
  UavDetectorProvider verifier(
    {compute.providerIdentity(), "detector-hq", "sha256:model", "high", "gpu", true});
  const auto verifiedEvidence = verifier.verifyFetchedData(
    *evidence, evidenceData, producerCertificate, &reason);
  BOOST_REQUIRE_MESSAGE(verifiedEvidence.has_value(), reason);
  const auto report = compute.executeDetector(coordinator.job(), *verifiedEvidence, 1, &reason);
  BOOST_REQUIRE_MESSAGE(report.has_value(), reason);
  BOOST_REQUIRE(coordinator.acceptReport(*report, 1200, &reason));
  BOOST_CHECK(coordinator.job().state == UavCollaborationJobState::Succeeded);
  BOOST_CHECK(mission.record().state == UavMissionSessionState::Active);
  BOOST_CHECK(!mission.record().incidents.front().acceptedTerminalReportDigest.empty());

  // The same terminal report cannot be accepted a second time, and mission
  // progress remains independent from this finite request.
  BOOST_CHECK(!mission.acceptTerminalReport(*report, &reason));
}

BOOST_AUTO_TEST_CASE(CpuFixtureFailsClosedForUnreadyDetectorAndLateAttempt)
{
  auto mission = UavMissionSession::create(
    "mission-failure", ndn::Name("/example/uav/gs"), 60000, 8);
  BOOST_REQUIRE(mission.start());
  UavIncidentRecord incident;
  incident.incidentId = "incident-failure";
  incident.missionId = "mission-failure";
  incident.triggerKind = "telemetry";
  incident.triggerTimeMs = 100;
  incident.requestedCapability = "detector/high";
  incident.currentAttemptId = "attempt-new";
  UavCollaborationParticipant scout(ndn::Name("/example/uav/drone/A"),
                                    "EvidenceSource");
  const ndn::Buffer retryFrame{'r', 'e', 't', 'r', 'y'};
  const auto retryEvidence = scout.freezeEvidence(
    "mission-failure", "incident-failure", "window-1", 1, "video-A", 1,
    1, 1, 100, 101, retryFrame, 5000, "image/jpeg");
  BOOST_REQUIRE(retryEvidence.has_value());
  incident.evidence.push_back(*retryEvidence);
  BOOST_REQUIRE(mission.recordIncident(incident));

  UavIncidentCoordinator coordinator(mission, incident);
  std::string reason;
  BOOST_REQUIRE(coordinator.begin(ndn::Name("/request/incident-failure"), 0, &reason));
  BOOST_REQUIRE(coordinator.closeAcks(1000, &reason));
  UavRoleAssignment role;
  role.role = "DetectorReporter";
  role.providerIdentity = ndn::Name("/example/uav/drone/C");
  role.terminalResponseOwner = true;
  BOOST_REQUIRE(!coordinator.commitPlan({role}, role.providerIdentity,
                                         "sha256:plan", &reason));
  BOOST_REQUIRE(coordinator.fail("plan", "no named evidence"));
  BOOST_CHECK(coordinator.job().state == UavCollaborationJobState::Failed);
  BOOST_CHECK(!coordinator.retryAnalysis(UavCollaborationFailureStage::AckClosure,
                                         ndn::Name("/request/retry-ack"),
                                         "attempt-retry-ack", 1100, &reason));
  BOOST_REQUIRE(coordinator.retryAnalysis(UavCollaborationFailureStage::Plan,
                                          ndn::Name("/request/retry-plan"),
                                          "attempt-retry-plan", 1100, &reason));
  BOOST_CHECK_EQUAL(coordinator.retriesUsed(), 1U);
  BOOST_REQUIRE(coordinator.fail("execution", "detector unavailable"));
  BOOST_CHECK(!coordinator.retryAnalysis(UavCollaborationFailureStage::Execution,
                                         ndn::Name("/request/retry-again"),
                                         "attempt-retry-again", 1200, &reason));
}

BOOST_AUTO_TEST_CASE(NamedEvidenceUsesInterestDrivenSignedData)
{
  boost::asio::io_context consumerIo;
  ndn::KeyChain keyChain("pib-memory:uav-evidence", "tpm-memory:uav-evidence");
  const auto producer = ndn::Name("/example/uav/drone/A");
  const auto identity = keyChain.createIdentity(producer);
  keyChain.setDefaultIdentity(identity);
  const auto producerCertificate = keyChain.getPib().getIdentity(producer)
                                     .getDefaultKey().getDefaultCertificate();
  ndn::DummyClientFace::Options options;
  options.enablePacketLogging = true;
  options.enableRegistrationReply = true;
  ndn::DummyClientFace consumerFace(consumerIo, keyChain, options);

  UavCollaborationParticipant scout(producer, "EvidenceSource");
  const ndn::Buffer frame{'n', 'd', 'n', '-', 'd', 'a', 't', 'a'};
  std::string reason;
  const auto evidence = scout.freezeEvidence(
    "mission-wire", "incident-wire", "frame-1", 1, "video-A", 1,
    7, 7, 100, 101, frame, 1000, "image/jpeg", &reason);
  BOOST_REQUIRE_MESSAGE(evidence.has_value(), reason);

  bool requested = false;
  bool received = false;
  bool valid = false;
  bool providerVerified = false;
  UavDetectorProvider detector({ndn::Name("/example/uav/drone/C"),
                                "detector-hq", "sha256:model", "high", "gpu", true});
  auto interestForward = consumerFace.onSendInterest.connect(
    [&](const ndn::Interest& interest) {
      requested = true;
      ndn::Data data(interest.getName());
      data.setContent(frame);
      keyChain.sign(data);
      consumerFace.receive(data);
    });
  consumerFace.expressInterest(
    ndn::Interest(evidence->exactDataName),
    [&](const auto&, const ndn::Data& data) {
      received = true;
      valid = data.getName() == evidence->exactDataName &&
              data.getContent().value_size() == frame.size() &&
              std::equal(frame.begin(), frame.end(), data.getContent().value_begin());
      providerVerified = detector.execute(*evidence, data, producerCertificate).success;
    },
    [&](const auto&, const auto&) { received = true; },
    [&](const auto&) { received = true; });
  for (int i = 0; i < 200 && !received; ++i) {
    consumerFace.processEvents(ndn::time::milliseconds(1));
    consumerIo.restart();
  }
  BOOST_CHECK(requested);
  BOOST_CHECK(received);
  BOOST_CHECK(valid);
  BOOST_CHECK(providerVerified);
  BOOST_CHECK(interestForward.isConnected());

  auto tampered = ndn::Data(evidence->exactDataName);
  tampered.setContent(frame);
  keyChain.sign(tampered);
  auto signature = ndn::Buffer(tampered.getSignatureValue().value_begin(),
                               tampered.getSignatureValue().value_end());
  BOOST_REQUIRE(!signature.empty());
  signature[0] ^= 0x01;
  tampered.setSignatureValue(signature);
  BOOST_CHECK(!detector.execute(*evidence, tampered, producerCertificate).success);
}

BOOST_AUTO_TEST_CASE(MultiSegmentNamedEvidenceUsesExactValidatedFetchForTwoConsumers)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/UAV/IncidentEvidence");
  profile.providerCount = 3;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() ==
                ndn_service_framework::test::EnvironmentStatus::Ready);
  environment.disconnectProviderPeerTransportForTest();
  environment.enableProductionIngressForTest();

  // The fixture normally wires the Provider mesh during bootstrap.  Replace
  // it with explicit in-process bridges so every Interest/Data still crosses
  // the real DummyFace boundary while the test can observe exact names.
  std::vector<ndn::signal::ScopedConnection> bridges;
  for (size_t source = 0; source < environment.providerCount(); ++source) {
    for (size_t destination = 0; destination < environment.providerCount();
         ++destination) {
      if (source == destination) {
        continue;
      }
      bridges.emplace_back(environment.providerFace(source).onSendInterest.connect(
        [&environment, destination] (const ndn::Interest& interest) {
          auto packet = interest;
          boost::asio::post(
              environment.providerFace(destination).getIoContext(),
              [&environment, destination, packet = std::move(packet)] {
                environment.providerFace(destination).receive(packet);
              });
        }));
      bridges.emplace_back(environment.providerFace(source).onSendData.connect(
        [&environment, destination] (const ndn::Data& data) {
          auto packet = data;
          boost::asio::post(
              environment.providerFace(destination).getIoContext(),
              [&environment, destination, packet = std::move(packet)] {
                environment.providerFace(destination).receive(packet);
              });
        }));
    }
  }

  auto& producer = environment.provider(0);
  auto& consumerOne = environment.provider(1);
  auto& consumerTwo = environment.provider(2);
  const ndn::Name requestId("/request/uav-multi-segment-evidence");
  const std::string keyScope = "incident-evidence";
  const ndn::Buffer scopeKey(32, 0x41);
  ndn_service_framework::ServiceProvider::CollaborationAssignment producerAssignment;
  producerAssignment.role = "EvidenceSource";
  producerAssignment.service = profile.serviceName;
  producerAssignment.scopeKeys.emplace(keyScope, scopeKey);
  ndn_service_framework::ServiceProvider::CollaborationAssignment consumerAssignment = producerAssignment;
  consumerAssignment.role = "DetectorReporter";
  ndn_service_framework::ServiceProvider::CollaborationContext producerContext(
      producer, profile.userIdentity, requestId,
      ndn_service_framework::RequestMessage(), producerAssignment);
  ndn_service_framework::ServiceProvider::CollaborationContext consumerOneContext(
      consumerOne, profile.userIdentity, requestId,
      ndn_service_framework::RequestMessage(), consumerAssignment);
  ndn_service_framework::ServiceProvider::CollaborationContext consumerTwoContext(
      consumerTwo, profile.userIdentity, requestId,
      ndn_service_framework::RequestMessage(), consumerAssignment);

  UavCollaborationParticipant scout(producer.getName(), "EvidenceSource");
  ndn::Buffer frame(12000);
  for (size_t i = 0; i < frame.size(); ++i) {
    frame[i] = static_cast<uint8_t>((i * 13 + 7) & 0xff);
  }
  std::string reason;
  const auto evidence = scout.freezeEvidence(
      "mission-large", "incident-large", "window-1", 7, "video-scout-A", 3,
      100, 140, 1000, 2400, frame, 60000, "application/octet-stream", &reason);
  BOOST_REQUIRE_MESSAGE(evidence.has_value(), reason);

  std::set<std::string> publishedSegments;
  auto observePublished = environment.providerFace(0).onSendData.connect(
      [&] (const ndn::Data& data) {
        if (evidence->exactDataName.isPrefixOf(data.getName()) &&
            !data.getName().empty() && data.getName()[-1].isSegment()) {
          publishedSegments.insert(data.getName().toUri());
        }
      });
  const auto publishedName = producerContext.publishLargeNamed(
      keyScope, evidence->exactDataName, frame, 256, 60000);
  BOOST_REQUIRE_EQUAL(publishedName, evidence->exactDataName);
  environment.pumpUntil([&] { return publishedSegments.size() >= 2U; });
  BOOST_REQUIRE_GE(publishedSegments.size(), 2U);
  const auto expectedSegments = publishedSegments.size();

  std::set<std::string> exactInterests;
  bool everyInterestWasExact = true;
  auto observeInterests = environment.providerFace(1).onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (!evidence->exactDataName.isPrefixOf(interest.getName())) {
          return;
        }
        exactInterests.insert(interest.getName().toUri());
        everyInterestWasExact = everyInterestWasExact &&
                                !interest.getCanBePrefix() &&
                                !interest.getName().empty() &&
                                interest.getName()[-1].isSegment();
      });

  auto fetchWith = [&] (ndn_service_framework::ServiceProvider::CollaborationContext& context) {
    return std::async(std::launch::async, [&context, &publishedName, keyScope,
                                           expectedSegments] {
      return context.fetchLarge(publishedName, keyScope, 3000,
                                expectedSegments);
    });
  };
  auto firstFetch = fetchWith(consumerOneContext);
  environment.pumpUntil([&] {
    return firstFetch.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
  });
  const auto firstContent = firstFetch.get();
  BOOST_REQUIRE(firstContent);
  BOOST_CHECK_EQUAL_COLLECTIONS(firstContent->begin(), firstContent->end(),
                                frame.begin(), frame.end());
  BOOST_CHECK_EQUAL(exactInterests.size(), expectedSegments);
  BOOST_CHECK(everyInterestWasExact);
  BOOST_TEST_MESSAGE("multi-segment evidence segments=" << expectedSegments
                     << " exact_interests=" << exactInterests.size()
                     << " payload_bytes=" << frame.size());

  UavDetectorProvider detector({consumerOne.getName(), "detector-cpu",
                                "sha256:model-large", "standard", "cpu", true});
  const auto verified = detector.acceptValidatedContent(
      *evidence, *firstContent, producer.getName(), &reason);
  BOOST_REQUIRE_MESSAGE(verified.has_value(), reason);
  BOOST_CHECK(detector.execute(*verified).success);

  // A second consumer retrieves the same immutable versioned object by exact
  // segment names; correctness does not depend on a producer-side cache hit.
  auto secondFetch = fetchWith(consumerTwoContext);
  environment.pumpUntil([&] {
    return secondFetch.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
  });
  const auto secondContent = secondFetch.get();
  BOOST_REQUIRE(secondContent);
  BOOST_CHECK_EQUAL_COLLECTIONS(secondContent->begin(), secondContent->end(),
                                frame.begin(), frame.end());
  BOOST_CHECK_EQUAL(publishedSegments.size(), expectedSegments);

  // The descriptor carries references only.  Neither frame bytes nor a
  // transport endpoint is allowed to leak into an invocation payload.
  const auto manifest = makeUavIncidentEvidenceContent(
      "mission-large", "incident-large", *evidence);
  BOOST_CHECK(std::search(manifest.begin(), manifest.end(), frame.begin(),
                          frame.end()) == manifest.end());
  const auto producerNameText = producer.getName().toUri();
  const ndn::Buffer producerNameWire(
      reinterpret_cast<const uint8_t*>(producerNameText.data()),
      producerNameText.size());
  BOOST_CHECK(std::search(manifest.begin(), manifest.end(),
                          producerNameWire.begin(), producerNameWire.end()) !=
              manifest.end());
  BOOST_CHECK_EQUAL(expectedSegments, publishedSegments.size());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndnsf::examples::uav::tests
