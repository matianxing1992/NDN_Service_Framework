#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <ndn-cxx/security/signing-helpers.hpp>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(GenericDynamicApi)
BOOST_AUTO_TEST_SUITE(CryptoAndAuthorization)

BOOST_AUTO_TEST_CASE(MessageTokenFieldsRoundTrip)
{
  RequestMessage request;
  request.setUserToken("user-token");
  request.setPolicyEpoch(42);
  RequestMessage decodedRequest;
  BOOST_CHECK(decodedRequest.WireDecode(request.WireEncode()));
  BOOST_CHECK_EQUAL(decodedRequest.getUserToken(), "user-token");
  BOOST_CHECK_EQUAL(decodedRequest.getPolicyEpoch(), 42);

  RequestAckMessage ack;
  ack.setUserToken("user-token");
  ack.setProviderToken("provider-token");
  ack.setPolicyEpoch(42);
  RequestAckMessage decodedAck;
  BOOST_CHECK(decodedAck.WireDecode(ack.WireEncode()));
  BOOST_CHECK_EQUAL(decodedAck.getUserToken(), "user-token");
  BOOST_CHECK_EQUAL(decodedAck.getProviderToken(), "provider-token");
  BOOST_CHECK_EQUAL(decodedAck.getPolicyEpoch(), 42);

  ServiceSelectionMessage selection;
  selection.setProviderToken("provider-token");
  selection.setPolicyEpoch(42);
  ServiceSelectionMessage decodedSelection;
  BOOST_CHECK(decodedSelection.WireDecode(selection.WireEncode()));
  BOOST_CHECK_EQUAL(decodedSelection.getProviderToken(), "provider-token");
  BOOST_CHECK_EQUAL(decodedSelection.getPolicyEpoch(), 42);

  ResponseMessage response;
  response.setUserToken("user-token");
  response.setPolicyEpoch(42);
  ResponseMessage decodedResponse;
  BOOST_CHECK(decodedResponse.WireDecode(response.WireEncode()));
  BOOST_CHECK_EQUAL(decodedResponse.getUserToken(), "user-token");
  BOOST_CHECK_EQUAL(decodedResponse.getPolicyEpoch(), 42);

  PolicyManifest manifest;
  manifest.setPolicyEpoch(42);
  manifest.setValidFromMs(1234);
  manifest.setGracePeriodMs(5000);
  manifest.setRequiredKeyEpoch(43);
  PolicyManifest decodedManifest;
  BOOST_CHECK(decodedManifest.WireDecode(manifest.WireEncode()));
  BOOST_CHECK_EQUAL(decodedManifest.getPolicyEpoch(), 42);
  BOOST_CHECK_EQUAL(decodedManifest.getValidFromMs(), 1234);
  BOOST_CHECK_EQUAL(decodedManifest.getGracePeriodMs(), 5000);
  BOOST_CHECK_EQUAL(decodedManifest.getRequiredKeyEpoch(), 43);
}

BOOST_AUTO_TEST_CASE(MessagePayloadBlockRoundTrip)
{
  const std::vector<uint8_t> payload = {'n', 'd', 'n', 's', 'f', 0, 'b', 'l', 'k'};
  auto payloadBlock = ndn::makeBinaryBlock(tlv::PayloadType,
                                           payload.begin(),
                                           payload.end());
  payloadBlock.encode();

  RequestMessage request;
  request.setPayloadBlock(payloadBlock);
  BOOST_CHECK_EQUAL(request.getPayloadBlock().type(), tlv::PayloadType);
  BOOST_CHECK_EQUAL(request.getPayloadSize(), payload.size());
  RequestMessage decodedRequest;
  BOOST_REQUIRE(decodedRequest.WireDecode(request.WireEncode()));
  BOOST_CHECK_EQUAL(decodedRequest.getPayloadBlock().type(), tlv::PayloadType);
  const auto decodedRequestPayload = decodedRequest.getPayload();
  BOOST_CHECK_EQUAL_COLLECTIONS(decodedRequestPayload.begin(),
                                decodedRequestPayload.end(),
                                payload.begin(),
                                payload.end());

  RequestAckMessage ack;
  ack.setPayloadBlock(payloadBlock);
  RequestAckMessage decodedAck;
  BOOST_REQUIRE(decodedAck.WireDecode(ack.WireEncode()));
  BOOST_CHECK_EQUAL(decodedAck.getPayloadBlock().type(), tlv::PayloadType);
  const auto decodedAckPayload = decodedAck.getPayload();
  BOOST_CHECK_EQUAL_COLLECTIONS(decodedAckPayload.begin(),
                                decodedAckPayload.end(),
                                payload.begin(),
                                payload.end());

  ResponseMessage response;
  response.setPayloadBlock(payloadBlock);
  ResponseMessage decodedResponse;
  BOOST_REQUIRE(decodedResponse.WireDecode(response.WireEncode()));
  BOOST_CHECK_EQUAL(decodedResponse.getPayloadBlock().type(), tlv::PayloadType);
  const auto decodedResponsePayload = decodedResponse.getPayload();
  BOOST_CHECK_EQUAL_COLLECTIONS(decodedResponsePayload.begin(),
                                decodedResponsePayload.end(),
                                payload.begin(),
                                payload.end());
}

BOOST_AUTO_TEST_CASE(HybridMessageEnvelopeProtectsRequestPayloadAndUserToken)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const ndn::Name serviceName("/HELLO");
  const ndn::Name sender("/test/user/alice");
  auto key = crypto.getOrCreateSendKey(serviceName, sender, "/SERVICE/HELLO",
                                       "REQUEST", counters);

  RequestMessage request;
  request.setUserToken("user-token-secret");
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>("payload-secret"), 14);
  request.setPayload(payload, payload.size());
  const auto plaintext = request.WireEncode();
  const auto ad = hybridAssociatedData(
    ndn::Name("/test/user/alice/NDNSF/REQUEST/HELLO/bloom/rid"),
    "REQUEST", ndn::Name("/rid"), serviceName, sender, key.keyId, key.epochId);

  auto encrypted = hybridAesGcmEncrypt(
    key.key,
    ndn::span<const uint8_t>(&*plaintext.begin(), plaintext.size()),
    ndn::span<const uint8_t>(ad.data(), ad.size()));

  HybridMessageEnvelope envelope;
  envelope.setKeyId(key.keyId);
  envelope.setEpochId(key.epochId);
  envelope.setMessageType("REQUEST");
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);

  const auto envelopeWire = envelope.WireEncode();
  const std::string envelopeBytes(
    reinterpret_cast<const char*>(&*envelopeWire.begin()),
    envelopeWire.size());
  BOOST_CHECK_EQUAL(envelopeBytes.find("user-token-secret"), std::string::npos);
  BOOST_CHECK_EQUAL(envelopeBytes.find("payload-secret"), std::string::npos);

  ndn::Buffer decrypted;
  BOOST_REQUIRE(hybridAesGcmDecrypt(key.key, envelope,
                                    ndn::span<const uint8_t>(ad.data(), ad.size()),
                                    decrypted));
  RequestMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(ndn::Block(decrypted)));
  BOOST_CHECK_EQUAL(decoded.getUserToken(), "user-token-secret");
  BOOST_CHECK_EQUAL(decoded.getPayloadSize(), 14);
}

BOOST_AUTO_TEST_CASE(HybridMessageEnvelopeProtectsAckProviderTokenAndDetectsTamper)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const ndn::Name serviceName("/HELLO");
  const ndn::Name sender("/test/provider/a");
  auto key = crypto.getOrCreateSendKey(serviceName, sender, "/PERMISSION/HELLO",
                                       "ACK", counters);

  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setMessage("ready");
  ack.setUserToken("user-token-secret");
  ack.setProviderToken("provider-token-secret");
  ndn::Buffer payload(reinterpret_cast<const uint8_t*>("queue=0"), 7);
  ack.setPayload(payload, payload.size());
  const auto plaintext = ack.WireEncode();
  const auto ad = hybridAssociatedData(
    makeRequestAckNameV2(ndn::Name("/test/provider/a"),
                         ndn::Name("/test/user/alice"),
                         serviceName,
                         ndn::Name("/rid")),
    "ACK", ndn::Name("/rid"), serviceName, sender, key.keyId, key.epochId);

  auto encrypted = hybridAesGcmEncrypt(
    key.key,
    ndn::span<const uint8_t>(&*plaintext.begin(), plaintext.size()),
    ndn::span<const uint8_t>(ad.data(), ad.size()));

  HybridMessageEnvelope envelope;
  envelope.setKeyId(key.keyId);
  envelope.setEpochId(key.epochId);
  envelope.setMessageType("ACK");
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);

  const auto envelopeWire = envelope.WireEncode();
  const std::string envelopeBytes(
    reinterpret_cast<const char*>(&*envelopeWire.begin()),
    envelopeWire.size());
  BOOST_CHECK_EQUAL(envelopeBytes.find("provider-token-secret"), std::string::npos);

  ndn::Buffer decrypted;
  BOOST_REQUIRE(hybridAesGcmDecrypt(key.key, envelope,
                                    ndn::span<const uint8_t>(ad.data(), ad.size()),
                                    decrypted));
  RequestAckMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(ndn::Block(decrypted)));
  BOOST_CHECK_EQUAL(decoded.getProviderToken(), "provider-token-secret");

  auto tamperedCiphertext = envelope.getCipherText();
  tamperedCiphertext[0] ^= 0x01;
  envelope.setCipherText(tamperedCiphertext);
  BOOST_CHECK(!hybridAesGcmDecrypt(key.key, envelope,
                                   ndn::span<const uint8_t>(ad.data(), ad.size()),
                                   decrypted));
}

BOOST_AUTO_TEST_CASE(HybridV2EnvelopeAndStandardMessagesStayWithinPiggybackLimit)
{
  constexpr size_t TARGET_WIRE_SIZE = 800;
  ndn::KeyChain keyChain("pib-memory:ndnsf-wire-size",
                         "tpm-memory:ndnsf-wire-size");
  const ndn::Name user("/example/hello/user");
  const ndn::Name provider("/example/hello/provider/A");
  const ndn::Name service("/HELLO");
  const ndn::Name requestId("/request-0000000000000001");
  auto rsaIdentity = keyChain.createIdentity(user, ndn::RsaKeyParams(2048));
  auto rsaCert = rsaIdentity.getDefaultKey().getDefaultCertificate();
  auto ecCert = keyChain.createKey(rsaIdentity, ndn::EcKeyParams())
                         .getDefaultCertificate();
  BOOST_CHECK_EQUAL(getCertificateKeyType(rsaCert), ndn::KeyType::RSA);
  BOOST_CHECK_EQUAL(getCertificateKeyType(ecCert), ndn::KeyType::EC);

  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;

  auto checkMessage = [&](const std::string& type,
                          const ndn::Name& messageName,
                          const ndn::Name& sender,
                          const std::string& attribute,
                          const ndn::Buffer& plaintext) {
    auto key = crypto.getOrCreateSendKey(service, sender, attribute, type, counters);
    const auto ad = hybridAssociatedData(messageName, type, requestId, service,
                                         sender, key.keyId, key.epochId);
    const auto encrypted = hybridAesGcmEncrypt(
      key.key, ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
      ndn::span<const uint8_t>(ad.data(), ad.size()));
    HybridMessageEnvelope envelope;
    envelope.setKeyId(key.keyId);
    envelope.setEpochId(key.epochId);
    envelope.setMessageType(type);
    envelope.setNonce(encrypted.nonce);
    envelope.setCipherText(encrypted.ciphertext);
    envelope.setAuthTag(encrypted.tag);
    BOOST_CHECK(!envelope.hasWrappedMessageKey());
    const auto wire = envelope.WireEncode();
    HybridMessageEnvelope decoded;
    BOOST_REQUIRE(decoded.WireDecode(wire));
    BOOST_CHECK_EQUAL(decoded.getVersion(), 2);
    BOOST_CHECK_EQUAL(decoded.getKeyId(), hybridCompactKeyId(key.keyId));
    BOOST_CHECK_EQUAL(decoded.getEpochId(), key.epochId);
    BOOST_CHECK_EQUAL(decoded.getMessageType(), type);
    ndn::Buffer recovered;
    BOOST_REQUIRE(hybridAesGcmDecrypt(
      key.key, decoded, ndn::span<const uint8_t>(ad.data(), ad.size()), recovered));
    BOOST_CHECK_EQUAL_COLLECTIONS(recovered.begin(), recovered.end(),
                                  plaintext.begin(), plaintext.end());

    ndn::Data data(messageName);
    data.setContent(wire);
    keyChain.sign(data, ndn::security::signingByCertificate(ecCert));
    BOOST_CHECK_LE(data.wireEncode().size(), TARGET_WIRE_SIZE);
  };

  RequestAckMessage ack;
  ack.setStatus(true);
  ack.setMessage("HELLO provider ready");
  ack.setUserToken("user-token-000001");
  ack.setProviderToken("provider-token-0001");
  const std::string ackPayload = "queue=0;gpu=idle;model=hello-v1";
  ndn::Buffer ackPayloadBuffer(reinterpret_cast<const uint8_t*>(ackPayload.data()),
                                ackPayload.size());
  ack.setPayload(ackPayloadBuffer, ackPayloadBuffer.size());
  const auto ackWire = ack.WireEncode();
  checkMessage("ACK", makeRequestAckNameV2(provider, user, service, requestId),
               provider, "/PERMISSION/HELLO",
               ndn::Buffer(ackWire.value(), ackWire.value_size()));

  ResponseMessage response;
  response.setStatus(true);
  response.setErrorInfo("ok");
  response.setUserToken("user-token-000001");
  ndn::Buffer responsePayload(reinterpret_cast<const uint8_t*>("HELLO"), 5);
  response.setPayload(responsePayload, responsePayload.size());
  const auto responseWire = response.WireEncode();
  checkMessage("RESPONSE", makeResponseNameV2(provider, user, service, requestId),
               provider, "/PERMISSION/HELLO",
               ndn::Buffer(responseWire.value(), responseWire.value_size()));

  ServiceSelectionMessage selection;
  selection.setRequestIDs({requestId.toUri()});
  selection.setAttempt(1);
  selection.addProviderEntry({provider,
                              std::string(64, 'a'),
                              ndn::Buffer(reinterpret_cast<const uint8_t*>("assign=none"), 11)});
  const auto selectionWire = selection.WireEncode();
  checkMessage("SELECTION", makeServiceSelectionNameV2(user, provider, service, requestId),
               user, "/SERVICE/HELLO",
               ndn::Buffer(selectionWire.value(), selectionWire.value_size()));
}

BOOST_AUTO_TEST_CASE(HybridKeyEpochRotatesByUsesAndNonceIsUnique)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const ndn::Name serviceName("/HELLO");
  const ndn::Name sender("/test/user/alice");

  std::set<std::string> nonces;
  std::string firstKeyId;
  for (size_t i = 0; i < HybridMessageCrypto::MAX_EPOCH_USES; ++i) {
    auto key = crypto.getOrCreateSendKey(serviceName, sender, "/SERVICE/HELLO",
                                         "REQUEST", counters);
    if (i == 0) {
      firstKeyId = key.keyId;
    }
    BOOST_CHECK_EQUAL(key.keyId, firstKeyId);
    ndn::Buffer plaintext(reinterpret_cast<const uint8_t*>("x"), 1);
    ndn::Buffer ad(reinterpret_cast<const uint8_t*>("ad"), 2);
    auto encrypted = hybridAesGcmEncrypt(
      key.key,
      ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
      ndn::span<const uint8_t>(ad.data(), ad.size()));
    nonces.insert(std::string(reinterpret_cast<const char*>(encrypted.nonce.data()),
                              encrypted.nonce.size()));
  }
  BOOST_CHECK_EQUAL(nonces.size(), HybridMessageCrypto::MAX_EPOCH_USES);

  auto rotated = crypto.getOrCreateSendKey(serviceName, sender, "/SERVICE/HELLO",
                                           "REQUEST", counters);
  BOOST_CHECK_NE(rotated.keyId, firstKeyId);
  BOOST_CHECK_EQUAL(counters.hybrid_key_rotation_uses.load(), 1);
}

BOOST_AUTO_TEST_CASE(ProviderRequiresPermissionAndUserToken)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:generic-auth-negative",
                                   "tpm-memory:generic-auth-negative");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  const ndn::Name requestId("/request-auth-negative");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-auth-negative"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert, "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{},
                           face,
                           ndn::Name("/test/group"),
                           providerCert,
                           aaCert,
                           "examples/trust-any.conf");

  bool handlerCalled = false;
  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
        handlerCalled = true;
        response.setClassification(1);
      }));

  installPermissions(user, provider, requesterName, serviceName);

  const auto requestName = makeRequestNameV2(requesterName,
                                            serviceName,
                                            requestId);
  auto goodRequest = makeRequestMessageWithUserToken("payload");
  auto goodResponse = provider.handleDecryptedRequestByName(requestName, goodRequest);
  BOOST_CHECK(goodResponse.getStatus());
  BOOST_CHECK_EQUAL(goodResponse.getUserToken(), goodRequest.getUserToken());
  BOOST_CHECK(handlerCalled);
  BOOST_TEST_MESSAGE("NDNSF_AUTH_CASE case_id=authorized_current_fresh terminal=allow observed_executions=1 gate=response_acceptance");

  handlerCalled = false;
  auto missingUserTokenRequest = makeRequestMessageWithUserToken("payload", "");
  auto missingUserTokenResponse =
    provider.handleDecryptedRequestByName(requestName, missingUserTokenRequest);
  BOOST_CHECK(!missingUserTokenResponse.getStatus());
  BOOST_CHECK(!handlerCalled);

  ServiceProvider providerWithoutPermission(ServiceProvider::LocalMockTag{},
                                            face,
                                            ndn::Name("/test/group"),
                                            providerCert,
                                            aaCert,
                                            "examples/trust-any.conf");
  providerWithoutPermission.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name&, const DynamicRequest&, DynamicResponse& response) {
        handlerCalled = true;
        response.setClassification(2);
      }));

  auto missingProviderPermissionResponse =
    providerWithoutPermission.handleDecryptedRequestByName(requestName, goodRequest);
  BOOST_CHECK(!missingProviderPermissionResponse.getStatus());
  BOOST_CHECK(!handlerCalled);
  BOOST_TEST_MESSAGE("NDNSF_AUTH_CASE case_id=provider_permission_absent terminal=deny observed_executions=0 gate=provider_authorization");
}

BOOST_AUTO_TEST_CASE(RequestServiceRequiresUserPermissionBeforePublication)
{
  ndn::security::KeyChain keyChain("pib-memory:normal-user-permission",
                                   "tpm-memory:normal-user-permission");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-user-permission"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");

  size_t publications = 0;
  user.setRequestPublisher(
    [&] (const ndn::Name&,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage&,
         size_t) {
      ++publications;
    });

  DynamicRequest request;
  request.setPayload("frame-bytes");
  const auto deniedRequestId = user.RequestService<DynamicRequest, DynamicResponse>(
    {providerName},
    serviceName,
    request,
    std::function<void(const DynamicResponse&)>([] (const DynamicResponse&) {}),
    std::function<void()>([] {}),
    1000,
    tlv::FirstResponding);

  BOOST_CHECK(deniedRequestId.empty());
  BOOST_CHECK_EQUAL(publications, 0);
  BOOST_TEST_MESSAGE("NDNSF_AUTH_CASE case_id=user_permission_absent terminal=deny observed_executions=0 gate=user_authorization");
}

BOOST_AUTO_TEST_CASE(AuthorizedRequestPublishesForExplicitAndDiscoveredProviders)
{
  ndn::security::KeyChain keyChain("pib-memory:normal-user-authorized",
                                   "tpm-memory:normal-user-authorized");
  ndn::DummyClientFace face(keyChain);
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/ObjectDetection/YOLOv8");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-user-authorized"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");
  user.applyPermissionResponse(
    makePermissionResponse(requesterName,
                           tlv::UserPermission,
                           providerName,
                           serviceName));

  size_t publications = 0;
  user.setRequestPublisher(
    [&] (const ndn::Name&,
         const ndn::Name&,
         const std::vector<ndn::Name>&,
         const ndn::Name&,
         const RequestMessage&,
         size_t) {
      ++publications;
    });

  RequestMessage request;
  const auto explicitRequestId = user.RequestService(
    {providerName}, serviceName, request, 1000,
    [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
    tlv::FirstResponding);
  const auto discoveredRequestId = user.RequestService(
    serviceName, request, 1000,
    [] (const ndn::Name&) {}, [] (const ResponseMessage&) {},
    tlv::FirstResponding);

  BOOST_CHECK(!explicitRequestId.empty());
  BOOST_CHECK(!discoveredRequestId.empty());
  BOOST_CHECK_EQUAL(publications, 2);
}

BOOST_AUTO_TEST_CASE(UnpermittedProviderAckAndResponseAreRejected)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:provider-user-permission",
                                   "tpm-memory:provider-user-permission");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name permittedProvider("/test/provider/permitted");
  const ndn::Name unpermittedProvider("/test/provider/unpermitted");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-provider-permission");
  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-provider-permission"));
  LocalServiceUser user(face,
                        ndn::Name("/test/group"),
                        userCert,
                        aaCert,
                        "examples/trust-any.conf");
  user.applyPermissionResponse(
    makePermissionResponse(requesterName,
                           tlv::UserPermission,
                           permittedProvider,
                           serviceName));
  user.addPendingCallForTokenTest(requestId, serviceName, "user-token");

  auto ack = makeSuccessAck();
  ack.setUserToken("user-token");
  ack.setProviderToken("provider-token");
  const auto ackName = makeRequestAckNameV2(
    unpermittedProvider, requesterName, serviceName, requestId);
  BOOST_CHECK(!user.handleRequestAckByName(ackName, ack));
  BOOST_CHECK_EQUAL(user.getPendingRequestAckCount(requestId), 0);

  size_t responseCallbacks = 0;
  user.setPendingResponseHandlerForTest(
    requestId, [&] (const ResponseMessage&) { ++responseCallbacks; });
  ResponseMessage response;
  response.setStatus(true);
  response.setUserToken("user-token");
  BOOST_CHECK(!user.handleDecryptedResponse(
    requestId, unpermittedProvider, response));
  BOOST_CHECK_EQUAL(responseCallbacks, 0);
}

BOOST_AUTO_TEST_CASE(StalePolicyEpochRejectsProviderRequestBeforeHandlers)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:stale-policy-epoch",
                                   "tpm-memory:stale-policy-epoch");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/HELLO");
  const ndn::Name requestId("/request-stale-policy-epoch");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-stale-policy-epoch"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName,
                           2));

  size_t ackHandlerCalls = 0;
  size_t serviceHandlerCalls = 0;
  provider.addService(
    serviceName,
    ServiceProvider::AckStrategyHandler([&] (const RequestMessage&) {
      ++ackHandlerCalls;
      ServiceProvider::AckDecision decision;
      decision.status = true;
      return decision;
    }),
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++serviceHandlerCalls;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  auto request = makeRequestMessageWithUserToken("hello");
  request.setPolicyEpoch(1);
  const auto requestBlock = request.WireEncode();
  const ndn::Buffer requestWire(requestBlock.data(), requestBlock.size());
  provider.OnRequestDecryptionSuccessCallbackV2(
    requesterName, serviceName, requestId, requestWire);

  BOOST_CHECK_EQUAL(ackHandlerCalls, 0);
  BOOST_CHECK_EQUAL(serviceHandlerCalls, 0);
  BOOST_TEST_MESSAGE("NDNSF_AUTH_CASE case_id=stale_policy_epoch terminal=deny observed_executions=0 gate=policy_epoch");
}

BOOST_AUTO_TEST_CASE(NewUserUsesUnchangedProviderAfterControllerMaterialRefresh)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:new-user-provider-refresh",
                                   "tpm-memory:new-user-provider-refresh");
  const ndn::Name requesterName("/test/user/new-user");
  const ndn::Name providerName("/test/provider/existing");
  const ndn::Name serviceName("/HELLO");
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-onboarding"));
  LocalServiceProvider provider(face,
                                ndn::Name("/test/group"),
                                providerCert,
                                aaCert,
                                "examples/trust-any.conf");
  provider.applyPermissionResponse(
    makePermissionResponse(providerName,
                           tlv::ProviderPermission,
                           providerName,
                           serviceName,
                           1));

  size_t ackHandlerCalls = 0;
  size_t serviceHandlerCalls = 0;
  provider.addService(
    serviceName,
    ServiceProvider::AckStrategyHandler([&] (const RequestMessage&) {
      ++ackHandlerCalls;
      ServiceProvider::AckDecision decision;
      decision.status = true;
      return decision;
    }),
    ServiceProvider::RequestHandler(
      [&] (const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const ndn::Name&,
           const RequestMessage&) {
        ++serviceHandlerCalls;
        ResponseMessage response;
        response.setStatus(true);
        return response;
      }));

  auto request = makeRequestMessageWithUserToken("new-user-request");
  request.setPolicyEpoch(2);
  const auto requestBlock = request.WireEncode();
  const ndn::Buffer requestWire(requestBlock.data(), requestBlock.size());
  const ndn::Name staleRequestId("/request-before-provider-refresh");
  provider.OnRequestDecryptionSuccessCallbackV2(
    requesterName, serviceName, staleRequestId, requestWire);
  BOOST_CHECK_EQUAL(ackHandlerCalls, 0);
  BOOST_CHECK_EQUAL(serviceHandlerCalls, 0);
  BOOST_CHECK(!provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, staleRequestId));

  const auto refreshStart = std::chrono::steady_clock::now();
  auto refreshedPermission = makePermissionResponse(providerName,
                                                     tlv::ProviderPermission,
                                                     providerName,
                                                     serviceName,
                                                     2);
  provider.applyPermissionResponse(refreshedPermission);
  BOOST_CHECK_EQUAL(provider.getCurrentPolicyEpoch(), 2);

  const ndn::Name refreshedRequestId("/request-after-provider-refresh");
  provider.OnRequestDecryptionSuccessCallbackV2(
    requesterName, serviceName, refreshedRequestId, requestWire);
  BOOST_CHECK_EQUAL(ackHandlerCalls, 1);
  BOOST_CHECK(provider.hasPendingRequestForTokenTest(
    requesterName, serviceName, refreshedRequestId));

  const std::string providerToken("provider-token-after-refresh");
  provider.addPendingRequestForTokenTest(requesterName,
                                         serviceName,
                                         refreshedRequestId,
                                         request,
                                         providerToken);
  const auto selection = makeSelectionBuffer(refreshedRequestId, providerToken);
  provider.OnServiceSelectionMessageDecryptionSuccessCallbackV2(
    requesterName, providerName, serviceName, refreshedRequestId, selection);
  BOOST_CHECK_EQUAL(serviceHandlerCalls, 1);

  PolicyManifest manifest;
  manifest.setPolicyEpoch(2);
  manifest.setRequiredKeyEpoch(2);
  manifest.setValidFromMs(0);
  manifest.setGracePeriodMs(0);
  const auto permissionWire = refreshedPermission.WireEncode();
  const auto manifestWire = manifest.WireEncode();
  const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now() - refreshStart).count();

  BOOST_TEST_MESSAGE(
    "NDNSF_ONBOARDING_CASE stale_terminal=deny refreshed_terminal=allow"
    " stale_executions=0 refreshed_executions=1 old_epoch=1 new_epoch=2"
    " provider_manual_changes=0 refresh_operations=1 control_bytes="
    << (permissionWire.size() + manifestWire.size())
    << " time_to_first_success_us=" << elapsedUs);
}

BOOST_AUTO_TEST_CASE(LegacySplitNameFailsClosed)
{
  ndn::Face face;
  ndn::security::KeyChain keyChain("pib-memory:generic-v1-name-negative",
                                   "tpm-memory:generic-v1-name-negative");
  const ndn::Name requesterName("/test/user/alice");
  const ndn::Name providerName("/test/provider/camera");
  const ndn::Name serviceName("/ObjectDetection/YOLOv8");

  auto userCert = makeRsaIdentity(keyChain, requesterName);
  auto providerCert = makeRsaIdentity(keyChain, providerName);
  auto aaCert = makeRsaIdentity(keyChain, ndn::Name("/test/aa-v1-name-negative"));

  LocalServiceUser user(face, ndn::Name("/test/group"), userCert, aaCert,
                        "examples/trust-any.conf");
  ServiceProvider provider(ServiceProvider::LocalMockTag{},
                           face,
                           ndn::Name("/test/group"),
                           providerCert,
                           aaCert,
                           "examples/trust-any.conf");

  bool handlerCalled = false;
  provider.addHandler<DynamicRequest, DynamicResponse>(
    serviceName,
    std::function<void(const ndn::Name&, const DynamicRequest&, DynamicResponse&)>(
      [&] (const ndn::Name&, const DynamicRequest&, DynamicResponse&) {
        handlerCalled = true;
      }));
  installPermissions(user, provider, requesterName, serviceName);

  // A V1 request appended FunctionName and BloomFilter components after the
  // service. V2 treats the complete suffix as one unified service name, so the
  // exact authorization lookup must reject it before dispatch.
  ndn::Name legacyName(requesterName);
  legacyName.append("NDNSF")
            .append("REQUEST")
            .append("ObjectDetection")
            .append("YOLOv8")
            .append("legacy-function")
            .append("legacy-bloom")
            .append("request-v1");

  const auto response = provider.handleDecryptedRequestByName(
    legacyName, makeRequestMessageWithUserToken("payload"));
  BOOST_CHECK(!response.getStatus());
  BOOST_CHECK(response.getErrorInfo().find("Permission denied") != std::string::npos);
  BOOST_CHECK(!handlerCalled);
}

BOOST_AUTO_TEST_CASE(SelectionGatedInputUsesFreshRecipientWrappedKeyAndBoundAad)
{
  ndn::security::KeyChain keyChain("pib-memory:selection-input",
                                   "tpm-memory:selection-input");
  const ndn::Name requester("/test/user/input");
  const ndn::Name service("/Inference/Generic");
  const ndn::Name requestId("request-input-1");
  auto recipient = makeRsaIdentity(keyChain, ndn::Name("/test/provider/input"));
  auto wrongRecipient = makeRsaIdentity(keyChain, ndn::Name("/test/provider/wrong"));
  const std::string input = "private-input-before-selection";
  auto encrypted = encryptSelectionGatedInput(
    requester, service, requestId,
    ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(input.data()),
                             input.size()));
  BOOST_CHECK(encrypted.first.getField("ciphertext").find(input) == std::string::npos);
  RequestMessage wireRequest;
  wireRequest.setEncryptedRequestInput(encrypted.first);
  ndn::Buffer empty;
  wireRequest.setPayload(empty, 0);
  const auto requestWire = wireRequest.WireEncode();
  const std::string wireText(reinterpret_cast<const char*>(requestWire.data()),
                             requestWire.size());
  BOOST_CHECK(wireText.find(input) == std::string::npos);
  const auto publicKey = recipient.getPublicKey();
  const auto wrapped = wrapSelectionGatedInputKey(
    encrypted.second, ndn::span<const uint8_t>(publicKey.data(), publicKey.size()));
  const auto unwrapped = unwrapSelectionGatedInputKey(
    wrapped, recipient.getName(), keyChain);
  ndn::Buffer plaintext;
  BOOST_REQUIRE(decryptSelectionGatedInput(
    encrypted.first, unwrapped, requester, service, requestId, plaintext));
  BOOST_CHECK_EQUAL(std::string(reinterpret_cast<const char*>(plaintext.data()),
                                plaintext.size()), input);

  ndn::Buffer wrongPlaintext;
  BOOST_CHECK(!decryptSelectionGatedInput(
    encrypted.first, unwrapped, requester, service,
    ndn::Name("different-request"), wrongPlaintext));
  BOOST_CHECK_THROW(unwrapSelectionGatedInputKey(
    wrapped, wrongRecipient.getName(), keyChain), std::exception);

  auto tampered = encrypted.first;
  auto tag = tampered.getField("authTag");
  tag[0] = tag[0] == '0' ? '1' : '0';
  tampered.setField("authTag", tag);
  BOOST_CHECK(!decryptSelectionGatedInput(
    tampered, unwrapped, requester, service, requestId, wrongPlaintext));
}

BOOST_AUTO_TEST_CASE(RecipientAssignmentIsFreshPlanBoundAndNotCrossDecryptable)
{
  ndn::security::KeyChain keyChain("pib-memory:recipient-assignment",
                                   "tpm-memory:recipient-assignment");
  const ndn::Name requester("/test/user/assignment");
  const ndn::Name provider("/test/provider/assignment");
  const ndn::Name service("/Inference/Generic");
  const ndn::Name requestId("request-assignment-1");
  auto recipient = makeRsaIdentity(keyChain, provider);
  auto wrong = makeRsaIdentity(keyChain, ndn::Name("/test/provider/other"));
  const std::string payload = "role=merge;privateModelFragment=layer-7;";
  const auto aad = recipientAssignmentAssociatedData(
    requester, provider, service, requestId, "reservation-1", "sha256:plan");
  const auto publicKey = recipient.getPublicKey();
  const auto first = encryptRecipientAssignment(
    ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()),
    publicKey, provider, recipient.getName(), aad);
  const auto second = encryptRecipientAssignment(
    ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()),
    publicKey, provider, recipient.getName(), aad);
  BOOST_CHECK_NE(first.getField("nonce"), second.getField("nonce"));
  BOOST_CHECK_NE(first.getField("wrappedAssignmentKey"),
                 second.getField("wrappedAssignmentKey"));
  BOOST_CHECK(first.getField("ciphertext").find("layer-7") == std::string::npos);
  ndn::Buffer plaintext;
  BOOST_REQUIRE(decryptRecipientAssignment(
    first, provider, recipient.getName(), keyChain, aad, plaintext));
  BOOST_CHECK_EQUAL(std::string(reinterpret_cast<const char*>(plaintext.data()),
                                plaintext.size()), payload);
  const auto wrongAad = recipientAssignmentAssociatedData(
    requester, provider, service, requestId, "reservation-1", "sha256:other");
  BOOST_CHECK(!decryptRecipientAssignment(
    first, provider, recipient.getName(), keyChain, wrongAad, plaintext));
  BOOST_CHECK(!decryptRecipientAssignment(
    first, ndn::Name("/test/provider/other"), wrong.getName(), keyChain, aad, plaintext));
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
