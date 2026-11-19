#include "tests/integration-tests/isolated-test-trust.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>

#include <optional>
#include <stdexcept>
#include <vector>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(Spec190AckWindow)

namespace {

class AckWindowUser final : public LocalServiceUser
{
public:
  using LocalServiceUser::LocalServiceUser;

  void
  markAckDecryptInFlight(const ndn::Name& requestId)
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      throw std::logic_error("missing Spec190 pending collaboration");
    }
    pending->second.ackDecryptsInFlight = 1;
  }

  bool
  expireAckWindowForTest(const ndn::Name& requestId)
  {
    return handleAckCollectionTimeout(requestId);
  }

  std::string
  closedAckDigest(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    return pending == m_pendingCalls.end() ? std::string() :
                                             pending->second.collaborationAckClosedDigest;
  }

  size_t
  closedAckCount(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    return pending == m_pendingCalls.end() ? 0 :
                                               pending->second.collaborationClosedAcks.size();
  }

  int
  ackTimeoutMs(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      throw std::logic_error("missing Spec190 pending collaboration");
    }
    return pending->second.ackTimeoutMs;
  }

  std::pair<uint64_t, uint64_t>
  ackWindowBounds(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      throw std::logic_error("missing Spec190 pending collaboration");
    }
    return {pending->second.publishedAtUs,
            pending->second.ackWindowDeadlineUs};
  }

  uint64_t
  requestDeadline(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    if (pending == m_pendingCalls.end()) {
      throw std::logic_error("missing Spec190 pending collaboration");
    }
    return pending->second.requestDeadlineUs;
  }

  void ingestAckPublication(const ndn::svs::SVSPubSub::SubscriptionData& publication)
  {
    OnRequestAck(publication);
  }

  size_t ackDecryptsInFlight(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    return pending == m_pendingCalls.end() ? 0 : pending->second.ackDecryptsInFlight;
  }

  bool
  planCommitted(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    return pending != m_pendingCalls.end() && pending->second.collaborationPlanCommitted;
  }

  std::string
  committedDigest(const ndn::Name& requestId) const
  {
    const auto pending = m_pendingCalls.find(requestId);
    return pending == m_pendingCalls.end() ? std::string{} :
      pending->second.collaborationCommittedPlanDigest;
  }
};

struct AckWindowFixture
{
  AckWindowFixture()
    : keyChain("pib-memory:spec190-ack-window",
               "tpm-memory:spec190-ack-window")
    , face(keyChain)
    , requester("/spec190/user")
    , providerA("/spec190/provider/a")
    , providerB("/spec190/provider/b")
    , service("/spec190/service")
    , userCert(makeRsaIdentity(keyChain, requester))
    , authorityCert(makeRsaIdentity(keyChain, "/spec190/authority"))
    , providerACert(makeRsaIdentity(keyChain, providerA))
    , providerBCert(makeRsaIdentity(keyChain, providerB))
    , trust{userCert, authorityCert, providerACert, providerBCert}
    , user(face, "/spec190/group", userCert, authorityCert, trust.schemaPath())
  {
    installUserPermissions(user, requester, service, {providerA, providerB});
  }

  ndn::security::KeyChain keyChain;
  ndn::DummyClientFace face;
  ndn::Name requester;
  ndn::Name providerA;
  ndn::Name providerB;
  ndn::Name service;
  ndn::security::Certificate userCert;
  ndn::security::Certificate authorityCert;
  ndn::security::Certificate providerACert;
  ndn::security::Certificate providerBCert;
  IsolatedTestTrust trust;
  AckWindowUser user;
};

AckAuthenticationEvidence
makeValidatedAckEvidence(const ndn::Name& provider)
{
  AckAuthenticationEvidence evidence;
  evidence.signerIdentity = provider.toUri();
  evidence.signerKeyLocator = provider.toUri() + "/KEY/spec190";
  evidence.wireDigest = "sha256:spec190-ack-wire";
  evidence.trustSchemaValidated = true;
  return evidence;
}

struct ControlledClock
{
  explicit ControlledClock(uint64_t initialUs)
    : nowUs(initialUs)
  {
    ServiceUser::setTestClockForUnitTests([this] { return nowUs; });
  }

  ~ControlledClock()
  {
    ServiceUser::setTestClockForUnitTests({});
  }

  uint64_t nowUs;
};

/** A deliberately mechanical policy; admission remains in the production Core. */
class Spec231PlanSelection final : public ParticipantSelectionPolicy
{
public:
  explicit Spec231PlanSelection(ndn::Name overrideProvider = {})
    : m_overrideProvider(std::move(overrideProvider))
  {
  }

  std::vector<SelectedParticipant>
  select(const std::vector<AckCandidate>& candidates,
         const std::vector<CollaborationRoleSpec>& roles) const override
  {
    if (candidates.empty() || roles.empty()) return {};
    const auto& candidate = candidates.front();
    SelectedParticipant selected;
    selected.role = roles.front().role;
    selected.service = roles.front().service;
    selected.provider = m_overrideProvider.empty() ? candidate.providerName : m_overrideProvider;
    selected.assignmentPayload = roles.front().assignmentPayload;
    selected.ack = candidate;
    return {selected};
  }

private:
  ndn::Name m_overrideProvider;
};

CollaborationPlan
spec231Plan(const ndn::Name& service, ndn::Name overrideProvider = {})
{
  CollaborationPlan plan;
  plan.ackCollectionTimeMs = 1000;
  plan.timeoutMs = 5000;
  CollaborationRoleSpec role;
  role.role = "worker";
  role.service = service;
  role.assignmentPayload = ndn::Buffer{'o', 'p', 'a', 'q', 'u', 'e'};
  plan.roles.push_back(std::move(role));
  plan.participantSelector = std::make_shared<Spec231PlanSelection>(std::move(overrideProvider));
  return plan;
}

/** Deliver authenticated bytes through OnRequestAck, not an invented ACK evidence record. */
void
spec231DeliverAck(AckWindowFixture& fixture, const RequestMessage& request,
                  const ndn::Name& requestId, const ndn::Name& provider,
                  bool corruptAuthenticationTag = false)
{
  const auto certificate = makeRsaIdentity(fixture.keyChain, provider);
  const ndn::Buffer key(32, 0x37);
  fixture.user.cacheHybridReceiveKeyForTest("spec231-ack-key", "spec231-epoch", key);
  const auto name = makeRequestAckNameV2(provider, fixture.requester, fixture.service, requestId);
  const auto ack = makeSuccessAckForRequest(request, "spec231-token-" + provider.toUri());
  const auto plaintext = ack.WireEncode();
  const auto ad = hybridAssociatedData(name, "ACK", requestId, fixture.service, provider,
                                       "spec231-ack-key", "spec231-epoch");
  const auto encrypted = hybridAesGcmEncrypt(key,
    ndn::span<const uint8_t>(plaintext.data(), plaintext.size()), ad);
  HybridMessageEnvelope envelope;
  envelope.setVersion(1);
  envelope.setAlgorithm("AES-256-GCM");
  envelope.setMessageType("ACK");
  envelope.setKeyId("spec231-ack-key");
  envelope.setEpochId("spec231-epoch");
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  auto tag = encrypted.tag;
  if (corruptAuthenticationTag) tag.front() ^= 1;
  envelope.setAuthTag(tag);
  const auto wire = envelope.WireEncode();
  ndn::Data packet(name);
  packet.setContent(ndn::span<const uint8_t>(wire.data(), wire.size()));
  fixture.keyChain.sign(packet, ndn::security::signingByIdentity(provider));
  BOOST_REQUIRE(ndn::security::verifySignature(packet, certificate));
  const std::optional<ndn::Data> signedPacket{packet};
  const auto prefix = ndn::Name(provider).append("provider").append("0");
  const ndn::svs::SVSPubSub::SubscriptionData publication{
    name, ndn::span<const uint8_t>(wire.data(), wire.size()), prefix, 1, signedPacket};
  fixture.user.ingestAckPublication(publication);
}

} // namespace

BOOST_FIXTURE_TEST_CASE(Spec231PlanRejectsInvalidCommitWithoutSelection, AckWindowFixture)
{
  RequestMessage published;
  ndn::Name selectionRequest;
  std::vector<ServiceSelectionMessage> selections;
  user.setLocalPublicationHandler([&] (const ndn::Name& name, const ndn::Buffer& wire) {
    BOOST_REQUIRE(name == makeServiceSelectionNameV2(requester, providerA, service,
                                                     selectionRequest));
    ServiceSelectionMessage selection;
    BOOST_REQUIRE(selection.WireDecode(ndn::Block(wire)));
    BOOST_REQUIRE_EQUAL(selection.getProviderEntries().size(), 1U);
    const auto& entry = selection.getProviderEntries().front();
    BOOST_CHECK_EQUAL(entry.providerName, providerA);
    CollaborationAssignmentEnvelope assignment;
    BOOST_REQUIRE(decodeCollaborationAssignmentEnvelope(entry.assignmentPayload, assignment));
    BOOST_CHECK_EQUAL(assignment.role, "worker");
    BOOST_CHECK(assignment.opaquePayload == spec231Plan(service).roles.front().assignmentPayload);
    selections.push_back(std::move(selection));
  });
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) { published = request; });
  std::optional<CollaborationAckClosure> closure;
  const auto id = user.BeginCollaborationWithProviders(service, ndn::Buffer{1}, 1000, 5000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {}, [] (const ndn::Name&) {}, {},
    [] (const std::vector<AckCandidate>& candidates) { return !candidates.empty(); },
    {}, std::nullopt, {}, {}, {}, {providerA});
  selectionRequest = id;
  const auto uncommitted = [&] {
    BOOST_CHECK(!user.planCommitted(id));
    BOOST_CHECK(user.getSelectedProvider(id).empty());
    BOOST_CHECK(user.getSelectionPublishedProviders(id).empty());
    BOOST_CHECK(selections.empty());
  };
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(id, "not-closed", spec231Plan(service)),
                    std::logic_error);
  uncommitted();
  spec231DeliverAck(*this, published, id, providerA);
  pumpFace(face, ndn::time::milliseconds(30));
  BOOST_REQUIRE(closure.has_value());
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK(closure->candidates.front().authenticationEvidence.trustSchemaValidated);
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(id, "wrong", spec231Plan(service)),
                    std::invalid_argument);
  uncommitted();
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(id, closure->digest,
                    spec231Plan(service, providerB)), std::invalid_argument);
  uncommitted();
  auto missingRole = spec231Plan(service);
  auto secondRole = missingRole.roles.front();
  secondRole.role = "missing";
  missingRole.roles.push_back(secondRole);
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(id, closure->digest, missingRole),
                    std::invalid_argument);
  uncommitted();
  auto missingDependency = spec231Plan(service);
  missingDependency.dependencies.push_back({{"missing"}, {"worker"}, "input", ndn::Name("/input"), true});
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(id, closure->digest, missingDependency),
                    std::invalid_argument);
  uncommitted();
  auto changedDeadline = spec231Plan(service);
  ++changedDeadline.timeoutMs;
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(id, closure->digest, changedDeadline),
                    std::invalid_argument);
  uncommitted();
  BOOST_REQUIRE(user.CommitCollaborationPlan(id, closure->digest, spec231Plan(service)));
  BOOST_REQUIRE_EQUAL(selections.size(), 1U);
  BOOST_REQUIRE(user.planCommitted(id));
  const auto digest = user.committedDigest(id);
  const auto selected = user.getSelectionPublishedProviders(id);
  BOOST_REQUIRE_EQUAL(selected.size(), 1U);
  BOOST_REQUIRE(!digest.empty());
  BOOST_CHECK_EQUAL(user.getSelectedProvider(id), providerA);
  BOOST_REQUIRE(user.CommitCollaborationPlan(id, closure->digest, spec231Plan(service)));
  BOOST_CHECK_EQUAL(selections.size(), 1U);
  BOOST_CHECK_EQUAL(user.committedDigest(id), digest);
  BOOST_CHECK(user.getSelectionPublishedProviders(id) == selected);
  auto conflict = spec231Plan(service);
  conflict.roles.front().assignmentPayload.push_back('x');
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(id, closure->digest, conflict), std::logic_error);
  BOOST_CHECK_EQUAL(selections.size(), 1U);
  BOOST_CHECK_EQUAL(user.committedDigest(id), digest);
  BOOST_CHECK(user.CancelCollaboration(id));
}

BOOST_FIXTURE_TEST_CASE(Spec231ExpiredPlanCannotPublishSelection, AckWindowFixture)
{
  ControlledClock clock(1000000);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) { published = request; });
  std::optional<CollaborationAckClosure> closure;
  const auto id = user.BeginCollaborationWithProviders(service, ndn::Buffer{1}, 1000, 5000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {}, [] (const ndn::Name&) {}, {},
    [] (const std::vector<AckCandidate>& candidates) { return !candidates.empty(); },
    {}, std::nullopt, {}, {}, {}, {providerA});
  spec231DeliverAck(*this, published, id, providerA);
  pumpFace(face, ndn::time::milliseconds(30));
  BOOST_REQUIRE(closure.has_value());
  clock.nowUs = user.requestDeadline(id);
  BOOST_CHECK_THROW(user.CommitCollaborationPlan(id, closure->digest, spec231Plan(service)),
                    std::runtime_error);
  BOOST_CHECK(!user.planCommitted(id));
  BOOST_CHECK(user.getSelectionPublishedProviders(id).empty());
  BOOST_CHECK(user.CancelCollaboration(id));
}

BOOST_FIXTURE_TEST_CASE(Spec231ValidSignatureDoesNotReplaceServicePermission, AckWindowFixture)
{
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) { published = request; });
  std::optional<CollaborationAckClosure> closure;
  const auto id = user.BeginCollaborationWithProviders(service, ndn::Buffer{1}, 1000, 5000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {}, [] (const ndn::Name&) {}, {},
    [] (const std::vector<AckCandidate>& candidates) { return !candidates.empty(); },
    {}, std::nullopt, {}, {}, {}, {providerA, providerB});
  // Replace the permission table after solicitation, before admitting either ACK.
  installUserPermissions(user, requester, service, {providerB}, 2);
  spec231DeliverAck(*this, published, id, providerA);
  pumpFace(face, ndn::time::milliseconds(30));
  BOOST_CHECK(!closure.has_value());
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(id), 0U);
  BOOST_CHECK(!user.planCommitted(id));
  BOOST_CHECK(user.getSelectionPublishedProviders(id).empty());
  spec231DeliverAck(*this, published, id, providerB);
  pumpFace(face, ndn::time::milliseconds(30));
  BOOST_REQUIRE(closure.has_value());
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK_EQUAL(closure->candidates.front().providerName, providerB);
  BOOST_CHECK(user.CancelCollaboration(id));
}

BOOST_FIXTURE_TEST_CASE(UnrequestedEncryptedAckDoesNotConsumeCollectionBudget,
                        AckWindowFixture)
{
  const auto certificateA = makeRsaIdentity(keyChain, providerA);
  const auto certificateB = makeRsaIdentity(keyChain, providerB);
  const ndn::Buffer key(32, 0x42);
  user.cacheHybridReceiveKeyForTest("spec202-ack-key", "spec202-epoch", key);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) { published = request; });
  const ndn::Buffer payload(1, 0x62);
  std::optional<CollaborationAckClosure> closure;
  bool mismatchedSignerObserved = false;
  const auto begin = [&] (const std::vector<ndn::Name>& providers) {
    return user.BeginCollaborationWithProviders(service, payload, 1000, 5000,
      [&] (const CollaborationAckClosure& value) { closure = value; },
      [] (const ResponseMessage&) {}, [] (const ndn::Name&) {}, ndn::Name(),
      [&] (const std::vector<AckCandidate>& candidates) {
        for (const auto& candidate : candidates) {
          mismatchedSignerObserved = mismatchedSignerObserved ||
            candidate.authenticationEvidence.signerIdentity != candidate.providerName.toUri();
        }
        return std::any_of(candidates.begin(), candidates.end(), [] (const auto& candidate) {
          return candidate.ack.getStatus() &&
                 candidate.authenticationEvidence.trustSchemaValidated &&
                 candidate.authenticationEvidence.signerIdentity == candidate.providerName.toUri();
        });
      },
      RequestCapabilities(), std::nullopt, {}, {}, {}, providers);
  };
  const auto deliver = [&] (const ndn::Name& provider, const ndn::Name& signer,
                            const ndn::security::Certificate& certificate,
                            const ndn::Name& requestId) {
    const auto name = makeRequestAckNameV2(provider, requester, service, requestId);
    const auto ack = makeSuccessAckForRequest(published, "spec202-token-" + provider.toUri());
    const auto plaintext = ack.WireEncode();
    const auto ad = hybridAssociatedData(name, "ACK", requestId, service, provider,
                                         "spec202-ack-key", "spec202-epoch");
    const auto encrypted = hybridAesGcmEncrypt(key,
      ndn::span<const uint8_t>(plaintext.data(), plaintext.size()), ad);
    HybridMessageEnvelope envelope;
    envelope.setVersion(1);
    envelope.setAlgorithm("AES-256-GCM");
    envelope.setMessageType("ACK");
    envelope.setKeyId("spec202-ack-key");
    envelope.setEpochId("spec202-epoch");
    envelope.setNonce(encrypted.nonce);
    envelope.setCipherText(encrypted.ciphertext);
    envelope.setAuthTag(encrypted.tag);
    const auto wire = envelope.WireEncode();
    ndn::Data packet(name);
    packet.setContent(ndn::span<const uint8_t>(wire.data(), wire.size()));
    keyChain.sign(packet, ndn::security::signingByIdentity(signer));
    BOOST_REQUIRE(ndn::security::verifySignature(packet, certificate));
    const std::optional<ndn::Data> signedPacket{packet};
    const ndn::Name producerPrefix = ndn::Name(provider).append("provider").append("0");
    const ndn::svs::SVSPubSub::SubscriptionData publication{
      name, ndn::span<const uint8_t>(wire.data(), wire.size()),
      producerPrefix, 1, signedPacket};
    user.ingestAckPublication(publication);
  };
  const auto requestId = begin({providerA});
  BOOST_REQUIRE(!requestId.empty());
  const auto bounds = user.ackWindowBounds(requestId);
  const auto decrypts = user.getHybridCryptoCounters().symmetric_decrypt_count.load();
  deliver(providerB, providerB, certificateB, requestId);
  BOOST_CHECK_EQUAL(user.ackDecryptsInFlight(requestId), 0U);
  pumpFace(face, ndn::time::milliseconds(10));
  // Authorized reader observation has its own bounded lease. It may decrypt
  // this ACK, but cannot consume the candidate collection lease or close it.
  BOOST_CHECK_EQUAL(user.getHybridCryptoCounters().symmetric_decrypt_count.load(), decrypts + 1);
  BOOST_CHECK(user.ackWindowBounds(requestId) == bounds);
  BOOST_CHECK(!closure);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 0U);
  BOOST_CHECK(user.getSelectionPublishedProviders(requestId).empty());
  deliver(providerA, providerA, certificateA, requestId);
  pumpFace(face, ndn::time::milliseconds(10));
  BOOST_REQUIRE(closure);
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK(closure->candidates.front().providerName == providerA);
  BOOST_CHECK(closure->candidates.front().authenticationEvidence.trustSchemaValidated);
  BOOST_CHECK(user.CancelCollaboration(requestId));
  closure.reset();
  // A valid signature from another Provider cannot authorize A's namespace.
  // Core must reject it before decryption or the caller's coverage predicate.
  const auto forged = begin({providerA});
  const auto decryptsBeforeForgery =
    user.getHybridCryptoCounters().symmetric_decrypt_count.load();
  deliver(providerA, providerB, certificateB, forged);
  pumpFace(face, ndn::time::milliseconds(10));
  BOOST_CHECK(!mismatchedSignerObserved);
  BOOST_CHECK_EQUAL(user.getHybridCryptoCounters().symmetric_decrypt_count.load(),
                    decryptsBeforeForgery);
  BOOST_CHECK_EQUAL(user.ackDecryptsInFlight(forged), 0U);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(forged), 0U);
  BOOST_CHECK(!closure);
  BOOST_CHECK(user.CancelCollaboration(forged));
  const auto discovery = begin({});
  deliver(providerB, providerB, certificateB, discovery);
  pumpFace(face, ndn::time::milliseconds(10));
  BOOST_REQUIRE(closure);
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK(closure->candidates.front().providerName == providerB);
  BOOST_CHECK(user.CancelCollaboration(discovery));
}

BOOST_FIXTURE_TEST_CASE(RejectsInvalidAndAcceptsBoundaryAckWindows,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  ControlledClock clock(1'000'000);

  const auto expectRejected = [&] (int ackMs, int timeoutMs, int suffix) {
    BOOST_CHECK_THROW(
      user.BeginCollaboration(
        service, payload, ackMs, timeoutMs,
        [] (const CollaborationAckClosure&) {},
        [] (const ResponseMessage&) {},
        [] (const ndn::Name&) {},
        ndn::Name("/spec190/invalid").append(std::to_string(suffix))),
      std::invalid_argument);
  };

  expectRejected(0, 5000, 0);
  expectRejected(-1, 5000, 1);
  expectRejected(5000, 5000, 2);
  expectRejected(5001, 5000, 3);

  for (const int ackMs : {999, 1000, 1001}) {
    std::optional<CollaborationAckClosure> closure;
    const auto requestId = user.BeginCollaboration(
      service, payload, ackMs, 5000,
      [&] (const CollaborationAckClosure& value) { closure = value; },
      [] (const ResponseMessage&) {},
      [] (const ndn::Name&) {},
      ndn::Name());
    BOOST_REQUIRE(!requestId.empty());
    BOOST_CHECK_EQUAL(user.ackTimeoutMs(requestId), ackMs);
    const auto [publishedAtUs, deadlineUs] = user.ackWindowBounds(requestId);
    BOOST_REQUIRE_EQUAL(deadlineUs - publishedAtUs,
                        static_cast<uint64_t>(ackMs) * 1000);
    BOOST_CHECK(!closure.has_value());
    clock.nowUs = deadlineUs - 1;
    BOOST_CHECK(!user.expireAckWindowForTest(requestId));
    BOOST_CHECK(!closure.has_value());
    clock.nowUs = deadlineUs;
    // Drive the production timer callback at the controlled deadline instead
    // of depending on one millisecond wall-clock scheduling slack.
    BOOST_CHECK(user.expireAckWindowForTest(requestId));
    BOOST_REQUIRE(closure.has_value());
    BOOST_CHECK(user.CancelCollaboration(requestId));
  }
}

BOOST_FIXTURE_TEST_CASE(OneSecondWindowFreezesAuthenticatedSnapshot,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) {
      published = request;
    });

  std::optional<CollaborationAckClosure> closure;
  size_t closureCount = 0;
  const auto requestId = user.BeginCollaboration(
    service, payload, 1000, 5000,
    [&] (const CollaborationAckClosure& value) {
      ++closureCount;
      closure = value;
    },
    [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {});
  BOOST_REQUIRE(!requestId.empty());
  BOOST_REQUIRE(!published.getPayload().empty());

  const auto firstAck = makeSuccessAckForRequest(published, "token-a");
  BOOST_REQUIRE(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId), firstAck,
    makeValidatedAckEvidence(providerA)));
  BOOST_CHECK(!user.isAckWindowExpired(requestId));

  // The replay is rejected before it can change the candidate snapshot.
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId), firstAck,
    makeValidatedAckEvidence(providerA)));

  pumpFace(face, ndn::time::milliseconds(1100));
  BOOST_REQUIRE(closure.has_value());
  BOOST_CHECK_EQUAL(closureCount, 1U);
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK_EQUAL(closure->candidates.front().providerName, providerA);
  BOOST_CHECK(closure->candidates.front().authenticationEvidence.trustSchemaValidated);
  BOOST_CHECK(user.isAckWindowExpired(requestId));
  const auto frozenDigest = user.closedAckDigest(requestId);
  const auto frozenCount = user.closedAckCount(requestId);
  BOOST_REQUIRE(!frozenDigest.empty());
  BOOST_CHECK_EQUAL(frozenCount, 1U);

  // An unauthorized Provider is rejected after the deadline and cannot
  // reopen or mutate ACK_CLOSED.
  const ndn::Name unauthorized("/spec190/provider/unauthorized");
  const auto unauthorizedAck = makeSuccessAckForRequest(published, "token-x");
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(unauthorized, requester, service, requestId),
    unauthorizedAck));

  // A valid late ACK may reach the request handler, but the immutable
  // ACK_CLOSED candidate set remains the one-second snapshot.
  const auto lateAck = makeSuccessAckForRequest(published, "token-b");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requester, service, requestId), lateAck,
    makeValidatedAckEvidence(providerB)));
  BOOST_CHECK_EQUAL(closureCount, 1U);
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK_EQUAL(closure->candidates.front().providerName, providerA);
  BOOST_CHECK_EQUAL(user.closedAckDigest(requestId), frozenDigest);
  BOOST_CHECK_EQUAL(user.closedAckCount(requestId), frozenCount);
  BOOST_CHECK(user.CancelCollaboration(requestId));
}

BOOST_FIXTURE_TEST_CASE(ExplicitProviderCoverageClosesBeforeDeadline,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) {
      published = request;
    });

  const std::vector<ndn::Name> expectedProviders{providerA, providerB};
  std::optional<CollaborationAckClosure> closure;
  size_t coverageChecks = 0;
  const auto requestId = user.BeginCollaborationWithProviders(
    service, payload, 1000, 5000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {},
    ndn::Name(),
    [&] (const std::vector<AckCandidate>& candidates) {
      ++coverageChecks;
      return std::all_of(expectedProviders.begin(), expectedProviders.end(),
        [&candidates] (const ndn::Name& expectedProvider) {
          return std::any_of(candidates.begin(), candidates.end(),
            [&expectedProvider] (const auto& candidate) {
              return candidate.providerName == expectedProvider &&
                     candidate.ack.getStatus();
            });
        });
    },
    RequestCapabilities(), std::nullopt, {}, {}, {}, expectedProviders);
  BOOST_REQUIRE(!requestId.empty());
  BOOST_REQUIRE(!published.getPayload().empty());

  const auto firstAck = makeSuccessAckForRequest(published, "token-a");
  BOOST_REQUIRE(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId), firstAck,
    makeValidatedAckEvidence(providerA)));
  BOOST_CHECK(!closure.has_value());

  const auto secondAck = makeSuccessAckForRequest(published, "token-b");
  BOOST_REQUIRE(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requester, service, requestId), secondAck,
    makeValidatedAckEvidence(providerB)));
  BOOST_REQUIRE(closure.has_value());
  BOOST_CHECK_EQUAL(closure->candidates.size(), 2U);
  BOOST_CHECK_GE(coverageChecks, 2U);
  BOOST_CHECK(user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.CancelCollaboration(requestId));
}

BOOST_FIXTURE_TEST_CASE(ExplicitProviderCoverageWaitsForMissingProvider,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) {
      published = request;
    });

  const std::vector<ndn::Name> expectedProviders{providerA, providerB};
  std::optional<CollaborationAckClosure> closure;
  const auto requestId = user.BeginCollaborationWithProviders(
    service, payload, 20, 1000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {},
    ndn::Name(),
    [&] (const std::vector<AckCandidate>& candidates) {
      return std::all_of(expectedProviders.begin(), expectedProviders.end(),
        [&candidates] (const ndn::Name& expectedProvider) {
          return std::any_of(candidates.begin(), candidates.end(),
            [&expectedProvider] (const auto& candidate) {
              return candidate.providerName == expectedProvider &&
                     candidate.ack.getStatus();
            });
        });
    },
    RequestCapabilities(), std::nullopt, {}, {}, {}, expectedProviders);
  BOOST_REQUIRE(!requestId.empty());
  BOOST_REQUIRE(!published.getPayload().empty());

  const auto firstAck = makeSuccessAckForRequest(published, "token-a");
  BOOST_REQUIRE(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId), firstAck,
    makeValidatedAckEvidence(providerA)));
  BOOST_CHECK(!closure.has_value());

  pumpFace(face, ndn::time::milliseconds(30));
  BOOST_REQUIRE(closure.has_value());
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK(user.CancelCollaboration(requestId));
}

BOOST_FIXTURE_TEST_CASE(DelayedAuthenticationDrainsBeforeAckClosed,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  ControlledClock clock(2'000'000);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) {
      published = request;
    });

  std::optional<CollaborationAckClosure> closure;
  const auto requestId = user.BeginCollaboration(
    service, payload, 20, 1000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {});
  BOOST_REQUIRE(!requestId.empty());
  BOOST_REQUIRE(!published.getPayload().empty());
  const auto bounds = user.ackWindowBounds(requestId);

  // Model the production ACK decrypt/auth callback already admitted by the
  // SVS path but not yet drained when the timer fires.
  user.markAckDecryptInFlight(requestId);
  for (int deferral = 0; deferral < 6; ++deferral) {
    clock.nowUs = bounds.second + static_cast<uint64_t>(deferral) * 20'000;
    BOOST_CHECK(!user.expireAckWindowForTest(requestId));
    BOOST_CHECK(!closure.has_value());
  }

  auto authenticatedAck = makeSuccessAckForRequest(published, "token-auth");
  BOOST_REQUIRE(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId),
    authenticatedAck, makeValidatedAckEvidence(providerA)));
  BOOST_CHECK(!closure.has_value());
  pumpFace(face, ndn::time::milliseconds(30));
  BOOST_REQUIRE(closure.has_value());
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK(closure->candidates.front().authenticationEvidence.trustSchemaValidated);
  BOOST_CHECK_EQUAL(user.closedAckCount(requestId), 1U);
  BOOST_CHECK(user.CancelCollaboration(requestId));
}

BOOST_FIXTURE_TEST_CASE(DeferredAckClosedWaitsForAuthenticationOrRequestTimeout,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  ControlledClock clock(4'000'000);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) {
      published = request;
    });
  const std::vector<ndn::Name> providers{providerA};
  const auto beginDeferred = [&] (std::optional<CollaborationAckClosure>& closure,
                                  bool& timedOut) {
    return user.BeginCollaborationWithProviders(
      service, payload, 20, 1000,
      [&] (const CollaborationAckClosure& value) { closure = value; },
      [] (const ResponseMessage&) {},
      [&] (const ndn::Name&) { timedOut = true; },
      ndn::Name(),
      [] (const std::vector<AckCandidate>&) { return false; },
      RequestCapabilities(), std::nullopt, {}, {}, {}, providers);
  };

  std::optional<CollaborationAckClosure> closure;
  bool timedOut = false;
  const auto requestId = beginDeferred(closure, timedOut);
  BOOST_REQUIRE(!requestId.empty());
  BOOST_REQUIRE(!published.getPayload().empty());
  const auto bounds = user.ackWindowBounds(requestId);
  BOOST_CHECK_GT(user.requestDeadline(requestId), bounds.second + 100'000);
  user.markAckDecryptInFlight(requestId);
  for (int deferral = 0; deferral < 6; ++deferral) {
    clock.nowUs = bounds.second + static_cast<uint64_t>(deferral) * 20'000;
    BOOST_CHECK(!user.expireAckWindowForTest(requestId));
    BOOST_CHECK(!closure.has_value());
  }
  const auto ack = makeSuccessAckForRequest(published, "token-auth");
  BOOST_REQUIRE(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId), ack,
    makeValidatedAckEvidence(providerA)));
  pumpFace(face, ndn::time::milliseconds(30));
  BOOST_REQUIRE(closure.has_value());
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK(!timedOut);
  BOOST_CHECK(user.CancelCollaboration(requestId));

  std::optional<CollaborationAckClosure> expiredClosure;
  bool expiredTimedOut = false;
  const auto expiredRequestId = beginDeferred(expiredClosure, expiredTimedOut);
  BOOST_REQUIRE(!expiredRequestId.empty());
  user.markAckDecryptInFlight(expiredRequestId);
  clock.nowUs = user.requestDeadline(expiredRequestId);
  BOOST_CHECK(!user.expireAckWindowForTest(expiredRequestId));
  BOOST_CHECK(!expiredClosure.has_value());
  pumpFace(face, ndn::time::milliseconds(1100));
  BOOST_CHECK(expiredTimedOut);
  BOOST_CHECK(!expiredClosure.has_value());
}

BOOST_FIXTURE_TEST_CASE(CancelAtAckDeadlineSuppressesDeferredClosure,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  ControlledClock clock(3'000'000);
  std::optional<CollaborationAckClosure> closure;
  const auto requestId = user.BeginCollaboration(
    service, payload, 10, 1000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {});
  BOOST_REQUIRE(!requestId.empty());
  const auto bounds = user.ackWindowBounds(requestId);
  user.markAckDecryptInFlight(requestId);
  clock.nowUs = bounds.second;
  BOOST_CHECK(!user.expireAckWindowForTest(requestId));
  BOOST_CHECK(user.CancelCollaboration(requestId));
  // The opposite same-tick ordering (cancel first, timer callback second)
  // must also be a no-op after the PendingCall has been removed.
  BOOST_CHECK(!user.expireAckWindowForTest(requestId));
  pumpFace(face, ndn::time::milliseconds(40));
  BOOST_CHECK(!closure.has_value());
}

BOOST_FIXTURE_TEST_CASE(InvalidUserTokenCannotBecomeAckCandidate,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) {
      published = request;
    });

  std::optional<CollaborationAckClosure> closure;
  const auto requestId = user.BeginCollaboration(
    service, payload, 20, 1000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {});
  BOOST_REQUIRE(!requestId.empty());
  BOOST_REQUIRE(!published.getUserToken().empty());

  auto invalidAck = makeSuccessAckForRequest(published, "token-invalid");
  invalidAck.setUserToken("wrong-user-token");
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId), invalidAck,
    makeValidatedAckEvidence(providerA)));
  pumpFace(face, ndn::time::milliseconds(40));
  BOOST_REQUIRE(closure.has_value());
  BOOST_CHECK(closure->candidates.empty());
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK(user.CancelCollaboration(requestId));
}

BOOST_FIXTURE_TEST_CASE(DecryptionFailureCannotBecomeAckCandidate,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) { published = request; });
  std::optional<CollaborationAckClosure> closure;
  const auto requestId = user.BeginCollaboration(
    service, payload, 20, 1000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {});
  BOOST_REQUIRE(!requestId.empty());

  // The signature and canonical actor are valid. Only GCM authentication is
  // invalid, so the failure counter proves arrival beyond the ingress filter.
  const auto failures = user.getHybridCryptoCounters().auth_decrypt_failure_count.load();
  const auto successes = user.getHybridCryptoCounters().symmetric_decrypt_count.load();
  spec231DeliverAck(*this, published, requestId, providerA, true);
  for (size_t attempt = 0; attempt < 200 && !closure; ++attempt) {
    pumpFace(face, ndn::time::milliseconds(5));
  }
  BOOST_CHECK_EQUAL(user.getHybridCryptoCounters().auth_decrypt_failure_count.load(), failures + 1);
  BOOST_CHECK_EQUAL(user.getHybridCryptoCounters().symmetric_decrypt_count.load(), successes);
  BOOST_CHECK_EQUAL(user.ackDecryptsInFlight(requestId), 0U);

  BOOST_REQUIRE(closure.has_value());
  BOOST_CHECK(closure->candidates.empty());
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK(user.getSelectionPublishedProviders(requestId).empty());
  BOOST_CHECK(user.CancelCollaboration(requestId));
}

BOOST_FIXTURE_TEST_CASE(NegativeAckIsRetainedForClosedSnapshotWithoutSelection,
                        AckWindowFixture)
{
  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload"), 7);
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& request, size_t) {
      published = request;
    });

  std::optional<CollaborationAckClosure> closure;
  const auto requestId = user.BeginCollaboration(
    service, payload, 20, 1000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {});
  BOOST_REQUIRE(!requestId.empty());

  auto negativeAck = makeSuccessAckForRequest(published, "token-negative");
  negativeAck.setStatus(false);
  negativeAck.setMessage("MODEL_UNAVAILABLE");
  BOOST_REQUIRE(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId), negativeAck,
    makeValidatedAckEvidence(providerA)));
  pumpFace(face, ndn::time::milliseconds(40));

  BOOST_REQUIRE(closure.has_value());
  BOOST_REQUIRE_EQUAL(closure->candidates.size(), 1U);
  BOOST_CHECK(!closure->candidates.front().ack.getStatus());
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK(user.CancelCollaboration(requestId));
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
