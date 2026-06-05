#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

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
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
