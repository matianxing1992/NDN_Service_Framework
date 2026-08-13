#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(SelectionStrategies)

BOOST_AUTO_TEST_CASE(FirstRespondingResponseTimeoutReselectsBoundedAlternate)
{
  ndn::security::KeyChain keyChain("pib-memory:response-reselection",
                                   "tpm-memory:response-reselection");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/reselection");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name service("/Inference/Generic");
  auto cert = makeRsaIdentity(keyChain, requester);
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa-reselection"));
  LocalServiceUser user(face, ndn::Name("/test/group"), cert, aa,
                        "examples/trust-any.conf");
  installUserPermissions(user, requester, service, {providerA, providerB});
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& value, size_t) { published = value; });
  ServiceUser::ResponseRetryOptions retry;
  retry.enabled = true;
  retry.attemptTimeoutMs = 10;
  retry.maxAttempts = 2;
  user.setResponseRetryOptions(retry);

  RequestMessage request;
  const auto requestId = user.RequestService(
    {providerA, providerB}, service, request, 50,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection, 200,
    [](const ndn::Name&) {}, [](const ResponseMessage&) {});

  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId),
    makeSuccessAckForRequest(published, "token-a")));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);

  // A later ACK is retained as a standby after FirstResponding has selected A.
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requester, service, requestId),
    makeSuccessAckForRequest(published, "token-b")));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 2);

  pumpFace(face, ndn::time::milliseconds(30));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerB);
  const auto selections = user.getSelectionPublishedProviders(requestId);
  BOOST_REQUIRE_EQUAL(selections.size(), 2);
  BOOST_CHECK_EQUAL(selections[0], providerA);
  BOOST_CHECK_EQUAL(selections[1], providerB);

  // maxAttempts includes the initial selection, so no third selection can be
  // scheduled even if the global request remains pending.
  pumpFace(face, ndn::time::milliseconds(30));
  BOOST_CHECK_EQUAL(user.getSelectionPublishedProviders(requestId).size(), 2);
}

BOOST_AUTO_TEST_CASE(FirstRespondingResponseRetryDecryptsLateAckPublication)
{
  ScopedEnvironmentValue cryptoDiag("NDNSF_CRYPTO_DIAG", "1");
  ScopedEnvironmentValue plaintextAck("NDNSF_DIAG_PLAINTEXT_ACK", "1");
  ndn::security::KeyChain keyChain("pib-memory:response-retry-late-ack",
                                   "tpm-memory:response-retry-late-ack");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/late-ack");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name service("/Inference/Generic");
  auto cert = makeRsaIdentity(keyChain, requester);
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa-late-ack"));
  LocalServiceUser user(face, ndn::Name("/test/group"), cert, aa,
                        "examples/trust-any.conf");
  installUserPermissions(user, requester, service, {providerA, providerB});
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& value, size_t) { published = value; });
  ServiceUser::ResponseRetryOptions retry;
  retry.enabled = true;
  retry.attemptTimeoutMs = 100;
  retry.maxAttempts = 2;
  user.setResponseRetryOptions(retry);

  const auto requestId = user.RequestService(
    {providerA, providerB}, service, RequestMessage(), 50,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection, 500,
    [](const ndn::Name&) {}, [](const ResponseMessage&) {});

  user.deliverPlaintextAckPublicationForTest(
    providerA, service, requestId,
    makeSuccessAckForRequest(published, "token-a"));
  pumpFace(face, 20_ms);
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);

  // This exercises the real SVS subscription callback seam.  Response retry
  // must keep decrypting ACKs from alternate Providers after FirstResponding
  // has already selected its initial Provider.
  user.deliverPlaintextAckPublicationForTest(
    providerB, service, requestId,
    makeSuccessAckForRequest(published, "token-b"));
  pumpFace(face, 20_ms);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 2);
  const auto candidates = user.getSuccessfulAckProviders(requestId);
  BOOST_CHECK(std::find(candidates.begin(), candidates.end(), providerB) !=
              candidates.end());
}

BOOST_AUTO_TEST_CASE(R1ReservationSelectionClosesOnlyAtDeadlineAndTargetsEveryLease)
{
  ndn::security::KeyChain keyChain("pib-memory:r1-decision-closure",
                                   "tpm-memory:r1-decision-closure");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/r1");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name service("/Inference/Generic");
  auto cert = makeRsaIdentity(keyChain, requester);
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa-r1"));
  auto providerCertA = makeRsaIdentity(keyChain, providerA);
  auto providerCertB = makeRsaIdentity(keyChain, providerB);
  LocalServiceUser user(face, ndn::Name("/test/group"), cert, aa,
                        "examples/trust-any.conf");
  installUserPermissions(user, requester, service, {providerA, providerB});
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& value, size_t) { published = value; });
  RequestCapabilities capabilities;
  capabilities.setField("DIReservationSelectionV1", "required");
  RequestMessage request;
  request.setRequestCapabilities(capabilities);
  const auto requestId = user.RequestService(
    {providerA, providerB}, service, request, 30,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection, 200,
    [](const ndn::Name&) {}, [](const ResponseMessage&) {});

  auto makeAck = [&published] (const std::string& token,
                               const std::string& reservation,
                               const ndn::security::Certificate& providerCert) {
    auto ack = makeSuccessAckForRequest(published, token);
    ReservationLease lease;
    lease.setField("reservationId", reservation);
    lease.setField("providerBootEpoch", "boot-1");
    lease.setField("expiresAtMs", "9999999999999");
    ack.setReservationLease(lease);
    ack.setSelectionInputKeyOffer(makeSelectionInputKeyOffer(providerCert));
    return ack;
  };
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requester, service, requestId),
    makeAck("token-a", "reservation-a", providerCertA)));
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requester, service, requestId),
    makeAck("token-b", "reservation-b", providerCertB)));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK(user.getSelectionPublishedProviders(requestId).empty());

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(user.isAckWindowExpired(requestId));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  const auto decisions = user.getSelectionPublishedProviders(requestId);
  BOOST_CHECK_EQUAL(decisions.size(), 2);
  BOOST_CHECK(namesContain(decisions, providerA));
  BOOST_CHECK(namesContain(decisions, providerB));
}

BOOST_AUTO_TEST_CASE(R1LatePositiveAckReceivesNotSelectedWithoutReopeningWindow)
{
  ndn::security::KeyChain keyChain("pib-memory:r1-late-negative",
                                   "tpm-memory:r1-late-negative");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/r1-late");
  const ndn::Name provider("/test/provider/late");
  const ndn::Name service("/Inference/Generic");
  auto cert = makeRsaIdentity(keyChain, requester);
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa-r1-late"));
  auto providerCert = makeRsaIdentity(keyChain, provider);
  LocalServiceUser user(face, ndn::Name("/test/group"), cert, aa,
                        "examples/trust-any.conf");
  installUserPermissions(user, requester, service, {provider});
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& value, size_t) { published = value; });
  RequestCapabilities capabilities;
  capabilities.setField("DIReservationSelectionV1", "required");
  RequestMessage request;
  request.setRequestCapabilities(capabilities);
  const auto requestId = user.RequestService(
    {provider}, service, request, 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection, 200,
    [](const ndn::Name&) {}, [](const ResponseMessage&) {});
  pumpFace(face, ndn::time::milliseconds(20));
  BOOST_REQUIRE(user.isAckWindowExpired(requestId));

  auto ack = makeSuccessAckForRequest(published, "late-token");
  ReservationLease lease;
  lease.setField("reservationId", "late-reservation");
  lease.setField("providerBootEpoch", "boot-1");
  lease.setField("expiresAtMs", "9999999999999");
  ack.setReservationLease(lease);
  ack.setSelectionInputKeyOffer(makeSelectionInputKeyOffer(providerCert));
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(provider, requester, service, requestId), ack));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  const auto decisions = user.getSelectionPublishedProviders(requestId);
  BOOST_REQUIRE_EQUAL(decisions.size(), 1);
  BOOST_CHECK_EQUAL(decisions.front(), provider);
}

BOOST_AUTO_TEST_CASE(R1LostReceiptRetriesExactDecisionAtMostTwice)
{
  ndn::security::KeyChain keyChain("pib-memory:r1-receipt-retry",
                                   "tpm-memory:r1-receipt-retry");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/r1-retry");
  const ndn::Name provider("/test/provider/r1-retry");
  const ndn::Name service("/Inference/Generic");
  auto cert = makeRsaIdentity(keyChain, requester);
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa-r1-retry"));
  auto providerCert = makeRsaIdentity(keyChain, provider);
  LocalServiceUser user(face, ndn::Name("/test/group"), cert, aa,
                        "examples/trust-any.conf");
  installUserPermissions(user, requester, service, {provider});
  RequestMessage published;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& value, size_t) { published = value; });
  RequestCapabilities capabilities;
  capabilities.setField("DIReservationSelectionV1", "required");
  RequestMessage request;
  request.setRequestCapabilities(capabilities);
  const auto requestId = user.RequestService(
    {provider}, service, request, 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection, 1000,
    [](const ndn::Name&) {}, [](const ResponseMessage&) {});
  auto ack = makeSuccessAckForRequest(published, "retry-token");
  ReservationLease lease;
  lease.setField("reservationId", "reservation-retry");
  lease.setField("providerBootEpoch", "boot-1");
  lease.setField("expiresAtMs", "9999999999999");
  ack.setReservationLease(lease);
  ack.setSelectionInputKeyOffer(makeSelectionInputKeyOffer(providerCert));
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(provider, requester, service, requestId), ack));
  pumpFace(face, ndn::time::milliseconds(500));
  BOOST_CHECK_EQUAL(user.getR1DecisionTransmissionCount(
                      requestId, "reservation-retry"), 3);
  BOOST_CHECK(!user.getR1DecisionDigestForTest(
                      requestId, "reservation-retry").empty());
  pumpFace(face, ndn::time::milliseconds(300));
  BOOST_CHECK_EQUAL(user.getR1DecisionTransmissionCount(
                      requestId, "reservation-retry"), 3);
}

BOOST_AUTO_TEST_CASE(R1DecisionAuthenticatesBeforeReleaseAndIsImmutable)
{
  ndn::security::KeyChain keyChain("pib-memory:r1-provider-decision",
                                   "tpm-memory:r1-provider-decision");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/r1-provider");
  const ndn::Name providerName("/test/provider/r1-provider");
  const ndn::Name service("/Inference/Generic");
  const ndn::Name requestId("request-r1-provider");
  auto cert = makeRsaIdentity(keyChain, providerName);
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa-r1-provider"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"), cert, aa,
                                "examples/trust-any.conf");

  RequestMessage request;
  RequestCapabilities capabilities;
  capabilities.setField("DIReservationSelectionV1", "required");
  request.setRequestCapabilities(capabilities);
  ReservationLease lease;
  lease.setField("reservationId", "reservation-r1-provider");
  lease.setField("providerBootEpoch", "boot-1");
  lease.setField("expiresAtMs", "9999999999999");
  provider.addPendingR1RequestForTokenTest(requester, service, requestId,
                                           request, "provider-token", lease);

  auto makeDecision = [&] (const std::string& value,
                           const std::string& token) {
    SelectionDecision decision;
    decision.setField("decision", value);
    decision.setField("requester", requester.toUri());
    decision.setField("requestId", requestId.toUri());
    decision.setField("attempt", "1");
    decision.setField("targetProvider", providerName.toUri());
    decision.setField("reservationId", lease.getField("reservationId"));
    decision.setField("reservationDigest", lease.computeDigest());
    decision.setField("providerBootEpoch", "boot-1");
    ServiceSelectionMessage selection;
    selection.setSelectionDecision(decision);
    selection.setProviderToken(token);
    return selection;
  };
  auto deliver = [&] (ServiceSelectionMessage selection) {
    const auto wire = selection.WireEncode();
    provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
      requester, providerName, service, requestId,
      ndn::Buffer(wire.data(), wire.size()));
    return computeSelectionDigest(selection);
  };

  deliver(makeDecision("NOT_SELECTED", "wrong-token"));
  BOOST_CHECK(provider.hasPendingRequestForTokenTest(requester, service, requestId));
  BOOST_CHECK(!provider.hasAcceptedR1DecisionForTest("reservation-r1-provider"));

  auto accepted = makeDecision("NOT_SELECTED", "provider-token");
  const auto acceptedDigest = deliver(accepted);
  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(requester, service, requestId));
  BOOST_CHECK(provider.hasAcceptedR1DecisionForTest("reservation-r1-provider"));
  BOOST_CHECK(!provider.getDecisionReceiptForTest(acceptedDigest).empty());

  // The exact same authenticated bytes are idempotent after pending state is gone.
  deliver(accepted);
  BOOST_CHECK(provider.hasAcceptedR1DecisionForTest("reservation-r1-provider"));

  // A later conflicting value cannot replace or execute the first decision.
  deliver(makeDecision("SELECTED", "provider-token"));
  BOOST_CHECK(provider.hasAcceptedR1DecisionForTest("reservation-r1-provider"));
}

BOOST_AUTO_TEST_CASE(R1SelectedProviderDecryptsInputAndAssignmentBeforeCommitAndExecution)
{
  // Use the default PIB/TPM because ServiceProvider owns a default KeyChain;
  // both must address the same recipient private key for this integration test.
  ndn::security::KeyChain keyChain;
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/r1-private");
  const ndn::Name providerName("/test/provider/r1-private");
  const ndn::Name service("/Inference/Generic");
  const ndn::Name requestId("request-r1-private");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa-r1-private"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"), providerCert,
                                aa, "examples/trust-any.conf");
  std::string observedInput;
  provider.addService(service, ServiceProvider::RequestHandler(
    [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
         const RequestMessage& request) {
      const auto payload = request.getPayload();
      observedInput.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
      ResponseMessage response; response.setStatus(true); return response;
    }));

  const std::string plaintext = "private-selected-input";
  auto input = encryptSelectionGatedInput(
    requester, service, requestId,
    ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(plaintext.data()),
                             plaintext.size()));
  RequestMessage request;
  RequestCapabilities capabilities;
  capabilities.setField("DIReservationSelectionV1", "required");
  capabilities.setField("SelectionGatedInputV1", "required");
  request.setRequestCapabilities(capabilities);
  request.setEncryptedRequestInput(input.first);
  ReservationLease lease;
  lease.setField("reservationId", "reservation-r1-private");
  lease.setField("providerBootEpoch", "boot-private");
  lease.setField("attempt", "1");
  lease.setField("expiresAtMs", "9999999999999");
  provider.addPendingR1RequestForTokenTest(
    requester, service, requestId, request, "provider-token", lease);

  DeploymentPlan plan;
  plan.setField("requesterIdentity", requester.toUri());
  plan.setField("requestId", requestId.toUri());
  plan.setField("attempt", "1");
  plan.setField("member.0.provider", providerName.toUri());
  plan.setField("member.0.role", "primary");
  plan.setField("memberCount", "1");
  SelectionDecision decision;
  decision.setField("decision", "SELECTED");
  decision.setField("requester", requester.toUri());
  decision.setField("requestId", requestId.toUri());
  decision.setField("attempt", "1");
  decision.setField("targetProvider", providerName.toUri());
  decision.setField("reservationId", lease.getField("reservationId"));
  decision.setField("reservationDigest", lease.computeDigest());
  decision.setField("providerBootEpoch", "boot-1");
  decision.setField("providerBootEpoch", "boot-private");
  decision.setField("globalPlanDigest", plan.computeDigest());

  const auto offer = makeSelectionInputKeyOffer(providerCert, "boot-private");
  SelectionInputKeyGrant grant;
  grant.setField("recipient", providerName.toUri());
  grant.setField("recipientCertName", offer.getField("recipientCertName"));
  grant.setField("recipientCertDigest", offer.getField("recipientCertDigest"));
  grant.setField("wrappedInputKey", selectionGatedHex(wrapSelectionGatedInputKey(
    input.second, selectionGatedUnhex(offer.getField("recipientPublicKey")))));
  grant.setField("encryptedInputDigest", input.first.computeDigest());
  grant.setField("requestId", requestId.toUri());
  grant.setField("attempt", "1");
  grant.setField("reservationId", lease.getField("reservationId"));
  const std::string assignmentText = "role=primary;privateFragment=only-this-provider;";
  const auto assignmentAad = recipientAssignmentAssociatedData(
    requester, providerName, service, requestId, lease.getField("reservationId"),
    plan.computeDigest());
  const auto assignment = encryptRecipientAssignment(
    ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(assignmentText.data()),
                             assignmentText.size()),
    providerCert.getPublicKey(), providerName, providerCert.getName(), assignmentAad);

  bool committed = false;
  unsigned terminalReleaseCount = 0;
  std::string terminalReservation;
  std::string terminalCause;
  provider.setR1SelectionDecisionHandler(service,
    [&] (const SelectionDecision& accepted) {
      committed = true;
      SelectionDecisionReceipt receipt;
      receipt.setField("decisionDigest", accepted.computeDigest());
      receipt.setField("reservationId", accepted.getField("reservationId"));
      return receipt;
    });
  provider.setR1ReservationTerminalHandler(service,
    [&] (const std::string& reservationId, const std::string& cause) {
      ++terminalReleaseCount;
      terminalReservation = reservationId;
      terminalCause = cause;
    });
  ServiceSelectionMessage selection;
  selection.setRequestIDs({requestId.toUri()});
  selection.setProviderToken("provider-token");
  selection.setSelectionDecision(decision);
  selection.setDeploymentPlan(plan);
  selection.setSelectionInputKeyGrant(grant);
  selection.setRecipientEncryptedAssignment(assignment);
  const auto wire = selection.WireEncode();
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requester, providerName, service, requestId,
    ndn::Buffer(wire.data(), wire.size()));
  const auto selectionStatus = provider.getSelectionExecutionStatus(
    computeSelectionDigest(selection));
  BOOST_TEST_MESSAGE("R1 selected status=" <<
                     (selectionStatus ? selectionStatus->message : "missing"));
  BOOST_CHECK(committed);
  BOOST_CHECK_EQUAL(observedInput, plaintext);
  BOOST_CHECK_EQUAL(terminalReleaseCount, 1);
  BOOST_CHECK_EQUAL(terminalReservation, lease.getField("reservationId"));
  BOOST_CHECK_EQUAL(terminalCause, "LOCAL_COMPLETE");
}

BOOST_AUTO_TEST_CASE(R1DecisionTombstoneExpiresAtReservationLeaseBoundary)
{
  ndn::security::KeyChain keyChain("pib-memory:r1-tombstone",
                                   "tpm-memory:r1-tombstone");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requester("/test/user/r1-tombstone");
  const ndn::Name providerName("/test/provider/r1-tombstone");
  const ndn::Name service("/Inference/Generic");
  const ndn::Name requestId("request-r1-tombstone");
  auto cert = makeRsaIdentity(keyChain, providerName);
  auto aa = makeRsaIdentity(keyChain, ndn::Name("/test/aa-r1-tombstone"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"), cert, aa,
                                "examples/trust-any.conf");

  RequestMessage request;
  RequestCapabilities capabilities;
  capabilities.setField("DIReservationSelectionV1", "required");
  request.setRequestCapabilities(capabilities);
  ReservationLease lease;
  lease.setField("reservationId", "reservation-r1-tombstone");
  lease.setField("providerBootEpoch", "boot-1");
  const auto expiresAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count() + 250;
  lease.setField("expiresAtMs", std::to_string(expiresAtMs));
  provider.addPendingR1RequestForTokenTest(
    requester, service, requestId, request, "provider-token", lease);

  SelectionDecision decision;
  decision.setField("decision", "NOT_SELECTED");
  decision.setField("requester", requester.toUri());
  decision.setField("requestId", requestId.toUri());
  decision.setField("attempt", "1");
  decision.setField("targetProvider", providerName.toUri());
  decision.setField("reservationId", lease.getField("reservationId"));
  decision.setField("reservationDigest", lease.computeDigest());
  decision.setField("providerBootEpoch", "boot-1");
  ServiceSelectionMessage selection;
  selection.setSelectionDecision(decision);
  selection.setProviderToken("provider-token");
  const auto wire = selection.WireEncode();
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requester, providerName, service, requestId,
    ndn::Buffer(wire.data(), wire.size()));
  BOOST_CHECK(provider.hasAcceptedR1DecisionForTest(
    lease.getField("reservationId")));
  face.processEvents(ndn::time::milliseconds(350));
  BOOST_CHECK(!provider.hasAcceptedR1DecisionForTest(
    lease.getField("reservationId")));
}

BOOST_AUTO_TEST_CASE(LateAckAfterAckTimeoutSelectsProviderBeforeRequestTimeout)
{
  ndn::security::KeyChain keyChain("pib-memory:late-ack-selects",
                                   "tpm-memory:late-ack-selects");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-late-ack-selects"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerName});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(20));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingSelectsFirstAckBeforeAckTimeout)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-before-ack-timeout",
                                   "tpm-memory:first-responding-before-ack-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-before-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerA, providerB});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 100,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    500,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    firstAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  pumpFace(face, ndn::time::milliseconds(150));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingSelectsFirstAckAfterNominalAckTimeout)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-late-ack",
                                   "tpm-memory:first-responding-late-ack");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-late-ack"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerName});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    100,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(20));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(!user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(RequestTimeoutExpressesSelectionStatusDiagnosticQuery)
{
  ndn::security::KeyChain keyChain("pib-memory:timeout-status-diag",
                                   "tpm-memory:timeout-status-diag");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/slow");
  const ndn::Name serviceName("/TextToImage/Generate");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-timeout-status-diag"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerName});
  user.setPendingCallTimeoutGrace(ndn::time::milliseconds(0));

  user.setRequestPublisher(
    [&] (const ndn::Name& requestId,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage& requestMessage,
         size_t) {
      auto ack = makeSuccessAckForRequest(requestMessage,
                                          "provider-token-timeout-status-diag");
      BOOST_CHECK(user.handleRequestAckByName(
        makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
        ack));
      BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    30,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {
      BOOST_FAIL("timeout diagnostic test should not receive a response");
    }));

  pumpFace(face, ndn::time::milliseconds(80));
  BOOST_CHECK(timeoutCalled);
  BOOST_CHECK(!user.hasPendingCall(requestId));

  bool sawStatusQuery = false;
  for (const auto& interest : face.sentInterests) {
    auto parsed = parseSelectionStatusQueryName(interest.getName());
    if (parsed &&
        parsed->providerName.equals(providerName) &&
        parsed->serviceName.equals(serviceName) &&
        !parsed->selectionDigest.empty()) {
      sawStatusQuery = true;
      break;
    }
  }
  BOOST_CHECK(sawStatusQuery);
}

BOOST_AUTO_TEST_CASE(FirstRespondingIgnoresAckTimeoutCompletely)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-ignores-ack-timeout",
                                   "tpm-memory:first-responding-ignores-ack-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-ignore-ack-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerName});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    200,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(!user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK(!timeoutCalled);

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerName);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(FirstRespondingLateAckAfterRequestTimeoutIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-late-after-timeout",
                                   "tpm-memory:first-responding-late-after-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-late-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerName});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    20,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(timeoutCalled);
  BOOST_CHECK(!user.hasPendingCall(requestId));

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK(!user.hasPendingCall(requestId));
}

BOOST_AUTO_TEST_CASE(FirstRespondingAckAfterProviderSelectedIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-ack-after-selected",
                                   "tpm-memory:first-responding-ack-after-selected");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-ack-after-selected"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerA, providerB});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 100,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    500,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("selected request should not time out in this unit test");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    firstAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  auto secondAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    secondAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
}

BOOST_AUTO_TEST_CASE(FirstRespondingV2AckCallbackDoesNotFallThroughToLegacySelection)
{
  ndn::security::KeyChain keyChain("pib-memory:first-responding-v2-no-legacy-fallback",
                                   "tpm-memory:first-responding-v2-no-legacy-fallback");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-first-v2-no-legacy"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerA, providerB});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 100,
    ServiceUser::AckSelectionStrategy::FirstRespondingSelection,
    500,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {}),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  BOOST_CHECK(user.hasLegacyStrategyState(requestId));

  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  auto firstAckBlock = firstAck.WireEncode();
  ndn::Buffer firstAckBuffer(firstAckBlock.data(), firstAckBlock.size());
  user.OnRequestAckDecryptionSuccessCallback(providerA,
                                             serviceName,
                                             requestId,
                                             firstAckBuffer);
  pumpFace(face, 50_ms);
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK(user.hasLegacyStrategyState(requestId));

  auto secondAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  auto secondAckBlock = secondAck.WireEncode();
  ndn::Buffer secondAckBuffer(secondAckBlock.data(), secondAckBlock.size());
  user.OnRequestAckDecryptionSuccessCallback(providerB,
                                             serviceName,
                                             requestId,
                                             secondAckBuffer);
  pumpFace(face, 50_ms);
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
  BOOST_CHECK(user.hasLegacyStrategyState(requestId));
}

BOOST_AUTO_TEST_CASE(LateAckAfterRequestTimeoutIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:late-ack-after-timeout",
                                   "tpm-memory:late-ack-after-timeout");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-late-ack-timeout"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerName});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerName}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    20,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(timeoutCalled);
  BOOST_CHECK(!user.hasPendingCall(requestId));

  auto ack = makeSuccessAckForRequest(publishedRequest);
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerName, requesterName, serviceName, requestId),
    ack));
  BOOST_CHECK(!user.hasPendingCall(requestId));
}

BOOST_AUTO_TEST_CASE(AckAfterProviderSelectedIsIgnored)
{
  ndn::security::KeyChain keyChain("pib-memory:ack-after-selected",
                                   "tpm-memory:ack-after-selected");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-ack-after-selected"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerA, providerB});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 5,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("selected request should not time out in this unit test");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  pumpFace(face, ndn::time::milliseconds(20));
  auto firstAck = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    firstAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  auto secondAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(!user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    secondAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerA);
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);
}

BOOST_AUTO_TEST_CASE(RandomSelectionMultipleAcksWithinWindowSelectsOneCandidate)
{
  ndn::security::KeyChain keyChain("pib-memory:random-selection-normal",
                                   "tpm-memory:random-selection-normal");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-random-selection-normal"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerA, providerB});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 30,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([] (const ndn::Name&) {
      BOOST_FAIL("normal ACK selection should not time out in this unit test");
    }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto ackA = makeSuccessAckForRequest(publishedRequest, "provider-token-A");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    ackA));
  auto ackB = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    ackB));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());

  pumpFace(face, ndn::time::milliseconds(50));
  const auto selectedProvider = user.getSelectedProvider(requestId);
  BOOST_CHECK(selectedProvider.equals(providerA) || selectedProvider.equals(providerB));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 2);
}

BOOST_AUTO_TEST_CASE(RandomSelectionIgnoresFailedAcksAndKeepsPendingForLateSuccess)
{
  ndn::security::KeyChain keyChain("pib-memory:random-selection-valid-only",
                                   "tpm-memory:random-selection-valid-only");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerA("/test/provider/A");
  const ndn::Name providerB("/test/provider/B");
  const ndn::Name serviceName("/HELLO");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-random-selection-valid-only"));
  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  installUserPermissions(user, requesterName, serviceName, {providerA, providerB});

  RequestMessage publishedRequest;
  user.setRequestPublisher(
    [&] (const ndn::Name&, const ndn::Name&, const std::vector<ndn::Name>&,
         const ndn::Name&, const RequestMessage& requestMessage, size_t) {
      publishedRequest = requestMessage;
    });

  bool timeoutCalled = false;
  const auto requestId = user.RequestService(
    {providerA, providerB}, serviceName, RequestMessage(), 30,
    ServiceUser::AckSelectionStrategy::RandomSelection,
    100,
    ServiceUser::TimeoutHandler([&] (const ndn::Name&) { timeoutCalled = true; }),
    ServiceUser::ResponseHandler([] (const ResponseMessage&) {}));

  auto failedAck = makeSuccessAckForRequest(publishedRequest, "");
  failedAck.setStatus(false);
  failedAck.setMessage("busy");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerA, requesterName, serviceName, requestId),
    failedAck));

  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK(user.hasPendingCall(requestId));
  BOOST_CHECK(user.isAckWindowExpired(requestId));
  BOOST_CHECK(user.getSelectedProvider(requestId).empty());
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 1);

  auto successAck = makeSuccessAckForRequest(publishedRequest, "provider-token-B");
  BOOST_CHECK(user.handleRequestAckByName(
    makeRequestAckNameV2(providerB, requesterName, serviceName, requestId),
    successAck));
  BOOST_CHECK_EQUAL(user.getSelectedProvider(requestId), providerB);
  BOOST_CHECK(!timeoutCalled);
}

BOOST_AUTO_TEST_CASE(RandomSelectionDistributionSanity)
{
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/random-distribution");
  std::vector<AckSelectionCandidate> candidates;
  for (const auto& provider : {ndn::Name("/test/provider/A"),
                               ndn::Name("/test/provider/B"),
                               ndn::Name("/test/provider/C")}) {
    AckSelectionCandidate candidate;
    candidate.providerName = provider;
    candidate.serviceName = serviceName;
    candidate.requestId = requestId;
    candidate.ack = makeSuccessAck();
    candidate.ack.setUserToken("user-token");
    candidate.ack.setProviderToken("provider-token");
    candidates.push_back(candidate);
  }

  std::map<std::string, int> selectedCounts;
  for (int i = 0; i < 200; ++i) {
    const auto selected = ServiceUser::selectRandomAck(candidates);
    BOOST_REQUIRE_EQUAL(selected.size(), 1);
    ++selectedCounts[selected.front().providerName.toUri()];
  }

  BOOST_CHECK_GE(selectedCounts.size(), 2);
}


BOOST_AUTO_TEST_CASE(DeploymentSelectionPreparesButCannotExecuteBeforeActivation)
{
  ndn::security::KeyChain keyChain("pib-memory:deployment-gate", "tpm-memory:deployment-gate");
  ndn::DummyClientFace::Options faceOptions;
  ndn::DummyClientFace face(keyChain, faceOptions);
  const ndn::Name requesterName("/test/user/deployer");
  const ndn::Name providerName("/test/provider/worker");
  const ndn::Name serviceName("/Inference/Generic");
  const ndn::Name requestId("/deployment-gate");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-deployment-gate"));
  LocalServiceProvider provider(face, ndn::Name("/test/group"), providerCert, aaCert,
                                "examples/trust-any.conf");
  provider.setUseTokens(false);

  int prepareCalls = 0;
  int executeCalls = 0;
  int readyPublications = 0;
  provider.addService(serviceName, ServiceProvider::RequestHandler(
    [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
         const RequestMessage&) {
      ++executeCalls;
      ResponseMessage response;
      response.setStatus(true);
      return response;
    }));
  provider.setDeploymentPrepareHandler(
    [&] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
         const RequestMessage&, const DeploymentPlan&, const std::string&) {
      ++prepareCalls;
      ProviderReadyMessage ready;
      ready.setField("artifactDigest", "sha256:test");
      ready.setField("deploymentInstanceId", "instance-1");
      ready.setField("operationId", "prepare-1");
      return ready;
    });
  provider.setProviderReadyPublisher(
    [&] (const ndn::Name&, const ProviderReadyMessage&) { ++readyPublications; });

  DeploymentIntent intent;
  intent.setField("artifactDigest", "sha256:test");
  RequestMessage request;
  request.setDeploymentIntent(intent);
  provider.addPendingRequestForTokenTest(requesterName, serviceName, requestId, request, "");

  DeploymentPlan plan;
  plan.setField("requestId", requestId.toUri());
  plan.setField("attempt", "1");
  plan.setField("requesterIdentity", requesterName.toUri());
  plan.setField("intentDigest", intent.computeDigest());
  plan.setField("member.0.provider", providerName.toUri());
  plan.setField("member.0.role", "worker");
  ServiceSelectionMessage selection;
  selection.setRequestIDs({requestId.toUri()});
  selection.setDeploymentPlan(plan);
  auto selectionBlock = selection.WireEncode();
  ndn::Buffer selectionBuffer(selectionBlock.data(), selectionBlock.size());
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName, providerName, serviceName, requestId, selectionBuffer);

  BOOST_CHECK_EQUAL(prepareCalls, 1);
  BOOST_CHECK_EQUAL(readyPublications, 1);
  BOOST_CHECK_EQUAL(executeCalls, 0);

  ExecutionActivateMessage activation;
  activation.setField("requestId", requestId.toUri());
  activation.setField("selectionDigest", computeSelectionDigest(selection));
  activation.setField("deploymentPlanDigest", plan.computeDigest());
  activation.setField("readySetDigest", "ready-set-digest");
  activation.setField("memberSetDigest", "member-set-digest");
  activation.setField("requesterIdentity", requesterName.toUri());
  activation.setField("activationSequence", "1");
  activation.setField("expiresAtUs", std::to_string(
    std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count() + 1000000));
  std::string rejection;
  BOOST_CHECK(provider.acceptExecutionActivate(activation, &rejection));
  pumpFace(face, ndn::time::milliseconds(50));
  BOOST_CHECK_EQUAL(executeCalls, 1);
  BOOST_CHECK(provider.acceptExecutionActivate(activation, &rejection));
  BOOST_CHECK_EQUAL(executeCalls, 1);

  auto conflicting = activation;
  conflicting.setField("activationSequence", "2");
  BOOST_CHECK(!provider.acceptExecutionActivate(conflicting, &rejection));
  BOOST_CHECK_EQUAL(executeCalls, 1);
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
