#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <set>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(CollaborationStatus)

namespace {

class FixedDeferredSelection final : public ParticipantSelectionPolicy
{
public:
  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    if (candidates.empty() || roles.empty() || !candidates.front().ack.getStatus()) {
      return {};
    }
    const auto& candidate = candidates.front();
    const auto& role = roles.front();
    std::string assignment = "opaque=" + role.requiredArtifact.toUri();
    ndn::Buffer payload(
      reinterpret_cast<const uint8_t*>(assignment.data()), assignment.size());
    return {{
      role.role,
      candidate.serviceName,
      candidate.providerName,
      role.requiredArtifact,
      false,
      0,
      std::move(payload),
      candidate,
    }};
  }
};

class ThreeRoleLargeDeferredSelection final : public ParticipantSelectionPolicy
{
public:
  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    if (candidates.size() != 3 || roles.size() != 3) {
      return {};
    }
    std::vector<SelectedParticipant> selected;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (!candidates[i].ack.getStatus()) {
        return {};
      }
      std::string opaque(2400, static_cast<char>('A' + i));
      opaque.replace(0, roles[i].role.size(), roles[i].role);
      ndn::Buffer payload(
        reinterpret_cast<const uint8_t*>(opaque.data()), opaque.size());
      selected.push_back({
        roles[i].role,
        candidates[i].serviceName,
        candidates[i].providerName,
        roles[i].requiredArtifact,
        false,
        0,
        std::move(payload),
        candidates[i],
      });
    }
    return selected;
  }
};

CollaborationPlan
makeDeferredPlan(const ndn::Name& artifact = ndn::Name("/artifact/a"))
{
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 100;
  plan.timeoutMs = 1000;
  CollaborationRoleSpec role;
  role.role = "worker";
  role.service = ndn::Name("/generic/work");
  role.requiredArtifact = artifact;
  plan.roles.push_back(std::move(role));
  plan.participantSelector = std::make_shared<FixedDeferredSelection>();
  return plan;
}

CollaborationPlan
makeThreeRoleLargeDeferredPlan()
{
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 100;
  plan.timeoutMs = 1000;
  for (size_t i = 0; i < 3; ++i) {
    CollaborationRoleSpec role;
    role.role = "stage-" + std::to_string(i);
    role.service = ndn::Name("/generic/work");
    role.requiredArtifact = ndn::Name("/artifact/stage").appendNumber(i);
    role.minProviders = 1;
    role.maxProviders = 1;
    plan.roles.push_back(std::move(role));
  }
  plan.keyScopes = {
    {"stage-0-to-1", {"stage-0", "stage-1"}},
    {"stage-1-to-2", {"stage-1", "stage-2"}},
  };
  plan.dependencies = {
    {{"stage-0"}, {"stage-1"}, "stage-0-to-1", ndn::Name("/activation"), true},
    {{"stage-1"}, {"stage-2"}, "stage-1-to-2", ndn::Name("/activation"), true},
  };
  const std::string scopeKeyMetadata =
    "scopeKeyData.stage-0-to-1=/key/stage-0-to-1;"
    "scopeKeyData.stage-1-to-2=/key/stage-1-to-2;";
  plan.sharedAssignmentMetadata = ndn::Buffer(
    reinterpret_cast<const uint8_t*>(scopeKeyMetadata.data()),
    scopeKeyMetadata.size());
  plan.participantSelector =
    std::make_shared<ThreeRoleLargeDeferredSelection>();
  return plan;
}

} // namespace

BOOST_AUTO_TEST_CASE(DeferredAckClosureAndPlanCommitAreOneShotAndIdempotent)
{
  ndn::security::KeyChain keyChain(
    "pib-memory:deferred-collab", "tpm-memory:deferred-collab");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/user/a"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceUser user(
    face, ndn::Name("/test/group"), userCert, aaCert,
    "examples/trust-any.conf");
  const ndn::Name requestId("/request/deferred-1");
  size_t closureCount = 0;
  CollaborationAckClosure closed;
  user.prepareDeferredCollaborationForTest(
    requestId,
    [&](const CollaborationAckClosure& value) {
      ++closureCount;
      closed = value;
    });
  user.addDeferredAckForTest(requestId, ndn::Name("/provider/a"), "worker");

  BOOST_CHECK_THROW(
    user.CommitCollaborationPlan(
      requestId, "sha256:" + std::string(64, '0'), makeDeferredPlan()),
    std::logic_error);
  BOOST_CHECK(user.closeDeferredAcksForTest(requestId));
  BOOST_CHECK(user.closeDeferredAcksForTest(requestId));
  BOOST_CHECK_EQUAL(closureCount, 1);
  BOOST_CHECK_EQUAL(closed.requestId, requestId);
  BOOST_REQUIRE_EQUAL(closed.candidates.size(), 1);
  BOOST_CHECK_EQUAL(closed.candidates.front().providerName, "/provider/a");
  BOOST_CHECK_EQUAL(closed.digest.size(), 71);

  BOOST_CHECK_THROW(
    user.CommitCollaborationPlan(
      requestId, "sha256:" + std::string(64, '1'), makeDeferredPlan()),
    std::invalid_argument);
  BOOST_CHECK(user.CommitCollaborationPlan(
    requestId, closed.digest, makeDeferredPlan()));
  BOOST_CHECK(user.CommitCollaborationPlan(
    requestId, closed.digest, makeDeferredPlan()));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), "/provider/a");
  BOOST_CHECK_THROW(
    user.CommitCollaborationPlan(
      requestId, closed.digest, makeDeferredPlan(ndn::Name("/artifact/b"))),
    std::logic_error);
}

BOOST_AUTO_TEST_CASE(LargeThreeRoleCollaborationPublishesBoundedProviderProjections)
{
  ndn::security::KeyChain keyChain(
    "pib-memory:deferred-large-collab", "tpm-memory:deferred-large-collab");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/user/large-collab"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceUser user(
    face, ndn::Name("/test/group"), userCert, aaCert,
    "examples/trust-any.conf");
  const ndn::Name requestId("/request/large-collab-1");
  size_t closureCount = 0;
  CollaborationAckClosure closed;
  user.prepareDeferredCollaborationForTest(
    requestId,
    [&](const CollaborationAckClosure& value) {
      ++closureCount;
      closed = value;
    });

  const std::vector<ndn::Name> providers = {
    ndn::Name("/provider/stage-0"),
    ndn::Name("/provider/stage-1"),
    ndn::Name("/provider/stage-2"),
  };
  for (size_t i = 0; i < providers.size(); ++i) {
    user.addDeferredAckForTest(
      requestId, providers[i], "stage-" + std::to_string(i));
  }
  BOOST_CHECK(user.closeDeferredAcksForTest(requestId));
  BOOST_REQUIRE_EQUAL(closed.candidates.size(), 3);
  BOOST_REQUIRE_EQUAL(closureCount, 1);

  const auto plan = makeThreeRoleLargeDeferredPlan();
  BOOST_CHECK(user.CommitCollaborationPlan(requestId, closed.digest, plan));

  const auto published = user.getSelectionPublishedProviders(requestId);
  BOOST_REQUIRE_EQUAL(published.size(), 3);
  const auto digests = user.getSelectionDigestsByProvider(requestId);
  BOOST_REQUIRE_EQUAL(digests.size(), 3);
  std::set<std::string> distinctDigests;
  size_t combinedAssignmentBytes = 0;
  for (const auto& provider : providers) {
    BOOST_CHECK(std::find(published.begin(), published.end(), provider) !=
                published.end());
    const auto found = digests.find(provider.toUri());
    BOOST_REQUIRE(found != digests.end());
    distinctDigests.insert(found->second);
    const auto assignment =
      user.getCollaborationAssignmentForTest(requestId, provider);
    BOOST_REQUIRE(!assignment.empty());
    CollaborationAssignmentEnvelope envelope;
    BOOST_REQUIRE(decodeCollaborationAssignmentEnvelope(assignment, envelope));
    const auto expectedScopeCount =
      provider == providers[0] || provider == providers[2] ? 1 : 2;
    BOOST_CHECK_EQUAL(envelope.scopeKeys.size(), expectedScopeCount);
    BOOST_CHECK_EQUAL(envelope.scopeKeyDataNames.size(), expectedScopeCount);
    if (provider == providers[0]) {
      BOOST_CHECK(envelope.scopeKeys.count("stage-0-to-1") == 1);
      BOOST_CHECK(envelope.scopeKeys.count("stage-1-to-2") == 0);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-0-to-1") == 1);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-1-to-2") == 0);
    }
    else if (provider == providers[1]) {
      BOOST_CHECK(envelope.scopeKeys.count("stage-0-to-1") == 1);
      BOOST_CHECK(envelope.scopeKeys.count("stage-1-to-2") == 1);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-0-to-1") == 1);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-1-to-2") == 1);
    }
    else {
      BOOST_CHECK(envelope.scopeKeys.count("stage-0-to-1") == 0);
      BOOST_CHECK(envelope.scopeKeys.count("stage-1-to-2") == 1);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-0-to-1") == 0);
      BOOST_CHECK(envelope.scopeKeyDataNames.count("stage-1-to-2") == 1);
    }
    combinedAssignmentBytes += assignment.size();
  }
  BOOST_CHECK(combinedAssignmentBytes > 7 * 1024);
  BOOST_CHECK_EQUAL(distinctDigests.size(), 3);

  // Recommitting the identical plan is idempotent: it does not reopen ACKs or
  // publish a second set of provider projections.
  BOOST_CHECK(user.CommitCollaborationPlan(requestId, closed.digest, plan));
  BOOST_CHECK_EQUAL(closureCount, 1);
  BOOST_CHECK_EQUAL(user.getSelectionPublishedProviders(requestId).size(), 3);
  BOOST_CHECK(user.getSelectionDigestsByProvider(requestId) == digests);
}

BOOST_AUTO_TEST_CASE(DeferredCollaborationTracksAckDecryptBeforeClosure)
{
  ndn::security::KeyChain keyChain(
    "pib-memory:deferred-ack-decrypt", "tpm-memory:deferred-ack-decrypt");
  ndn::DummyClientFace face(keyChain);
  auto userCert = makeRsaIdentity(keyChain, ndn::Name("/user/decrypt"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceUser user(
    face, ndn::Name("/test/group"), userCert, aaCert,
    "examples/trust-any.conf");
  const ndn::Name requestId("/request/deferred-ack-decrypt");
  user.prepareDeferredCollaborationForTest(
    requestId, [](const CollaborationAckClosure&) {});

  // A deferred collaboration owns an immutable ACK_CLOSED snapshot. An ACK
  // observed before the deadline must therefore be tracked while its
  // asynchronous decrypt finishes; otherwise the timer can freeze an empty
  // candidate set even though the Provider already accepted the Request.
  BOOST_CHECK(user.tracksAckDecryptForTest(requestId));
}

BOOST_AUTO_TEST_CASE(OperationStatusCodecRetainsMonotonicAndUnknownProgressFields)
{
  ServiceProvider::ServiceOperationStatus status;
  status.operationId = "prepare:prefill";
  status.operation = "MODEL_PREPARE";
  status.serviceName = ndn::Name("/LLM/Qwen");
  status.providerName = ndn::Name("/provider/a");
  status.requestId = ndn::Name("/request/1");
  status.role = "prefill";
  status.attempt = 2;
  status.epoch = 3;
  status.sequence = 4;
  status.state = "LOADING";
  status.progressKnown = false;
  status.progress = 0.0;
  status.detailsSchema = "ndnsf-di-progress-v1";
  status.detailsPayload = ndn::Buffer{0x00, 0x7f, 0xff};

  const auto wire = ServiceProvider::makeServiceOperationStatusPayload(status);
  const auto decoded = ServiceProvider::parseServiceOperationStatusPayload(wire);
  BOOST_REQUIRE(decoded);
  BOOST_CHECK_EQUAL(decoded->role, "prefill");
  BOOST_CHECK_EQUAL(decoded->attempt, 2);
  BOOST_CHECK_EQUAL(decoded->epoch, 3);
  BOOST_CHECK_EQUAL(decoded->sequence, 4);
  BOOST_CHECK(!decoded->progressKnown);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded->detailsPayload.begin(),
                                decoded->detailsPayload.end(),
                                status.detailsPayload.begin(),
                                status.detailsPayload.end());
}

BOOST_AUTO_TEST_CASE(SelectionSnapshotRejectsStaleMemberAndKeepsLatest)
{
  ndn::security::KeyChain keyChain("pib-memory:collab-status",
                                   "tpm-memory:collab-status");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/provider/a"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"),
                                providerCert, aaCert,
                                "examples/trust-any.conf");
  provider.seedSelectionStatusForTest("sha256:selection",
                                      ndn::Name("/LLM/Qwen"),
                                      ndn::Name("/request/1"));

  ServiceProvider::ServiceOperationStatus status;
  status.operationId = "prepare:decode";
  status.operation = "MODEL_PREPARE";
  status.role = "decode";
  status.attempt = 1;
  status.epoch = 1;
  status.sequence = 1;
  status.state = "FETCHING";
  status.progressKnown = true;
  status.progress = 0.25;
  provider.reportSelectionOperationStatus("sha256:selection", status);

  auto snapshot = provider.getSelectionExecutionStatus("sha256:selection");
  BOOST_REQUIRE(snapshot);
  BOOST_REQUIRE_EQUAL(snapshot->memberStatuses.size(), 1);
  BOOST_CHECK_EQUAL(snapshot->memberStatuses.front().role, "decode");
  BOOST_CHECK_CLOSE(snapshot->memberStatuses.front().progress, 0.25, 0.001);

  BOOST_CHECK_THROW(
    provider.reportSelectionOperationStatus("sha256:selection", status),
    std::invalid_argument);
  status.sequence = 2;
  status.state = "VERIFYING";
  status.progress = 0.5;
  provider.reportSelectionOperationStatus("sha256:selection", status);
  snapshot = provider.getSelectionExecutionStatus("sha256:selection");
  BOOST_REQUIRE(snapshot);
  BOOST_REQUIRE_EQUAL(snapshot->memberStatuses.size(), 1);
  BOOST_CHECK_EQUAL(snapshot->memberStatuses.front().sequence, 2);
}

BOOST_AUTO_TEST_CASE(CollaborationFailureUpdatesSelectionStatus)
{
  ndn::security::KeyChain keyChain("pib-memory:collab-failure",
                                   "tpm-memory:collab-failure");
  ndn::DummyClientFace face(keyChain);
  auto providerCert = makeRsaIdentity(keyChain, ndn::Name("/provider/a"));
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"),
                                providerCert, aaCert,
                                "examples/trust-any.conf");
  const std::string selectionDigest = "sha256:failed-selection";
  const ndn::Name serviceName("/LLM/Qwen");
  const ndn::Name requestId("/request/failed-1");
  provider.seedSelectionStatusForTest(selectionDigest, serviceName, requestId);

  provider.failCollaborationForTest(selectionDigest,
                                    serviceName,
                                    requestId,
                                    "model fragment verification failed");

  const auto snapshot = provider.getSelectionExecutionStatus(selectionDigest);
  BOOST_REQUIRE(snapshot);
  BOOST_CHECK(snapshot->state == SelectionExecutionState::Failed);
  BOOST_CHECK_EQUAL(snapshot->serviceName, serviceName);
  BOOST_CHECK_EQUAL(snapshot->requestId, requestId);
  BOOST_CHECK_EQUAL(snapshot->message, "model fragment verification failed");
  BOOST_CHECK_NE(snapshot->completedAtUs, 0);
}

BOOST_AUTO_TEST_CASE(R1DecisionReceiptSurvivesSignedStatusPayloadCodec)
{
  SelectionDecisionReceipt receipt;
  receipt.setField("decisionDigest", "sha256:decision");
  receipt.setField("reservationId", "reservation-1");
  receipt.setField("provider", "/provider/a");
  receipt.setField("acceptedState", "RELEASE_ACCEPTED");
  const auto block = receipt.WireEncode();
  SelectionExecutionStatus status;
  status.providerName = ndn::Name("/provider/a");
  status.serviceName = ndn::Name("/Inference/Generic");
  status.requestId = ndn::Name("request-1");
  status.selectionDigest = "sha256:selection";
  status.state = SelectionExecutionState::Completed;
  status.decisionReceipt = ndn::Buffer(block.data(), block.size());
  const auto payload = LocalServiceProvider::encodeSelectionStatusForTest(status);
  ndn::Data data(ndn::Name("/provider/a/status"));
  data.setContent(payload);
  const auto decoded = LocalServiceUser::parseSelectionStatusForTest(
    data, status.providerName, status.selectionDigest);
  BOOST_CHECK_EQUAL_COLLECTIONS(decoded.decisionReceipt.begin(),
                                decoded.decisionReceipt.end(),
                                status.decisionReceipt.begin(),
                                status.decisionReceipt.end());
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
