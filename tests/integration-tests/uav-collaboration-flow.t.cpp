#include "tests/boost-test.hpp"

#include "NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp"
#include "NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp"
#include "NDNSF-UAV-APP/shared/UavDigest.hpp"
#include "NDNSF-UAV-APP/shared/UavObservationRefVerifier.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/utils.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <map>
#include <set>
#include <thread>
#include <vector>

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
  std::mutex publishedSegmentsMutex;
  auto observePublished = environment.providerFace(0).onSendData.connect(
      [&] (const ndn::Data& data) {
        if (evidence->exactDataName.isPrefixOf(data.getName()) &&
            !data.getName().empty() && data.getName()[-1].isSegment()) {
          std::lock_guard<std::mutex> lock(publishedSegmentsMutex);
          publishedSegments.insert(data.getName().toUri());
        }
      });
  const auto publishedName = producerContext.publishLargeNamed(
      keyScope, evidence->exactDataName, frame, 256, 60000);
  BOOST_REQUIRE_EQUAL(publishedName, evidence->exactDataName);
  auto publishedSegmentCount = [&] {
    std::lock_guard<std::mutex> lock(publishedSegmentsMutex);
    return publishedSegments.size();
  };
  environment.pumpUntil([&] { return publishedSegmentCount() >= 2U; });
  const auto expectedSegments = publishedSegmentCount();
  BOOST_REQUIRE_GE(expectedSegments, 2U);

  std::set<std::string> exactInterests;
  bool everyInterestWasExact = true;
  std::mutex exactInterestsMutex;
  auto observeInterests = environment.providerFace(1).onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (!evidence->exactDataName.isPrefixOf(interest.getName())) {
          return;
        }
        std::lock_guard<std::mutex> lock(exactInterestsMutex);
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
  size_t exactInterestCount = 0;
  bool allInterestsWereExact = false;
  {
    std::lock_guard<std::mutex> lock(exactInterestsMutex);
    exactInterestCount = exactInterests.size();
    allInterestsWereExact = everyInterestWasExact;
  }
  BOOST_CHECK_EQUAL(exactInterestCount, expectedSegments);
  BOOST_CHECK(allInterestsWereExact);
  BOOST_TEST_MESSAGE("multi-segment evidence segments=" << expectedSegments
                     << " exact_interests=" << exactInterestCount
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
  BOOST_CHECK_EQUAL(publishedSegmentCount(), expectedSegments);

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
  BOOST_CHECK_EQUAL(expectedSegments, publishedSegmentCount());
}

BOOST_AUTO_TEST_CASE(ImmutableStaticPublicationReusesOriginalPacketsAcrossRequests)
{
  using Context = ndn_service_framework::ServiceProvider::CollaborationContext;
  using Assignment = ndn_service_framework::ServiceProvider::CollaborationAssignment;
  ndn_service_framework::test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/UAV/Observation");
  profile.providerIdentity = ndn::Name("/example/uav/static-provider");
  profile.syncPrefix = ndn::Name("/example/uav/static-group");
  profile.providerCount = 3;
  // Actual AA parameters/DKEY and encryption, with fixture trust-any. This
  // verifies producer authorization, not deployment certificate qualification.
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  BOOST_REQUIRE_MESSAGE(environment.status() ==
    ndn_service_framework::test::EnvironmentStatus::Ready, environment.failureReason());
  for (size_t i = 0; i < environment.providerCount(); ++i)
    environment.disconnectProviderTransportForTest(i);
  environment.disconnectProviderPeerTransportForTest();
  environment.enableProductionIngressForTest();
  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& freshConsumer = environment.provider(2);
  const auto producerName = producer.getName();
  const auto otherCertificate = environment.keyChain().createIdentity(
    ndn::Name("/example/uav"), ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  const auto controllerCertificate = environment.keyChain().createIdentity(
    ndn::Name("/example/uav/static-controller"), ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  auto installStatus = [&](uint64_t policyGeneration) {
    ndn_service_framework::PolicyStatusData status;
    const ndn_service_framework::ControllerVersion version{1000, policyGeneration};
    const auto now = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::milliseconds>(
      ndn::time::system_clock::now().time_since_epoch()).count());
    status.setServiceName(profile.serviceName);
    status.setControllerVersion(version);
    status.setValidity(now - 1000, now + 600000);
    status.setPolicyDigest("sha256:" + std::string(64, policyGeneration == 1 ? 'a' : 'b'));
    status.setAbePublicParametersName(environment.attributeAuthorityPublicParametersName());
    status.setAbePublicParametersDigest(environment.attributeAuthorityPublicParametersDigest());
    status.setControllerCertificate(controllerCertificate.getName());
    ndn::Data data(ndn_service_framework::makePolicyStatusName(
      controllerCertificate.getIdentity(), profile.serviceName, version));
    data.setContent(status.wireEncode());
    environment.keyChain().sign(data, ndn::security::signingByCertificate(controllerCertificate));
    const bool signatureValid = ndn::security::verifySignature(data, controllerCertificate);
    BOOST_REQUIRE(signatureValid);
    for (size_t i = 0; i < environment.providerCount(); ++i) {
      BOOST_REQUIRE(environment.provider(i).installControllerStatus(status, signatureValid));
      BOOST_REQUIRE(environment.provider(i).getControllerVersion(profile.serviceName));
      BOOST_CHECK(*environment.provider(i).getControllerVersion(profile.serviceName) == version);
    }
    environment.pumpUntilWithAttributeAuthority([&] {
      for (size_t i = 0; i < environment.providerCount(); ++i)
        if (!environment.provider(i).isRequestDecryptionReady() ||
            !environment.provider(i).isNacProducerReadyForTest()) return false;
      return true;
    });
  };
  installStatus(1);
  Assignment sourceAssignment;
  sourceAssignment.role = "EvidenceSource";
  sourceAssignment.service = profile.serviceName;
  sourceAssignment.workCurrent = [] { return true; };
  sourceAssignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  Assignment computeAssignment = sourceAssignment;
  computeAssignment.role = "DetectorReporter";
  const auto computeCurrent = std::make_shared<std::atomic<bool>>(true);
  computeAssignment.workCurrent = [computeCurrent] { return computeCurrent->load(); };
  Context firstSource(producer, profile.userIdentity, ndn::Name("/request/static-a"),
                      ndn_service_framework::RequestMessage(), sourceAssignment);
  Context nextSource(producer, profile.userIdentity, ndn::Name("/request/static-b"),
                     ndn_service_framework::RequestMessage(), sourceAssignment);
  Context firstCompute(consumer, profile.userIdentity, ndn::Name("/request/compute-a"),
                       ndn_service_framework::RequestMessage(), computeAssignment);
  Context nextCompute(freshConsumer, profile.userIdentity, ndn::Name("/request/compute-b"),
                      ndn_service_framework::RequestMessage(), computeAssignment);
  auto execute = [&](auto operation) {
    auto result = std::async(std::launch::async, std::move(operation));
    environment.pumpUntilWithAttributeAuthority([&] {
      return result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    });
    BOOST_REQUIRE(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
    return result.get();
  };
  const ndn::Name semanticPrefix = ndn::Name(producerName)
    .append("UAV").append("CALIBRATION").append("UAV1").append("11");
  const std::string text = "{\"fixture\":\"" + std::string(16000, 'x') + "\"}";
  const ndn::Buffer plaintext(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  const auto first = execute([&] {
    return firstSource.publishImmutableProtectedData(semanticPrefix, plaintext);
  });
  BOOST_REQUIRE_MESSAGE(first.success, first.errorMessage);
  BOOST_CHECK_EQUAL(first.dataName.size(), semanticPrefix.size() + 1);
  BOOST_CHECK(first.dataName.getPrefix(-1) == semanticPrefix);
  BOOST_CHECK(first.dataName[-1].isVersion());
  BOOST_CHECK_GT(first.dataName[-1].toVersion(), 0U);
  const auto same = execute([&] {
    return nextSource.publishImmutableProtectedData(semanticPrefix, plaintext);
  });
  BOOST_REQUIRE_MESSAGE(same.success, same.errorMessage);
  BOOST_CHECK(same.dataName == first.dataName);
  BOOST_CHECK_EQUAL(same.contentSha256, first.contentSha256);
  ndn::util::Sha256 expectedDigest;
  expectedDigest << text;
  BOOST_CHECK_EQUAL(first.contentSha256, canonicalSha256Digest(expectedDigest.toString()));
  auto changed = plaintext;
  changed.back() ^= 1;
  const auto conflict = execute([&] {
    return nextSource.publishImmutableProtectedData(semanticPrefix, changed);
  });
  BOOST_CHECK(!conflict.success);
  BOOST_CHECK_EQUAL(conflict.errorMessage, "IDENTITY_CONFLICT");
  const auto revisedPrefix = ndn::Name(semanticPrefix.getPrefix(-1)).append("12");
  const auto revised = execute([&] {
    return nextSource.publishImmutableProtectedData(revisedPrefix, changed);
  });
  BOOST_REQUIRE_MESSAGE(revised.success, revised.errorMessage);
  BOOST_CHECK(revised.dataName != first.dataName);
  BOOST_CHECK(revised.dataName.getPrefix(-1) == revisedPrefix);

  std::atomic<bool> forgeCk{true};
  std::atomic<unsigned> forgedCkPackets{0};
  std::atomic<bool> holdContent{false}, contentInterestObserved{false};
  const auto staticPrefix = ndn::Name(producerName).append("UAV");
  const auto ckPrefix = ndn::Name(producerName).append("CK");
  const auto wrapperPrefix = ndn::Name(producerName).append("NDNSF").append("KEY");
  std::map<ndn::Name, ndn::Block> originalWires;
  unsigned replayedContentSegments = 0, replayedCkSegments = 0;
  unsigned replayedWrapperSegments = 0, originalWireConflicts = 0;
  ndn::signal::ScopedConnection observe = environment.providerFace(0).onSendData.connect(
    [&](const ndn::Data& packet) {
      if (!staticPrefix.isPrefixOf(packet.getName()) &&
          !ckPrefix.isPrefixOf(packet.getName()) && !wrapperPrefix.isPrefixOf(packet.getName()))
        return;
      const auto found = originalWires.find(packet.getName());
      if (found != originalWires.end()) {
        if (found->second != packet.wireEncode()) ++originalWireConflicts;
        if (first.dataName.isPrefixOf(packet.getName())) ++replayedContentSegments;
        if (ckPrefix.isPrefixOf(packet.getName())) ++replayedCkSegments;
        if (wrapperPrefix.isPrefixOf(packet.getName())) ++replayedWrapperSegments;
      }
      else originalWires.emplace(packet.getName(), packet.wireEncode());
    });
  std::vector<ndn::signal::ScopedConnection> bridges;
  for (size_t source = 0; source < environment.providerCount(); ++source) {
    for (size_t destination = 0; destination < environment.providerCount(); ++destination) {
      if (source == destination) continue;
      bridges.emplace_back(environment.providerFace(source).onSendInterest.connect(
        [&, destination](const ndn::Interest& interest) {
          if (destination == 0 && first.dataName.isPrefixOf(interest.getName())) {
            contentInterestObserved.store(true);
            if (holdContent.load()) return;
          }
          boost::asio::post(environment.providerFace(destination).getIoContext(),
            [&, destination, interest] { environment.providerFace(destination).receive(interest); });
        }));
      bridges.emplace_back(environment.providerFace(source).onSendData.connect(
        [&, destination](const ndn::Data& data) {
          auto packet = data;
          if (destination == 1 && forgeCk.load() && ckPrefix.isPrefixOf(packet.getName())) {
            environment.keyChain().sign(packet,
              ndn::security::signingByCertificate(otherCertificate));
            ++forgedCkPackets;
          }
          boost::asio::post(environment.providerFace(destination).getIoContext(),
            [&, destination, packet = std::move(packet)] {
              environment.providerFace(destination).receive(packet);
            });
        }));
    }
  }
  const auto fetch = [&](Context& context, const ndn::Name& name, const ndn::Name& owner) {
    return execute([&] {
      return context.fetchImmutableProtectedDataCurrent(name, owner,
        std::chrono::steady_clock::now() + std::chrono::seconds(3));
    });
  };
  const auto unauthorizedCk = fetch(firstCompute, first.dataName, producerName);
  BOOST_CHECK(!unauthorizedCk);
  BOOST_CHECK_GT(forgedCkPackets.load(), 0U);
  forgeCk.store(false);
  const auto firstPayload = fetch(firstCompute, first.dataName, producerName);
  BOOST_REQUIRE(firstPayload);
  BOOST_CHECK(*firstPayload == plaintext);
  // A successful read warms the shared receive-key cache. A later strict read
  // must still fetch/authorize CK packets rather than accepting that shortcut.
  const auto forgedBeforeWarmRead = forgedCkPackets.load();
  forgeCk.store(true);
  const auto rejectedWarmRead = fetch(firstCompute, first.dataName, producerName);
  BOOST_CHECK(!rejectedWarmRead);
  BOOST_CHECK_GT(forgedCkPackets.load(), forgedBeforeWarmRead);
  forgeCk.store(false);
  const auto oldCkReplays = replayedCkSegments;
  const auto oldWrapperReplays = replayedWrapperSegments;
  const auto secondPayload = fetch(nextCompute, same.dataName, producerName);
  BOOST_REQUIRE(secondPayload);
  BOOST_CHECK(*secondPayload == plaintext);
  BOOST_CHECK_GT(replayedCkSegments, oldCkReplays);
  BOOST_CHECK_GT(replayedWrapperSegments, oldWrapperReplays);
  const auto revisedPayload = fetch(nextCompute, revised.dataName, producerName);
  BOOST_REQUIRE(revisedPayload);
  BOOST_CHECK(*revisedPayload == changed);
  BOOST_CHECK_GT(replayedContentSegments, 0U);
  BOOST_CHECK_EQUAL(originalWireConflicts, 0U);
  BOOST_CHECK(std::any_of(originalWires.begin(), originalWires.end(),
    [&](const auto& wire) { return ckPrefix.isPrefixOf(wire.first); }));
  BOOST_CHECK(std::any_of(originalWires.begin(), originalWires.end(),
    [&](const auto& wire) { return wrapperPrefix.isPrefixOf(wire.first); }));
  for (const auto& keyPrefix : {ckPrefix, wrapperPrefix}) {
    const auto packet = std::find_if(originalWires.begin(), originalWires.end(),
      [&](const auto& wire) { return keyPrefix.isPrefixOf(wire.first); });
    BOOST_REQUIRE(packet != originalWires.end());
    unsigned replies = 0;
    ndn::signal::ScopedConnection observeReply = environment.providerFace(0).onSendData.connect(
      [&](const ndn::Data& data) {
        if (data.getName() == packet->first) ++replies;
      });
    ndn::Interest keyInterest(packet->first);
    keyInterest.setMustBeFresh(true);
    environment.providerFace(0).receive(keyInterest);
    const auto replyDeadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(50);
    environment.pumpUntilWithAttributeAuthority([&] {
      return std::chrono::steady_clock::now() >= replyDeadline;
    });
    BOOST_CHECK_EQUAL(replies, 1U);
  }
  BOOST_CHECK(!fetch(nextCompute, first.dataName, consumer.getName()));
  auto envelopeIdentity = [&](const ndn::Name& name) {
    auto bytes = std::make_shared<ndn::Buffer>();
    for (const auto& entry : originalWires) {
      if (!name.isPrefixOf(entry.first) || entry.first.size() != name.size() + 1) continue;
      const ndn::Data packet(entry.second);
      const auto& content = packet.getContent();
      bytes->insert(bytes->end(), content.value_begin(), content.value_end());
    }
    ndn_service_framework::HybridMessageEnvelope envelope;
    BOOST_REQUIRE(envelope.WireDecode(ndn::Block(bytes)));
    BOOST_CHECK(!envelope.hasWrappedMessageKey());
    return std::make_pair(envelope.getKeyId(), envelope.getEpochId());
  };
  const auto initialProtection = envelopeIdentity(first.dataName);
  const auto wrappersBeforeTransition = replayedWrapperSegments;
  const auto ckBeforeTransition = replayedCkSegments;
  installStatus(2);
  // The cache invalidation must not remove the original static publication or
  // its signed key packets. A new request can still resolve the old reference.
  const auto retainedAfterTransition = fetch(nextCompute, first.dataName, producerName);
  BOOST_REQUIRE(retainedAfterTransition);
  BOOST_CHECK(*retainedAfterTransition == plaintext);
  BOOST_CHECK_GT(replayedWrapperSegments, wrappersBeforeTransition);
  BOOST_CHECK_GT(replayedCkSegments, ckBeforeTransition);
  Context advancedSource(producer, profile.userIdentity, ndn::Name("/request/static-status-2"),
                         ndn_service_framework::RequestMessage(), sourceAssignment);
  const auto advanced = execute([&] {
    return advancedSource.publishImmutableProtectedData(semanticPrefix, plaintext);
  });
  BOOST_REQUIRE_MESSAGE(advanced.success, advanced.errorMessage);
  BOOST_CHECK(advanced.dataName != first.dataName);
  BOOST_CHECK(advanced.dataName.getPrefix(-1) == semanticPrefix);
  BOOST_CHECK_EQUAL(advanced.contentSha256, first.contentSha256);
  const auto wrapperPacketCount = [&] {
    return std::count_if(originalWires.begin(), originalWires.end(),
      [&](const auto& entry) { return wrapperPrefix.isPrefixOf(entry.first); });
  };
  const auto oldWrapperPacketCount = wrapperPacketCount();
  const auto advancedPayload = fetch(nextCompute, advanced.dataName, producerName);
  BOOST_REQUIRE(advancedPayload);
  BOOST_CHECK(*advancedPayload == plaintext);
  BOOST_CHECK_GT(wrapperPacketCount(), oldWrapperPacketCount);
  const auto advancedProtection = envelopeIdentity(advanced.dataName);
  BOOST_CHECK_NE(initialProtection.first, advancedProtection.first);
  BOOST_CHECK_NE(initialProtection.second, advancedProtection.second);
  const auto advancedAgain = execute([&] {
    return advancedSource.publishImmutableProtectedData(semanticPrefix, plaintext);
  });
  BOOST_REQUIRE_MESSAGE(advancedAgain.success, advancedAgain.errorMessage);
  BOOST_CHECK(advancedAgain.dataName == advanced.dataName);
  BOOST_CHECK_EQUAL(originalWireConflicts, 0U);

  // A distinct actor can prepublish calibration once and supply the same
  // producer-owned reference to different camera requests/epochs. Payload here
  // is a transport fixture; full calibration schema is checked by UAV callers.
  const auto externalProducer = consumer.getName();
  const auto externalPrefix = ndn::Name(externalProducer)
    .append("UAV").append("CALIBRATION").append("UAV1").append("4");
  const std::string externalText = R"({"fixture":"external-calibration","revision":"4"})";
  const ndn::Buffer externalBytes(externalText.begin(), externalText.end());
  Context calibrationSource(consumer, profile.userIdentity, ndn::Name("/request/calibration-a"),
                            ndn_service_framework::RequestMessage(), sourceAssignment);
  Context calibrationNext(consumer, profile.userIdentity, ndn::Name("/request/calibration-b"),
                          ndn_service_framework::RequestMessage(), sourceAssignment);
  const auto externalPublication = execute([&] {
    return calibrationSource.publishImmutableProtectedData(externalPrefix, externalBytes);
  });
  BOOST_REQUIRE_MESSAGE(externalPublication.success, externalPublication.errorMessage);
  const auto externalAgain = execute([&] {
    return calibrationNext.publishImmutableProtectedData(externalPrefix, externalBytes);
  });
  BOOST_REQUIRE_MESSAGE(externalAgain.success, externalAgain.errorMessage);
  BOOST_CHECK(externalAgain.dataName == externalPublication.dataName);
  BOOST_CHECK(externalPublication.dataName.getPrefix(-1) == externalPrefix);
  const auto externalReference = ndnsf::examples::uav::parseObservationReference(
    externalPublication.dataName.toUri(), externalPublication.contentSha256,
    externalProducer.toUri(), "CALIBRATION");
  BOOST_CHECK_NO_THROW(ndnsf::examples::uav::requireObservationReferenceNameShape(
    externalReference, "CALIBRATION", "UAV1", 7, 1));
  BOOST_CHECK_NO_THROW(ndnsf::examples::uav::requireObservationReferenceNameShape(
    externalReference, "CALIBRATION", "UAV1", 8, 99));
  const auto externalForCamera = fetch(firstSource, externalPublication.dataName, externalProducer);
  BOOST_REQUIRE(externalForCamera);
  BOOST_CHECK(*externalForCamera == externalBytes);
  const auto externalForNextRequest = fetch(nextCompute, externalPublication.dataName, externalProducer);
  BOOST_REQUIRE(externalForNextRequest);
  BOOST_CHECK(*externalForNextRequest == externalBytes);
  BOOST_CHECK(!fetch(nextCompute, externalPublication.dataName, producerName));

  Assignment raceAssignment = sourceAssignment;
  raceAssignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  Context raceRecovery(producer, profile.userIdentity, ndn::Name("/request/race-recovery"),
                       ndn_service_framework::RequestMessage(), raceAssignment);
  Context raceReader(freshConsumer, profile.userIdentity, ndn::Name("/request/race-reader"),
                     ndn_service_framework::RequestMessage(), raceAssignment);
  const auto racePrefix = [&](const char* revision) {
    return ndn::Name(producerName).append("UAV").append("CLOCK").append(revision);
  };
  const ndn::Buffer raceBytes{1, 2, 3}, recoveredBytes{4, 5, 6};
  const auto cancelAtCommit = std::make_shared<Context>(
    producer, profile.userIdentity, ndn::Name("/request/cancel-at-commit"),
    ndn_service_framework::RequestMessage(), raceAssignment);
  const auto beforeRevoked = std::make_shared<std::atomic<bool>>(false);
  producer.setImmutablePublicationHooksForTest({},
    [cancelAtCommit, beforeRevoked](const ndn::Name&) {
      std::thread revoker([cancelAtCommit, beforeRevoked] {
        beforeRevoked->store(cancelAtCommit->revokeCurrentWorkAuthority());
      });
      revoker.join();
    }, {});
  const auto canceledPublication = execute([&] {
    return cancelAtCommit->publishImmutableProtectedData(racePrefix("9001"), raceBytes);
  });
  producer.setImmutablePublicationHooksForTest({}, {}, {});
  BOOST_CHECK(beforeRevoked->load());
  BOOST_CHECK(!canceledPublication.success);
  // Changed bytes at the same semantic revision would conflict if cancellation
  // had left an original committed, even if the API falsely reported failure.
  const auto afterCancellation = execute([&] {
    return raceRecovery.publishImmutableProtectedData(racePrefix("9001"), recoveredBytes);
  });
  BOOST_REQUIRE_MESSAGE(afterCancellation.success, afterCancellation.errorMessage);
  const auto recovered = fetch(raceReader, afterCancellation.dataName, producerName);
  BOOST_REQUIRE(recovered);
  BOOST_CHECK(*recovered == recoveredBytes);

  const auto commitBeforeCancel = std::make_shared<Context>(
    producer, profile.userIdentity, ndn::Name("/request/commit-before-cancel"),
    ndn_service_framework::RequestMessage(), raceAssignment);
  const auto afterRevoked = std::make_shared<std::atomic<bool>>(false);
  producer.setImmutablePublicationHooksForTest({}, {},
    [commitBeforeCancel, afterRevoked](const ndn::Name&) {
      std::thread revoker([commitBeforeCancel, afterRevoked] {
        afterRevoked->store(commitBeforeCancel->revokeCurrentWorkAuthority());
      });
      revoker.join();
    });
  const auto committedBeforeCancellation = execute([&] {
    return commitBeforeCancel->publishImmutableProtectedData(racePrefix("9002"), raceBytes);
  });
  producer.setImmutablePublicationHooksForTest({}, {}, {});
  BOOST_CHECK(afterRevoked->load());
  BOOST_REQUIRE_MESSAGE(committedBeforeCancellation.success, committedBeforeCancellation.errorMessage);
  const auto committedBytes = fetch(raceReader, committedBeforeCancellation.dataName, producerName);
  BOOST_REQUIRE(committedBytes);
  BOOST_CHECK(*committedBytes == raceBytes);

  const auto queuedContext = std::make_shared<Context>(
    producer, profile.userIdentity, ndn::Name("/request/cancel-queued"),
    ndn_service_framework::RequestMessage(), raceAssignment);
  const auto queuedNotice = std::make_shared<std::promise<void>>();
  auto queuedReady = queuedNotice->get_future();
  producer.setImmutablePublicationHooksForTest([queuedNotice] { queuedNotice->set_value(); }, {}, {});
  auto queuedPublication = std::async(std::launch::async, [&] {
    return queuedContext->publishImmutableProtectedData(racePrefix("9003"), raceBytes);
  });
  // No Face pumping between notification, revoke and worker completion.
  const bool wasQueued = queuedReady.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  const bool queuedRevoked = queuedContext->revokeCurrentWorkAuthority();
  const bool finishedUnpumped = queuedPublication.wait_for(std::chrono::seconds(1)) ==
                               std::future_status::ready;
  const auto queueDrained = std::make_shared<std::atomic<bool>>(false);
  boost::asio::post(environment.providerFace(0).getIoContext(),
                    [queueDrained] { queueDrained->store(true); });
  environment.pumpUntilWithAttributeAuthority([&] {
    return queueDrained->load() &&
           queuedPublication.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  producer.setImmutablePublicationHooksForTest({}, {}, {});
  BOOST_CHECK(wasQueued);
  BOOST_CHECK(queuedRevoked);
  BOOST_CHECK(finishedUnpumped);
  BOOST_REQUIRE(queuedPublication.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
  BOOST_CHECK(!queuedPublication.get().success);
  const auto afterQueuedCancellation = execute([&] {
    return raceRecovery.publishImmutableProtectedData(racePrefix("9003"), recoveredBytes);
  });
  BOOST_REQUIRE_MESSAGE(afterQueuedCancellation.success, afterQueuedCancellation.errorMessage);
  const auto queuedRecoveryBytes = fetch(raceReader, afterQueuedCancellation.dataName, producerName);
  BOOST_REQUIRE(queuedRecoveryBytes);
  BOOST_CHECK(*queuedRecoveryBytes == recoveredBytes);

  holdContent.store(true);
  contentInterestObserved.store(false);
  auto canceled = std::async(std::launch::async, [&] {
    return nextCompute.fetchImmutableProtectedDataCurrent(first.dataName, producerName,
      std::chrono::steady_clock::now() + std::chrono::seconds(3));
  });
  environment.pumpUntilWithAttributeAuthority([&] { return contentInterestObserved.load(); });
  BOOST_REQUIRE(contentInterestObserved.load());
  computeCurrent->store(false);
  environment.pumpUntilWithAttributeAuthority([&] {
    return canceled.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  BOOST_REQUIRE(canceled.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
  BOOST_CHECK(!canceled.get());
  const auto rejectedPublication = execute([&] {
    Assignment revoked = sourceAssignment;
    revoked.workCurrent = [] { return false; };
    Context closed(producer, profile.userIdentity, ndn::Name("/request/closed"),
                   ndn_service_framework::RequestMessage(), revoked);
    return closed.publishImmutableProtectedData(semanticPrefix, plaintext);
  });
  BOOST_CHECK(!rejectedPublication.success);
}

BOOST_AUTO_TEST_CASE(ObservationDependencyFetchRejectsTamperAndValidatesReferenceDigest)
{
  ndn_service_framework::test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/UAV/Observation");
  profile.providerIdentity = ndn::Name("/example/uav/drone");
  profile.syncPrefix = ndn::Name("/example/uav/group");
  profile.providerCount = 2;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  const ndn::Name ancestorSignerName("/example/uav");
  const auto ancestorSignerCertificate =
    environment.createTestIdentityCertificate(ancestorSignerName);
  environment.bootstrap();
  BOOST_REQUIRE(environment.status() ==
                ndn_service_framework::test::EnvironmentStatus::Ready);
  for (size_t index = 0; index < environment.providerCount(); ++index) {
    environment.disconnectProviderTransportForTest(index);
  }
  environment.disconnectProviderPeerTransportForTest();
  environment.enableProductionIngressForTest();

  auto& producer = environment.provider(0);
  auto& consumer = environment.provider(1);
  auto& consumerFace = environment.providerFace(1);
  const auto producerName = producer.getName();
  BOOST_REQUIRE_NE(ancestorSignerName, producerName);
  const ndn::Name requestId("/request/uav-observation-clock");
  const std::string keyScope = "uav-observation";
  const ndn::Buffer scopeKey(32, 0x53);
  ndn_service_framework::ServiceProvider::CollaborationAssignment producerAssignment;
  producerAssignment.role = "EvidenceSource";
  producerAssignment.service = profile.serviceName;
  producerAssignment.scopeKeys.emplace(keyScope, scopeKey);
  auto consumerAssignment = producerAssignment;
  consumerAssignment.role = "DetectorReporter";
  ndn_service_framework::ServiceProvider::CollaborationContext producerContext(
      producer, profile.userIdentity, requestId,
      ndn_service_framework::RequestMessage(), producerAssignment);
  ndn_service_framework::ServiceProvider::CollaborationContext consumerContext(
      consumer, profile.userIdentity, requestId,
      ndn_service_framework::RequestMessage(), consumerAssignment);

  std::atomic<bool> corruptNextData{false};
  std::atomic<bool> holdTargetData{false};
  std::atomic<bool> cancelFetchAuthorityRevoked{false};
  std::atomic<bool> cancelFetchInterestObserved{false};
  std::atomic<unsigned> corruptedPackets{0};
  std::atomic<bool> originalPacketSignatureValid{false};
  std::atomic<bool> corruptedPacketSignatureValid{true};
  std::atomic<bool> corruptedWireSignatureValid{true};
  std::atomic<bool> signNextDataAsAncestor{false};
  std::atomic<unsigned> ancestorSignedPackets{0};
  std::optional<ndn::Data> corruptedPacketForValidation;
  std::optional<ndn::Data> ancestorPacketForValidation;
  std::optional<ndn::Buffer> corruptedPayload;
  std::string targetDataName;
  std::vector<ndn::signal::ScopedConnection> bridges;

  const ndn::Name dependencyName(producerName.toUri() +
    "/UAV/OBS/UAV1/7/2/POSE/v=1");
  targetDataName = dependencyName.toUri();
  const std::string clockText =
    R"({"revision":"4","clockId":"clock-uav1","clockEpoch":"1","a":"1","bUs":"0",)"
    R"("validFromUs":"0","validUntilUs":"9223372036854775807","uncertaintyUs":"0"})";
  const ndn::Buffer clockPayload(reinterpret_cast<const uint8_t*>(clockText.data()),
                                 clockText.size());
  std::set<std::string> publishedSegments;
  std::mutex publishedSegmentsMutex;
  auto observePublished = environment.providerFace(0).onSendData.connect(
    [&] (const ndn::Data& data) {
      if (dependencyName.isPrefixOf(data.getName()) && !data.getName().empty() &&
          data.getName()[-1].isSegment()) {
        std::lock_guard<std::mutex> lock(publishedSegmentsMutex);
        publishedSegments.insert(data.getName().toUri());
      }
    });
  const auto publishedName = producerContext.publishLargeNamed(
    keyScope, dependencyName, clockPayload, 4096, 60000);
  BOOST_REQUIRE_EQUAL(publishedName, dependencyName);
  auto publishedSegmentCount = [&] {
    std::lock_guard<std::mutex> lock(publishedSegmentsMutex);
    return publishedSegments.size();
  };
  environment.pumpUntil([&] { return publishedSegmentCount() != 0; });
  BOOST_REQUIRE_EQUAL(publishedSegmentCount(), 1U);

  for (size_t source = 0; source < environment.providerCount(); ++source) {
    for (size_t destination = 0; destination < environment.providerCount(); ++destination) {
      if (source == destination) continue;
      bridges.emplace_back(environment.providerFace(source).onSendInterest.connect(
        [&environment, destination] (const ndn::Interest& interest) {
          auto packet = interest;
          boost::asio::post(environment.providerFace(destination).getIoContext(),
            [&environment, destination, packet = std::move(packet)] {
              environment.providerFace(destination).receive(packet);
            });
        }));
      bridges.emplace_back(environment.providerFace(source).onSendData.connect(
        [&environment, destination, &corruptNextData, &corruptedPackets,
         &originalPacketSignatureValid, &corruptedPacketSignatureValid,
         &corruptedWireSignatureValid,
         &signNextDataAsAncestor, &ancestorSignedPackets,
         &corruptedPacketForValidation, &ancestorPacketForValidation,
         &corruptedPayload, &targetDataName, &holdTargetData, ancestorSignerName,
         producerName] (const ndn::Data& data) {
          auto packet = data;
          const bool isTargetData = destination == 1 &&
            !targetDataName.empty() && ndn::Name(targetDataName).isPrefixOf(packet.getName());
          if (isTargetData && holdTargetData.load()) return;
          if (isTargetData) {
            const bool isCorrupted = corruptNextData.load();
            if (isCorrupted) {
              auto content = ndn::Buffer(packet.getContent().value_begin(),
                                         packet.getContent().value_end());
              if (!content.empty()) {
                content[0] ^= 0x01;
                packet.setContent(content);
                corruptedPayload = content;
              }
              auto signature = ndn::Buffer(packet.getSignatureValue().value_begin(),
                                           packet.getSignatureValue().value_end());
              if (!signature.empty()) {
                signature[0] ^= 0x01;
                packet.setSignatureValue(signature);
                ++corruptedPackets;
              }
            }
            else if (signNextDataAsAncestor.exchange(false)) {
              environment.keyChain().sign(
                packet, ndn::security::signingByIdentity(ancestorSignerName));
              ancestorPacketForValidation = packet;
              ++ancestorSignedPackets;
            }
            try {
              const auto& keyLocator = packet.getSignatureInfo().getKeyLocator();
              const auto& certificateName = keyLocator.getName();
              const auto keyName = ndn::security::extractKeyNameFromCertName(certificateName);
              const auto signerIdentity =
                ndn::security::extractIdentityFromCertName(certificateName);
              const auto certificate = environment.keyChain().getPib().getIdentity(signerIdentity)
                                         .getKey(keyName).getCertificate(certificateName);
              const bool signatureValid = ndn::security::verifySignature(packet, certificate);
              if (isCorrupted) {
                corruptedPacketForValidation = packet;
                corruptedPacketSignatureValid.store(signatureValid);
                ndn::Data wireDecoded;
                wireDecoded.wireDecode(packet.wireEncode());
                corruptedWireSignatureValid.store(
                  ndn::security::verifySignature(wireDecoded, certificate));
              }
              else {
                originalPacketSignatureValid.store(signatureValid);
              }
            }
            catch (const std::exception&) {
            }
          }
          boost::asio::post(environment.providerFace(destination).getIoContext(),
            [&environment, destination, packet = std::move(packet)] {
              environment.providerFace(destination).receive(packet);
            });
        }));
    }
  }

  auto directValidator = std::make_shared<ndn_service_framework::MessageValidator>(
    environment.profile().trustSchemaPath, std::nullopt, &environment.providerFace(1));

  auto fetchDependency = [&] (std::size_t expectedSegments = 1) {
    return std::async(std::launch::async, [&, expectedSegments] {
      return consumerContext.fetchLargeFromProducer(
        publishedName, keyScope, producerName, 3000, expectedSegments);
    });
  };
  auto validateAncestorPacket = [&] {
    BOOST_REQUIRE(ancestorPacketForValidation.has_value());
    BOOST_CHECK(ndn::security::verifySignature(
      *ancestorPacketForValidation, ancestorSignerCertificate));
    bool validationDone = false;
    bool validationSucceeded = false;
    directValidator->validate(
      *ancestorPacketForValidation,
      [&] (const ndn::Data&) {
        validationSucceeded = true;
        validationDone = true;
      },
      [&] (const ndn::Data&, const ndn::security::ValidationError&) {
        validationDone = true;
      });
    environment.pumpUntil([&] { return validationDone; });
    BOOST_CHECK(validationSucceeded);
  };
  corruptNextData.store(true);
  auto invalidSignatureFetch = fetchDependency();
  environment.pumpUntil([&] {
    return invalidSignatureFetch.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
  });
  const auto invalidSignatureContent = invalidSignatureFetch.get();
  BOOST_CHECK(!invalidSignatureContent);
  if (invalidSignatureContent && corruptedPayload) {
    BOOST_CHECK_EQUAL_COLLECTIONS(invalidSignatureContent->begin(), invalidSignatureContent->end(),
                                  corruptedPayload->begin(), corruptedPayload->end());
  }
  BOOST_CHECK_GE(corruptedPackets.load(), 1U);
  BOOST_CHECK(!corruptedPacketSignatureValid.load());
  BOOST_CHECK(!corruptedWireSignatureValid.load());
  BOOST_REQUIRE(corruptedPacketForValidation.has_value());
  bool directValidationDone = false;
  bool directValidationSucceeded = false;
  directValidator->validate(
    *corruptedPacketForValidation,
    [&directValidationDone, &directValidationSucceeded] (const ndn::Data&) {
      directValidationSucceeded = true;
      directValidationDone = true;
    },
    [&directValidationDone] (const ndn::Data&, const ndn::security::ValidationError&) {
      directValidationDone = true;
    });
  environment.pumpUntil([&] { return directValidationDone; });
  BOOST_CHECK(!directValidationSucceeded);
  corruptNextData.store(false);

  signNextDataAsAncestor.store(true);
  // Exercise the streaming SegmentFetcher path as well as the exact-segment
  // path used by the tamper and valid-fetch cases above and below.
  auto wrongProducerSignerFetch = fetchDependency(0);
  environment.pumpUntil([&] {
    return wrongProducerSignerFetch.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
  });
  const auto wrongProducerSignerContent = wrongProducerSignerFetch.get();
  BOOST_CHECK(!wrongProducerSignerContent);
  BOOST_CHECK_EQUAL(ancestorSignedPackets.load(), 1U);
  validateAncestorPacket();

  // The exact-segment fetch path must enforce the same expected signer rule.
  signNextDataAsAncestor.store(true);
  auto exactWrongProducerSignerFetch = fetchDependency(1);
  environment.pumpUntil([&] {
    return exactWrongProducerSignerFetch.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
  });
  const auto exactWrongProducerSignerContent = exactWrongProducerSignerFetch.get();
  BOOST_CHECK(!exactWrongProducerSignerContent);
  BOOST_CHECK_EQUAL(ancestorSignedPackets.load(), 2U);
  validateAncestorPacket();

  auto validFetch = fetchDependency();
  environment.pumpUntil([&] {
    return validFetch.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
  });
  const auto fetched = validFetch.get();
  BOOST_REQUIRE(fetched);
  BOOST_CHECK_EQUAL_COLLECTIONS(fetched->begin(), fetched->end(),
                                clockPayload.begin(), clockPayload.end());
  BOOST_CHECK(originalPacketSignatureValid.load());

  const std::string fetchedText(fetched->begin(), fetched->end());
  ndn::util::Sha256 digest;
  digest << fetchedText;
  const auto contentSha256 = ndnsf::examples::uav::canonicalSha256Digest(digest.toString());
  const auto reference = ndnsf::examples::uav::parseObservationReference(
    publishedName.toUri(), contentSha256, producerName.toUri(), "POSE");
  ndnsf::examples::uav::requireObservationReferenceNameShape(
    reference, "POSE", "UAV1", 7, 2);
  BOOST_CHECK_NO_THROW(ndnsf::examples::uav::validateObservationReference(
    reference, publishedName, producerName, contentSha256, "POSE"));
  auto wrongDigest = contentSha256;
  wrongDigest.back() = wrongDigest.back() == '0' ? '1' : '0';
  const auto wrongReference = ndnsf::examples::uav::parseObservationReference(
    publishedName.toUri(), wrongDigest, producerName.toUri(), "POSE");
  BOOST_CHECK_THROW(ndnsf::examples::uav::validateObservationReference(
    wrongReference, publishedName, producerName, contentSha256, "POSE"),
    std::runtime_error);

  auto cancelAssignment = consumerAssignment;
  cancelAssignment.workDeadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
  cancelAssignment.workCurrent = [] { return true; };
  ndn_service_framework::ServiceProvider::CollaborationContext cancelContext(
      consumer, profile.userIdentity, requestId,
      ndn_service_framework::RequestMessage(), cancelAssignment);
  BOOST_CHECK(cancelContext.hasCurrentWorkAuthority());
  auto observeCancellationInterest = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
      if (!dependencyName.isPrefixOf(interest.getName())) return;
      cancelFetchInterestObserved.store(true);
      cancelFetchAuthorityRevoked.store(cancelContext.revokeCurrentWorkAuthority());
    });
  holdTargetData.store(true);
  const auto pendingBeforeCancellation = consumerFace.getNPendingInterests();
  auto cancelledFetch = std::async(std::launch::async, [&] {
    return cancelContext.fetchLargeFromProducerCurrent(
      publishedName, keyScope, producerName, 3000);
  });
  const auto cancellationStart = std::chrono::steady_clock::now();
  environment.pumpUntil([&] {
    return cancelledFetch.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
  });
  const auto cancellationElapsed = std::chrono::steady_clock::now() - cancellationStart;
  const auto cancelledPayload = cancelledFetch.get();
  holdTargetData.store(false);
  BOOST_CHECK(cancelFetchInterestObserved.load());
  BOOST_CHECK(cancelFetchAuthorityRevoked.load());
  BOOST_CHECK(!cancelContext.hasCurrentWorkAuthority());
  BOOST_CHECK(!cancelledPayload);
  BOOST_CHECK_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                   cancellationElapsed).count(), 1000);
  const auto cleanupDeadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(250);
  environment.pumpUntil([&] {
    return consumerFace.getNPendingInterests() == pendingBeforeCancellation ||
           std::chrono::steady_clock::now() >= cleanupDeadline;
  });
  BOOST_CHECK_EQUAL(consumerFace.getNPendingInterests(), pendingBeforeCancellation);

  // A timed-out caller must prevent an unpumped Face dispatch from starting
  // later, even while its request authority would otherwise remain current.
  auto timeoutAssignment = consumerAssignment;
  timeoutAssignment.workCurrent = [] { return true; };
  timeoutAssignment.workDeadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(5);
  ndn_service_framework::ServiceProvider::CollaborationContext timeoutContext(
      consumer, profile.userIdentity, requestId,
      ndn_service_framework::RequestMessage(), timeoutAssignment);
  std::atomic<unsigned> timedOutInterests{0};
  ndn::signal::ScopedConnection observeTimedOutInterest = consumerFace.onSendInterest.connect(
      [&] (const ndn::Interest& interest) {
        if (dependencyName.isPrefixOf(interest.getName())) ++timedOutInterests;
      });
  auto timedOutFetch = std::async(std::launch::async, [&] {
    return timeoutContext.fetchLargeFromProducerCurrent(
      publishedName, keyScope, producerName, 40);
  });
  BOOST_REQUIRE(timedOutFetch.wait_for(std::chrono::milliseconds(500)) ==
                std::future_status::ready);
  BOOST_CHECK(!timedOutFetch.get());
  const auto drainDeadline = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(100);
  environment.pumpUntil([&] {
    return std::chrono::steady_clock::now() >= drainDeadline;
  });
  BOOST_CHECK_EQUAL(timedOutInterests.load(), 0U);
  BOOST_CHECK_EQUAL(consumerFace.getNPendingInterests(), pendingBeforeCancellation);

}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace ndnsf::examples::uav::tests
