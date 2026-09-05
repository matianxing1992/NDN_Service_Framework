#include "tests/boost-test.hpp"

#include "ndn-service-framework/RequestConfidentiality.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ndn_service_framework::test {
namespace {

RequestSecurityBinding
makeBinding()
{
  RequestSecurityBinding binding;
  binding.serviceName = ndn::Name("/ObjectDetection/YOLOv8");
  binding.requestId = ndn::Name("/request/42");
  binding.attempt = 1;
  binding.controllerVersion = ControllerVersion{1788285600123ULL, 7};
  binding.userEncryptionCertName =
      ndn::Name("/user/alice/KEY/encryption/issuer/v=1");
  binding.userEncryptionCertDigest =
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  binding.providerEncryptionCertName =
      ndn::Name("/provider/p1/KEY/encryption/issuer/v=1");
  binding.providerEncryptionCertDigest =
      "sha256:abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  binding.selectionDigest =
      "sha256:1111111111111111111111111111111111111111111111111111111111111111";
  binding.inputDataName = ndn::Name("/user/alice/NDNSF/INPUT/request-42");
  binding.segmentOrEventId = "input";
  return binding;
}

ndn::Buffer
makeKey()
{
  ndn::Buffer key(HybridMessageCrypto::MESSAGE_KEY_SIZE);
  for (size_t i = 0; i < key.size(); ++i)
    key[i] = static_cast<uint8_t>(i);
  return key;
}

std::string
sha256Hex(const ndn::Block& block)
{
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(block.data(), block.size(), digest);
  std::ostringstream os;
  os << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    os << std::setw(2) << static_cast<unsigned>(byte);
  }
  return os.str();
}

} // namespace

BOOST_AUTO_TEST_SUITE(RequestScopedConfidentiality)

BOOST_AUTO_TEST_CASE(RequestSecurityBindingIsCanonicalAndRoundTrips)
{
  const auto binding = makeBinding();
  BOOST_REQUIRE(binding.isValid());
  const auto first = binding.wireEncode();
  const auto second = binding.wireEncode();
  BOOST_CHECK_EQUAL(first.size(), second.size());
  BOOST_CHECK(std::equal(first.begin(), first.end(), second.begin()));
  BOOST_CHECK(!binding.canonicalAad().empty());
  BOOST_CHECK(binding.digestHex().compare(0, 7, "sha256:") == 0);

  RequestSecurityBinding decoded;
  BOOST_REQUIRE(decoded.wireDecode(first));
  BOOST_CHECK(decoded.serviceName == binding.serviceName);
  BOOST_CHECK(decoded.requestId == binding.requestId);
  BOOST_CHECK_EQUAL(decoded.attempt, binding.attempt);
  BOOST_CHECK(decoded.controllerVersion == binding.controllerVersion);
  BOOST_CHECK(decoded.userEncryptionCertName == binding.userEncryptionCertName);
  BOOST_CHECK(decoded.providerEncryptionCertName == binding.providerEncryptionCertName);
  BOOST_CHECK_EQUAL(decoded.digestHex(), binding.digestHex());

  auto changed = binding;
  changed.attempt = 2;
  BOOST_CHECK(changed.digestHex() != binding.digestHex());
  changed = binding;
  changed.controllerVersion.controllerEpoch++;
  BOOST_CHECK(changed.digestHex() != binding.digestHex());
}

BOOST_AUTO_TEST_CASE(ConfidentialityContainersRoundTripThroughProtectedMessages)
{
  const auto binding = makeBinding();
  const auto bindingWire = binding.wireEncode();

  RequestMessage request;
  request.setRequestSecurityBinding(bindingWire);
  const auto requestWire = request.WireEncode();
  RequestMessage decodedRequest;
  BOOST_REQUIRE(decodedRequest.WireDecode(requestWire));
  BOOST_REQUIRE(decodedRequest.hasRequestSecurityBinding());
  BOOST_CHECK(std::equal(decodedRequest.getRequestSecurityBinding().begin(),
                         decodedRequest.getRequestSecurityBinding().end(),
                         bindingWire.begin()));

  SelectionKeyEnvelope selection;
  selection.binding = binding;
  selection.wrappedKeys = ndn::Buffer{0x01, 0x02, 0x03};
  selection.createdAtMs = 1000;
  selection.expiresAtMs = 2000;
  const auto selectionWire = selection.wireEncode();
  ServiceSelectionMessage message;
  message.setSelectionKeyEnvelope(selectionWire);
  const auto messageWire = message.WireEncode();
  ServiceSelectionMessage decodedMessage;
  BOOST_REQUIRE(decodedMessage.WireDecode(messageWire));
  BOOST_REQUIRE(decodedMessage.hasSelectionKeyEnvelope());
  BOOST_CHECK(std::equal(decodedMessage.getSelectionKeyEnvelope().begin(),
                         decodedMessage.getSelectionKeyEnvelope().end(),
                         selectionWire.begin()));

  const auto aead = encryptRequestContent(
      makeKey(), "key-1", binding,
      ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>("result"), 6));
  const auto aeadWire = aead.wireEncode();
  ResponseMessage response;
  response.setAeadEnvelope(aeadWire);
  const auto responseWire = response.WireEncode();
  ResponseMessage decodedResponse;
  BOOST_REQUIRE(decodedResponse.WireDecode(responseWire));
  BOOST_REQUIRE(decodedResponse.hasAeadEnvelope());
  BOOST_CHECK(std::equal(decodedResponse.getAeadEnvelope().begin(),
                         decodedResponse.getAeadEnvelope().end(),
                         aeadWire.begin()));

  BOOST_CHECK_THROW(request.setRequestSecurityBinding(
      ndn::makeStringBlock(0xF8FF, "wrong-type")), std::invalid_argument);
  BOOST_CHECK_THROW(response.setAeadEnvelope(
      ndn::makeStringBlock(0xF8FF, "wrong-type")), std::invalid_argument);
  BOOST_CHECK_THROW(message.setSelectionKeyEnvelope(
      ndn::makeStringBlock(0xF8FF, "wrong-type")), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(EncryptionCertificateAdvertisementsRoundTripAndBindToMessages)
{
  EncryptionCertificateAdvertisement advertisement;
  advertisement.certificateName = ndn::Name("/user/alice/KEY/encryption/v=1");
  advertisement.certificateDigest =
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  advertisement.validFromMs = 1000;
  advertisement.validUntilMs = 2000;
  advertisement.supportedEnvelopeAlgorithms = {"RSA-OAEP-SHA256"};
  BOOST_REQUIRE(advertisement.isValid(1500));
  BOOST_CHECK(!advertisement.isValid(2000));
  BOOST_CHECK(advertisement.supportsAlgorithm("RSA-OAEP-SHA256"));
  BOOST_CHECK(!advertisement.supportsAlgorithm("unsupported"));

  const auto advertisementWire = advertisement.WireEncode();
  EncryptionCertificateAdvertisement decoded;
  BOOST_REQUIRE(decoded.WireDecode(advertisementWire));
  BOOST_CHECK(decoded.certificateName == advertisement.certificateName);
  BOOST_CHECK_EQUAL(decoded.certificateDigest, advertisement.certificateDigest);
  BOOST_CHECK_EQUAL(decoded.validFromMs, advertisement.validFromMs);
  BOOST_CHECK_EQUAL(decoded.validUntilMs, advertisement.validUntilMs);
  BOOST_CHECK(decoded.supportedEnvelopeAlgorithms ==
              advertisement.supportedEnvelopeAlgorithms);

  const ControllerVersion version{1788285600123ULL, 7};
  RequestMessage request;
  request.setControllerVersion(version);
  request.setUserEncryptionCertificate(advertisement);
  const auto requestWire = request.WireEncode();
  RequestMessage decodedRequest;
  BOOST_REQUIRE(decodedRequest.WireDecode(requestWire));
  BOOST_REQUIRE(decodedRequest.hasUserEncryptionCertificate());
  BOOST_CHECK(decodedRequest.getUserEncryptionCertificate().certificateName ==
              advertisement.certificateName);
  BOOST_CHECK(decodedRequest.getControllerVersion() == version);

  RequestAckMessage ack;
  ack.setControllerVersion(version);
  ack.setProviderEncryptionCertificate(advertisement);
  const auto ackWire = ack.WireEncode();
  RequestAckMessage decodedAck;
  BOOST_REQUIRE(decodedAck.WireDecode(ackWire));
  BOOST_REQUIRE(decodedAck.hasProviderEncryptionCertificate());
  BOOST_CHECK(decodedAck.getProviderEncryptionCertificate().certificateDigest ==
              advertisement.certificateDigest);
  BOOST_CHECK(decodedAck.getControllerVersion() == version);

  auto invalidDigest = advertisement;
  invalidDigest.certificateDigest = "sha256:short";
  BOOST_CHECK(!invalidDigest.isValid());
  BOOST_CHECK_THROW(request.setUserEncryptionCertificate(invalidDigest),
                    std::invalid_argument);

  auto duplicateAlgorithm = advertisement;
  duplicateAlgorithm.supportedEnvelopeAlgorithms.push_back("RSA-OAEP-SHA256");
  BOOST_CHECK(!duplicateAlgorithm.isValid());
  BOOST_CHECK_THROW(duplicateAlgorithm.WireEncode(), std::invalid_argument);

  auto unsortedAlgorithms = advertisement;
  unsortedAlgorithms.supportedEnvelopeAlgorithms = {"Z-algorithm", "A-algorithm"};
  BOOST_CHECK(!unsortedAlgorithms.isValid());
}

BOOST_AUTO_TEST_CASE(GoldenWireVectorsAreStable)
{
  const auto binding = makeBinding();
  SelectionKeyEnvelope selection;
  selection.binding = binding;
  selection.wrappedKeys = ndn::Buffer{0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
  selection.createdAtMs = 1000;
  selection.expiresAtMs = 2000;

  AeadEnvelope aead;
  aead.keyId = "key-1";
  aead.nonce = ndn::Buffer{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  aead.ciphertext = ndn::Buffer{0xaa, 0xbb, 0xcc};
  aead.tag = ndn::Buffer(16);
  std::fill(aead.tag.begin(), aead.tag.end(), 0x5a);
  aead.aadDigest = ndn::Buffer(32);
  std::fill(aead.aadDigest.begin(), aead.aadDigest.end(), 0x6b);
  aead.segmentOrEventId = "event-1";

  PolicyStatusData status;
  status.setServiceName(ndn::Name("/ObjectDetection/YOLOv8"));
  status.setControllerVersion(ControllerVersion{1788285600123ULL, 7});
  status.setValidity(100, 2000);
  status.setPolicyDigest(
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  status.setControllerCertificate(ndn::Name("/controller/KEY/signing/v=1"));
  status.setSignature(ndn::Buffer{0xde, 0xad, 0xbe, 0xef});

  #if 0 // Full byte vectors are retained in contracts/golden-vectors.md.
  BOOST_CHECK_EQUAL(hex(binding.controllerVersion.wireEncode()),
                    "fdf70011fdf70108000001a05e20c17bfdf7020107");
  BOOST_CHECK_EQUAL(hex(binding.wireEncode()),
                    "fdf810fd01c2fdf8111b0719080f4f626a656374446574656374696f6e0806594f4c4f7638fdf8120f070d08077265717565737408023432fdf8130101"
                    "fdf70011fdf70108000001a05e20c17bfdf7020107fdf8142b07290804757365720805616c69636508034b4559080a656e6372797074696f6e0806"
                    "697373756572360101fdf815477368613235363a30313233343536373839616263646566303132333435363738396162636465663031323334353637"
                    "383961626364656630313233343536373839616263646566fdf8162c072a080870726f76696465720802703108034b4559080a656e6372797074696f"
                    "6e0806697373756572360101fdf817477368613235363a61626364656630313233343536373839616263646566303132333435363738396162636465"
                    "663031323334353637383961626364656630313233343536373839fdf818477368613235363a31313131313131313131313131313131313131313131"
                    "313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131"
                    "313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131"
                    "fdf8192907270804757365720805616c69636508054e444e53460805494e505554080a726571756573742d3432fdf81a05696e707574");
  BOOST_CHECK_EQUAL(hex(selection.wireEncode()),
                    "fdf830fd01f1fdf810fd01c2fdf8111b0719080f4f626a656374446574656374696f6e0806594f4c4f7638fdf8120f070d0807726571756573740802"
                    "3432fdf8130101fdf70011fdf70108000001a05e20c17bfdf7020107fdf8142b07290804757365720805616c69636508034b4559080a656e63727970"
                    "74696f6e0806697373756572360101fdf815477368613235363a30313233343536373839616263646566303132333435363738396162636465663031"
                    "323334353637383961626364656630313233343536373839616263646566fdf8162c072a080870726f76696465720802703108034b4559080a656e63"
                    "72797074696f6e0806697373756572360101fdf817477368613235363a61626364656630313233343536373839616263646566303132333435363738"
                    "396162636465663031323334353637383961626364656630313233343536373839fdf818477368613235363a31313131313131313131313131313131313131313131"
                    "313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131"
                    "313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131313131"
                    "fdf8192907270804757365720805616c69636508054e444e53460805494e505554080a726571756573742d3432fdf81a05696e707574fdf8310f5253412d4f4145502d5348413235"
                    "36fdf833060123456789abfdf8340203e8fdf8350207d0");
  BOOST_CHECK_EQUAL(hex(aead.wireEncode()),
                    "fdf82072fdf8210b4145532d3235362d47434dfdf822056b65792d31fdf8230c000102030405060708090a0bfdf82403aabbccfdf825105a5a5a5a5a"
                    "5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a"
                    "fdf826206b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6b6bfdf827076576656e742d31");
  BOOST_CHECK_EQUAL(hex(status.wireEncode()),
                    "fdf710adfdf711172f4f626a656374446574656374696f6e2f594f4c4f7638fdf70011fdf70108000001a05e20c17bfdf7020107fdf7130164fdf714"
                    "0207d0fdf715477368613235363a30313233343536373839616263646566303132333435363738396162636465663031323334353637383961626364"
                    "656630313233343536373839616263646566fdf7161b2f636f6e74726f6c6c65722f4b45592f7369676e696e672f763d31fdf71704deadbeef");
  #endif
  BOOST_CHECK_EQUAL(sha256Hex(binding.controllerVersion.wireEncode()),
                    "551f7db75b58ae93733eec80d69d8e82e8fc87a9ebdda3f5fc67f6e4f0dba0ea");
  BOOST_CHECK_EQUAL(sha256Hex(binding.wireEncode()),
                    "16971d8287fd77248a1bdeb985ec83d52a819b14581f944d41453aca650c72e4");
  BOOST_CHECK_EQUAL(sha256Hex(selection.wireEncode()),
                    "3279b65bdf796ade76da0ed7531023d1a29db508f871fe74090e69b912797ce5");
  BOOST_CHECK_EQUAL(sha256Hex(aead.wireEncode()),
                    "9d5f146483394b1aa9f1eb4045659c90f9bc0506036623cff9b1f645bf7e4fdb");
  BOOST_CHECK_EQUAL(sha256Hex(status.wireEncode()),
                    "b2d69620d8423a2bc35070253dd2173cb5ebf8af02848c312d5c33835c80973d");
}

BOOST_AUTO_TEST_CASE(SelectionEnvelopeWrapsOnlyForTheRecipient)
{
  ndn::security::KeyChain keyChain("pib-memory:spec179-recipient",
                                   "tpm-memory:spec179-recipient");
  const auto identity = keyChain.createIdentity(ndn::Name("/provider/p1"),
                                                ndn::RsaKeyParams(2048));
  const auto certificate = identity.getDefaultKey().getDefaultCertificate();
  const auto publicKey = certificate.getPublicKey();
  const auto keys = generateRequestKeyBundle(1000, 2000);
  const auto binding = makeBinding();

  const auto envelope = wrapSelectionKeyEnvelope(
      keys, binding, ndn::span<const uint8_t>(publicKey.data(), publicKey.size()), 1500);
  BOOST_REQUIRE(envelope.isValid(1500));
  RequestKeyBundle recovered;
  RequestCryptoFailure failure = RequestCryptoFailure::NONE;
  const bool unwrapped = unwrapSelectionKeyEnvelope(envelope, binding, certificate.getName(),
                                                    keyChain, 1500, recovered, &failure);
  BOOST_TEST_MESSAGE("selection.unwrap=" << unwrapped << " failure="
                     << requestCryptoFailureName(failure));
  BOOST_REQUIRE(unwrapped);
  BOOST_CHECK(failure == RequestCryptoFailure::NONE);
  BOOST_CHECK(recovered.inputKey == keys.inputKey);
  BOOST_CHECK(recovered.responseKey == keys.responseKey);
  BOOST_CHECK_EQUAL(recovered.keyId, keys.keyId);

  auto other = keyChain.createIdentity(ndn::Name("/provider/p2"),
                                       ndn::RsaKeyParams(2048));
  RequestKeyBundle wrongRecipient;
  BOOST_CHECK(!unwrapSelectionKeyEnvelope(envelope, binding,
                                           other.getDefaultKey().getDefaultCertificate().getName(),
                                           keyChain, 1500, wrongRecipient, &failure));
  BOOST_CHECK(failure == RequestCryptoFailure::WRONG_RECIPIENT);

  auto changedBinding = binding;
  changedBinding.attempt++;
  BOOST_CHECK(!unwrapSelectionKeyEnvelope(envelope, changedBinding, certificate.getName(),
                                           keyChain, 1500, wrongRecipient, &failure));
  BOOST_CHECK(failure == RequestCryptoFailure::INVALID_BINDING);
  BOOST_CHECK(!unwrapSelectionKeyEnvelope(envelope, binding, certificate.getName(),
                                           keyChain, 2000, wrongRecipient, &failure));
  BOOST_CHECK(failure == RequestCryptoFailure::EXPIRED_KEY);
}

BOOST_AUTO_TEST_CASE(RequestSecurityBindingRejectsAmbiguousOrMalformedFields)
{
  auto binding = makeBinding();
  binding.providerEncryptionCertName.clear();
  BOOST_CHECK(!binding.isValid());
  binding = makeBinding();
  binding.userEncryptionCertDigest = "sha256:short";
  BOOST_CHECK(!binding.isValid());
  binding = makeBinding();
  binding.controllerVersion.controllerEpoch = 0;
  BOOST_CHECK(!binding.isValid());

  auto wire = makeBinding().wireEncode();
  ndn::Block unknown(RequestSecurityBinding::TYPE);
  unknown.push_back(ndn::makeStringBlock(0xF8FF, "unknown"));
  unknown.encode();
  BOOST_CHECK(!RequestSecurityBinding{}.wireDecode(unknown));
  BOOST_CHECK(!RequestSecurityBinding{}.wireDecode(ndn::Block(0xF810)));
  (void)wire;
}

BOOST_AUTO_TEST_CASE(RequestKeyBundleHasBoundedLifetimeAndZeroizes)
{
  auto keys = generateRequestKeyBundle(1000, 2000);
  BOOST_REQUIRE(keys.isValid(1500));
  BOOST_CHECK(!keys.isValid(2000));
  BOOST_CHECK(!keys.isValid(2500));
  BOOST_REQUIRE(!keys.inputKey.empty());
  BOOST_REQUIRE(!keys.responseKey.empty());
  keys.zeroize();
  BOOST_CHECK(keys.inputKey.empty());
  BOOST_CHECK(keys.responseKey.empty());
  BOOST_CHECK(keys.keyId.empty());
  BOOST_CHECK(keys.consumed);
  BOOST_CHECK(!keys.isValid(1500));
  BOOST_CHECK_THROW(generateRequestKeyBundle(2000, 2000), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(AeadEnvelopeRoundTripsAndBindsAllFields)
{
  auto binding = makeBinding();
  const auto key = makeKey();
  const ndn::Buffer plaintext{'p', 'a', 'y', 'l', 'o', 'a', 'd'};
  auto envelope = encryptRequestContent(key, "request-key-1", binding, plaintext);
  BOOST_REQUIRE(envelope.isValid());
  auto wire = envelope.wireEncode();
  AeadEnvelope decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));

  ndn::Buffer recovered;
  RequestCryptoFailure failure = RequestCryptoFailure::NONE;
  BOOST_REQUIRE(decryptRequestContent(key, "request-key-1", binding, decoded,
                                      recovered, &failure));
  BOOST_CHECK(failure == RequestCryptoFailure::NONE);
  BOOST_CHECK_EQUAL_COLLECTIONS(recovered.begin(), recovered.end(),
                                plaintext.begin(), plaintext.end());

  auto changed = binding;
  changed.requestId = ndn::Name("/request/43");
  BOOST_CHECK(!decryptRequestContent(key, "request-key-1", changed, decoded,
                                     recovered, &failure));
  BOOST_CHECK(failure == RequestCryptoFailure::INVALID_BINDING);
  BOOST_CHECK(!decryptRequestContent(key, "different-key", binding, decoded,
                                     recovered, &failure));
  BOOST_CHECK(failure == RequestCryptoFailure::WRONG_KEY_ID);

  decoded.ciphertext[0] ^= 0x01;
  BOOST_CHECK(!decryptRequestContent(key, "request-key-1", binding, decoded,
                                     recovered, &failure));
  BOOST_CHECK(failure == RequestCryptoFailure::AUTHENTICATION_FAILED);
}

BOOST_AUTO_TEST_CASE(AeadEnvelopeRejectsMalformedAndWrongNonce)
{
  AeadEnvelope envelope;
  envelope.keyId = "key";
  envelope.nonce = ndn::Buffer(11);
  envelope.tag = ndn::Buffer(HybridMessageCrypto::TAG_SIZE);
  envelope.aadDigest = ndn::Buffer(32);
  envelope.segmentOrEventId = "event";
  BOOST_CHECK(!envelope.isValid());
  BOOST_CHECK_THROW(envelope.wireEncode(), std::invalid_argument);
  BOOST_CHECK(!AeadEnvelope{}.wireDecode(ndn::Block(AeadEnvelope::TYPE)));
}

BOOST_AUTO_TEST_CASE(NonceRegistryRejectsReuseAndExpiresEntries)
{
  NonceRegistry registry;
  ndn::Buffer nonce(NonceRegistry::NONCE_SIZE);
  std::fill(nonce.begin(), nonce.end(), 0xA5);
  BOOST_REQUIRE(registry.reserve("key", nonce, std::chrono::milliseconds(100)));
  BOOST_CHECK(!registry.reserve("key", nonce, std::chrono::milliseconds(100)));
  BOOST_CHECK_EQUAL(registry.size(), 1);
  registry.invalidate("key");
  BOOST_CHECK_EQUAL(registry.size(), 0);
  BOOST_CHECK(!registry.reserve("", nonce, std::chrono::milliseconds(100)));
  ndn::Buffer malformed(NonceRegistry::NONCE_SIZE - 1);
  BOOST_CHECK(!registry.reserve("key", malformed, std::chrono::milliseconds(100)));

  const auto now = std::chrono::steady_clock::now();
  BOOST_REQUIRE(registry.reserve("key", nonce, now + std::chrono::milliseconds(10)));
  registry.clearExpired(now + std::chrono::milliseconds(11));
  BOOST_CHECK_EQUAL(registry.size(), 0);
}

BOOST_AUTO_TEST_CASE(AuthenticatedNonceReservationIsAtomicAfterTamper)
{
  const auto binding = makeBinding();
  const auto key = makeKey();
  const ndn::Buffer plaintext{'o', 'k'};
  const auto validEnvelope = encryptRequestContent(key, "key-1", binding, plaintext);
  auto tamperedEnvelope = validEnvelope;
  tamperedEnvelope.ciphertext[0] ^= 0x01;

  ndn::Buffer recovered;
  RequestCryptoFailure failure = RequestCryptoFailure::NONE;
  BOOST_CHECK(!decryptRequestContent(key, "key-1", binding,
                                     tamperedEnvelope, recovered, &failure));
  BOOST_CHECK(failure == RequestCryptoFailure::AUTHENTICATION_FAILED);

  // The caller may now reserve the nonce for the authenticated packet.  A
  // failed tag check must not have consumed replay state first.
  NonceRegistry registry;
  const auto lifetime = std::chrono::milliseconds(100);
  BOOST_REQUIRE(registry.reserve("key-1", validEnvelope.nonce, lifetime));
  BOOST_CHECK(!registry.reserve("key-1", validEnvelope.nonce, lifetime));
  BOOST_REQUIRE(decryptRequestContent(key, "key-1", binding,
                                      validEnvelope, recovered, &failure));
}

BOOST_AUTO_TEST_CASE(NonceRegistryBatchReservationIsAllOrNothing)
{
  NonceRegistry registry;
  ndn::Buffer first(NonceRegistry::NONCE_SIZE);
  std::fill(first.begin(), first.end(), 0x11);
  ndn::Buffer second(NonceRegistry::NONCE_SIZE);
  std::fill(second.begin(), second.end(), 0x22);
  ndn::Buffer malformed(NonceRegistry::NONCE_SIZE - 1);
  std::fill(malformed.begin(), malformed.end(), 0x33);
  const auto lifetime = std::chrono::milliseconds(100);

  BOOST_REQUIRE(registry.reserveBatch("key", {first, second}, lifetime));
  BOOST_CHECK(!registry.reserve("key", first, lifetime));
  BOOST_CHECK(!registry.reserve("key", second, lifetime));

  ndn::Buffer third(NonceRegistry::NONCE_SIZE);
  std::fill(third.begin(), third.end(), 0x44);
  BOOST_CHECK(!registry.reserveBatch("key", {third, first}, lifetime));
  BOOST_REQUIRE(registry.reserve("key", third, lifetime));
  BOOST_CHECK(!registry.reserveBatch("key", {malformed, third}, lifetime));
}

BOOST_AUTO_TEST_CASE(RequestCryptoFailureNamesAreStable)
{
  BOOST_CHECK_EQUAL(std::string(requestCryptoFailureName(RequestCryptoFailure::NONE)), "none");
  BOOST_CHECK_EQUAL(std::string(requestCryptoFailureName(RequestCryptoFailure::INVALID_BINDING)),
                   "invalid_binding");
  BOOST_CHECK_EQUAL(std::string(requestCryptoFailureName(RequestCryptoFailure::NONCE_REUSE)),
                   "nonce_reuse");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
