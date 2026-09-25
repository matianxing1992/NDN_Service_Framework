#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

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
    , user(face, "/spec190/group", userCert, authorityCert,
           "examples/trust-any.conf")
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

} // namespace

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
  clock.nowUs = bounds.second;
  BOOST_CHECK(!user.expireAckWindowForTest(requestId));
  BOOST_CHECK(!closure.has_value());

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
  std::optional<CollaborationAckClosure> closure;
  const auto requestId = user.BeginCollaboration(
    service, payload, 20, 1000,
    [&] (const CollaborationAckClosure& value) { closure = value; },
    [] (const ResponseMessage&) {},
    [] (const ndn::Name&) {});
  BOOST_REQUIRE(!requestId.empty());

  // The publication reaches the production ACK decrypt boundary, but its
  // plaintext RequestAckMessage wire is intentionally not a hybrid envelope.
  // Decrypt failure must not create an authenticated candidate.
  auto malformedEnvelope = makeSuccessAck();
  user.deliverPlaintextAckPublicationForTest(
    providerA, service, requestId, malformedEnvelope);
  pumpFace(face, ndn::time::milliseconds(60));

  BOOST_REQUIRE(closure.has_value());
  BOOST_CHECK(closure->candidates.empty());
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
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
