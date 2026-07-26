#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(DeploymentControl)

BOOST_AUTO_TEST_CASE(VersionedMessagesRoundTripAndDigestCanonically)
{
  DeploymentIntent intent;
  intent.setField("artifactDigest", "sha256:abc");
  intent.setField("requestId", "request-1");
  const auto digest = intent.computeDigest();

  DeploymentIntent decoded;
  BOOST_REQUIRE(decoded.WireDecode(intent.WireEncode()));
  BOOST_CHECK_EQUAL(decoded.getVersion(), DeploymentControlMessage::VERSION);
  BOOST_CHECK_EQUAL(decoded.getField("artifactDigest"), "sha256:abc");
  BOOST_CHECK_EQUAL(decoded.computeDigest(), digest);

  ProviderCapabilityOffer offer;
  offer.setField("providerBootEpoch", "7");
  offer.setField("supportedRuntime", "onnx");
  ProviderCapabilityOffer decodedOffer;
  BOOST_REQUIRE(decodedOffer.WireDecode(offer.WireEncode()));
  BOOST_CHECK_EQUAL(decodedOffer.getField("supportedRuntime"), "onnx");

  DeploymentPlan plan;
  plan.setField("membership", "/p=a,/q=b");
  ProviderReadyMessage ready;
  ready.setField("deploymentPlanDigest", plan.computeDigest());
  ReadyAcknowledgement acknowledgement;
  acknowledgement.setField("accepted", "true");
  ExecutionActivateMessage activation;
  activation.setField("readySetDigest", ready.computeDigest());
  BOOST_CHECK(!plan.computeDigest().empty());
  BOOST_CHECK(!acknowledgement.computeDigest().empty());
  BOOST_CHECK(!activation.computeDigest().empty());
}

BOOST_AUTO_TEST_CASE(UnknownVersionAndBoundsFailClosed)
{
  DeploymentIntent unknown;
  unknown.setVersion(2);
  BOOST_CHECK_THROW(unknown.WireEncode(), std::invalid_argument);

  DeploymentIntent bounded;
  BOOST_CHECK_THROW(bounded.setField("", "value"), std::invalid_argument);
  BOOST_CHECK_THROW(bounded.setField(std::string(65, 'k'), "value"), std::invalid_argument);
  BOOST_CHECK_THROW(bounded.setField("value", std::string(4097, 'x')),
                    std::invalid_argument);
  for (size_t i = 0; i < DeploymentControlMessage::MAX_FIELDS; ++i) {
    bounded.setField("field" + std::to_string(i), "x");
  }
  BOOST_CHECK_THROW(bounded.setField("overflow", "x"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(IntentOfferAndPlanRemainExplicitAcrossExistingMessages)
{
  DeploymentIntent intent;
  intent.setField("artifactDigest", "sha256:artifact");
  intent.setField("requiredRoles", "prefill,decode");
  RequestMessage request;
  request.setDeploymentIntent(intent);
  RequestMessage decodedRequest;
  BOOST_REQUIRE(decodedRequest.WireDecode(request.WireEncode()));
  BOOST_REQUIRE(decodedRequest.hasDeploymentIntent());
  BOOST_CHECK_EQUAL(decodedRequest.getDeploymentIntent().computeDigest(), intent.computeDigest());

  ProviderCapabilityOffer offer;
  offer.setField("providerIdentity", "/provider/a");
  offer.setField("providerBootEpoch", "boot-7");
  RequestAckMessage ack;
  ack.setProviderCapabilityOffer(offer);
  RequestAckMessage decodedAck;
  BOOST_REQUIRE(decodedAck.WireDecode(ack.WireEncode()));
  BOOST_REQUIRE(decodedAck.hasProviderCapabilityOffer());
  BOOST_CHECK_EQUAL(decodedAck.getProviderCapabilityOffer().getField("providerIdentity"),
                    "/provider/a");

  DeploymentPlan plan;
  plan.setField("requestId", "request-1");
  plan.setField("membership", "prefill=/provider/a;decode=/provider/b");
  ServiceSelectionMessage selection;
  selection.setDeploymentPlan(plan);
  ServiceSelectionMessage decodedSelection;
  BOOST_REQUIRE(decodedSelection.WireDecode(selection.WireEncode()));
  BOOST_REQUIRE(decodedSelection.hasDeploymentPlan());
  BOOST_CHECK_EQUAL(decodedSelection.getDeploymentPlan().computeDigest(), plan.computeDigest());

  RequestMessage legacyRequest;
  RequestMessage decodedLegacy;
  BOOST_REQUIRE(decodedLegacy.WireDecode(legacyRequest.WireEncode()));
  BOOST_CHECK(!decodedLegacy.hasDeploymentIntent());
}

BOOST_AUTO_TEST_CASE(R1CapabilitiesAndIndependentInputContractsRoundTrip)
{
  RequestCapabilities capabilities;
  capabilities.setField("DIReservationSelectionV1", "required");
  capabilities.setField("SelectionGatedInputV1", "required");
  EncryptedRequestInput input;
  input.setField("algorithm", "AES-256-GCM");
  input.setField("ciphertext", "opaque-input");
  input.setField("keyId", "input-key-1");
  RequestMessage request;
  request.setRequestCapabilities(capabilities);
  request.setEncryptedRequestInput(input);
  RequestMessage decodedRequest;
  BOOST_REQUIRE(decodedRequest.WireDecode(request.WireEncode()));
  BOOST_REQUIRE(decodedRequest.hasRequestCapabilities());
  BOOST_REQUIRE(decodedRequest.hasEncryptedRequestInput());
  BOOST_CHECK(!decodedRequest.hasDeploymentIntent());

  SelectionInputKeyOffer keyOffer;
  keyOffer.setField("provider", "/provider/a");
  keyOffer.setField("certificateDigest", "sha256:cert");
  ReservationLease lease;
  lease.setField("reservationId", "reservation-1");
  lease.setField("expiresAtMs", "1000");
  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setSelectionInputKeyOffer(keyOffer);
  ack.setReservationLease(lease);
  RequestAckMessage decodedAck;
  BOOST_REQUIRE(decodedAck.WireDecode(ack.WireEncode()));
  BOOST_REQUIRE(decodedAck.hasSelectionInputKeyOffer());
  BOOST_REQUIRE(decodedAck.hasReservationLease());

  SelectionDecision decision;
  decision.setField("decision", "SELECTED");
  decision.setField("reservationId", "reservation-1");
  SelectionInputKeyGrant grant;
  grant.setField("recipient", "/provider/a");
  grant.setField("wrappedInputKey", "opaque-key");
  ServiceSelectionMessage selection;
  selection.setSelectionDecision(decision);
  selection.setSelectionInputKeyGrant(grant);
  ServiceSelectionMessage decodedSelection;
  BOOST_REQUIRE(decodedSelection.WireDecode(selection.WireEncode()));
  BOOST_REQUIRE(decodedSelection.hasSelectionDecision());
  BOOST_REQUIRE(decodedSelection.hasSelectionInputKeyGrant());
  BOOST_CHECK(!decodedSelection.hasDeploymentPlan());
}

BOOST_AUTO_TEST_CASE(R1ControlTypesAreDistinctCanonicalAndBounded)
{
  SelectionDecisionReceipt receipt;
  receipt.setField("decisionDigest", "sha256:decision");
  RecipientEncryptedAssignment assignment;
  assignment.setField("ciphertext", "opaque-assignment");
  StageInputEvidence stage;
  stage.setField("producerRole", "prefill");
  StageAbort abort;
  abort.setField("reason", "deadline");
  SelectionDecisionTombstone tombstone;
  tombstone.setField("retainUntilMs", "2000");
  BOOST_CHECK_NE(receipt.WireEncode().type(), assignment.WireEncode().type());
  BOOST_CHECK_NE(stage.WireEncode().type(), abort.WireEncode().type());
  BOOST_CHECK(!tombstone.computeDigest().empty());
  SelectionInputKeyGrant unknown;
  unknown.setVersion(99);
  BOOST_CHECK_THROW(unknown.WireEncode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OpaqueControlNamesAreExactAndVersioned)
{
  const auto handle = makeOpaqueControlHandle();
  BOOST_CHECK(isValidOpaqueControlHandle(handle));
  BOOST_CHECK(!isValidOpaqueControlHandle("request-123"));

  const ndn::Name provider("/provider/a");
  const ndn::Name requester("/requester/u");
  const auto statusName = makeSecureSelectionStatusName(provider, 1, handle);
  const auto readyName = makeProviderReadyName(requester, 1, handle);
  const auto activateName = makeExecutionActivateName(provider, 1, handle);
  BOOST_REQUIRE(parseSecureSelectionStatusName(statusName));
  BOOST_REQUIRE(parseProviderReadyName(readyName));
  BOOST_REQUIRE(parseExecutionActivateName(activateName));
  BOOST_CHECK(!parseSecureSelectionStatusName(ndn::Name(statusName).append("extra")));
  BOOST_CHECK_THROW(makeSecureSelectionStatusName(provider, 2, handle),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(R1SelectionDecisionNameTargetsProviderAndAttempt)
{
  const auto name = makeServiceSelectionDecisionNameV2(
    ndn::Name("/requester/u"), ndn::Name("/provider/a"),
    ndn::Name("/service/model"), ndn::Name("request-1"), 3);
  const auto parsed = parseServiceSelectionDecisionNameV2(name);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(parsed->requesterName, ndn::Name("/requester/u"));
  BOOST_CHECK_EQUAL(parsed->providerName, ndn::Name("/provider/a"));
  BOOST_CHECK_EQUAL(parsed->serviceName, ndn::Name("/service/model"));
  BOOST_CHECK_EQUAL(parsed->requestId, ndn::Name("request-1"));
  BOOST_CHECK_EQUAL(parsed->attempt, 3);
  BOOST_CHECK(!parseServiceSelectionDecisionNameV2(name.getPrefix(-1)));
  BOOST_CHECK_THROW(makeServiceSelectionDecisionNameV2(
                      ndn::Name("/requester/u"), ndn::Name("/provider/a"),
                      ndn::Name("/service/model"), ndn::Name("request-1"), 0),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RequesterScopedStatusAeadBindsAssociatedData)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const ndn::Name requester("/requester/u");
  const ndn::Name provider("/provider/a");
  const auto handle = makeOpaqueControlHandle();
  auto key = crypto.getOrCreateStatusSendKey(requester, handle, "/requester/u/KEY/1", counters);
  const auto name = makeSecureSelectionStatusName(provider, 1, handle);
  const auto aad = secureStatusAssociatedData(name, 1, handle, requester, provider,
                                               3, key.keyId, key.epochId);
  ndn::Buffer plaintext(reinterpret_cast<const uint8_t*>("state=WARMING"), 13);
  const auto encrypted = hybridAesGcmEncrypt(
    key.key, ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
    ndn::span<const uint8_t>(aad.data(), aad.size()));
  HybridMessageEnvelope envelope;
  envelope.setMessageType("SECURE-SELECTION-STATUS");
  envelope.setKeyId(key.keyId);
  envelope.setEpochId(key.epochId);
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);
  ndn::Buffer decoded;
  BOOST_REQUIRE(hybridAesGcmDecrypt(
    key.key, envelope, ndn::span<const uint8_t>(aad.data(), aad.size()), decoded));
  auto wrongAad = secureStatusAssociatedData(name, 1, handle, requester, provider,
                                              4, key.keyId, key.epochId);
  BOOST_CHECK(!hybridAesGcmDecrypt(
    key.key, envelope, ndn::span<const uint8_t>(wrongAad.data(), wrongAad.size()), decoded));
  BOOST_CHECK_NE(key.keyId.find("SECURE-STATUS"), std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
