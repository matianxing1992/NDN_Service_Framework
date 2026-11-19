#include "tests/boost-test.hpp"
#include "ndn-service-framework/detail/ProtectedTargetedRequest.hpp"
#include <ndn-cxx/security/key-params.hpp>
#include <algorithm>
#include <openssl/sha.h>

namespace ndn_service_framework::test {
namespace {

SelectionKeyEnvelope makeCarrierEnvelope()
{
  SelectionKeyEnvelope envelope;
  auto& binding = envelope.binding;
  binding.serviceName = ndn::Name("/service/echo");
  binding.requestId = ndn::Name("/request/1");
  binding.attempt = 1;
  binding.controllerVersion = ControllerVersion{123456, 1};
  binding.userEncryptionCertName = ndn::Name("/user/KEY/key/issuer/v=1");
  binding.userEncryptionCertDigest = "sha256:" + std::string(64, '1');
  binding.providerEncryptionCertName = ndn::Name("/provider/KEY/key/issuer/v=1");
  binding.providerEncryptionCertDigest = "sha256:" + std::string(64, '2');
  binding.selectionDigest = "sha256:" + std::string(64, '3');
  binding.inputDataName = ndn::Name("/user/NDNSF/INPUT/request-1");
  binding.segmentOrEventId = "request-input";
  envelope.envelopeAlgorithm = "RSA-OAEP-SHA256";
  // Include embedded NUL and high bytes to detect accidental text treatment.
  envelope.wrappedKeys.assign(256, 0);
  envelope.wrappedKeys.back() = 255;
  envelope.createdAtMs = 1000;
  envelope.expiresAtMs = 2000;
  return envelope;
}

RequestMessage makeRequest(const char* carrier, const std::string& value)
{
  RequestMessage request;
  request.setRequestMode(tlv::TargetedRequest);
  RequestCapabilities caps;
  caps.setField("RequestScopedConfidentialityV1", "required");
  caps.setField(carrier, value);
  request.setRequestCapabilities(caps);
  return request;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec205ProtectedCarrier)

BOOST_AUTO_TEST_CASE(BinaryNegotiationRequiresExplicitAuthenticatedScopedOffer)
{
  std::map<std::string, std::string> offer;
  BOOST_CHECK(!detail::supportsBinaryTargetedKeyEnvelope(offer, true));
  for (const auto& value : {std::string("0"), std::string("true"), std::string("01"), std::string{}}) {
    offer["targeted.binaryKeyEnvelopeV1"] = value;
    BOOST_CHECK(!detail::supportsBinaryTargetedKeyEnvelope(offer, true));
  }
  offer["targeted.binaryKeyEnvelopeV1"] = "1";
  BOOST_CHECK(!detail::supportsBinaryTargetedKeyEnvelope(offer, false));
  BOOST_CHECK(detail::supportsBinaryTargetedKeyEnvelope(offer, true));
}

BOOST_AUTO_TEST_CASE(BinaryRoundTripPreservesBytesAndRemovesHexExpansion)
{
  const auto wire = makeCarrierEnvelope().wireEncode();
  const std::string binary(reinterpret_cast<const char*>(wire.data()), wire.size());
  const auto hex = selectionGatedHex(ndn::span<const uint8_t>(wire.data(), wire.size()));
  BOOST_REQUIRE_EQUAL(hex.size(), 2 * binary.size());
  auto request = makeRequest(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD, binary);
  RequestMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(request.WireEncode()));
  BOOST_CHECK(detail::decodeProtectedTargetedKeyEnvelope(decoded) == wire);
  const auto legacy = makeRequest(detail::TARGETED_KEY_ENVELOPE_FIELD, hex);
  BOOST_CHECK(detail::decodeProtectedTargetedKeyEnvelope(legacy) == wire);
  // The new field name adds six bytes; ciphertext and TLV length boundaries
  // must not consume the saving from removing the second copy of every byte.
  BOOST_CHECK_GT(legacy.WireEncode().size() - request.WireEncode().size(),
                 binary.size() - 16);
}

BOOST_AUTO_TEST_CASE(RejectAmbiguousEmptyOversizeAndTrailingCarriers)
{
  const auto wire = makeCarrierEnvelope().wireEncode();
  const std::string binary(reinterpret_cast<const char*>(wire.data()), wire.size());
  auto request = makeRequest(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD, binary);
  auto caps = request.getRequestCapabilities();
  caps.setField(detail::TARGETED_KEY_ENVELOPE_FIELD, selectionGatedHex(
    ndn::span<const uint8_t>(wire.data(), wire.size())));
  request.setRequestCapabilities(caps);
  BOOST_CHECK_THROW(detail::decodeProtectedTargetedKeyEnvelope(request), std::invalid_argument);
  for (const auto& value : {std::string{}, binary + std::string("\x00\x00", 2),
                            std::string("not-a-TLV")}) {
    request = makeRequest(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD, value);
    BOOST_CHECK_THROW(detail::decodeProtectedTargetedKeyEnvelope(request), std::exception);
  }
  caps.Clear();
  BOOST_CHECK_THROW(caps.setField(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD,
    std::string(DeploymentControlMessage::MAX_FIELD_VALUE + 1, 'x')), std::exception);
}

BOOST_AUTO_TEST_CASE(RejectReorderedBinaryEnvelope)
{
  const auto wire = makeCarrierEnvelope().wireEncode();
  wire.parse();
  auto fields = wire.elements();
  std::swap(fields[1], fields[2]);
  ndn::Block reordered(SelectionKeyEnvelope::TYPE);
  for (const auto& field : fields) reordered.push_back(field);
  reordered.encode();
  const auto request = makeRequest(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD,
    std::string(reinterpret_cast<const char*>(reordered.data()), reordered.size()));
  BOOST_CHECK_THROW(detail::decodeProtectedTargetedKeyEnvelope(request), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CanonicalEnvelopeBoundsAndMalformedFieldMatrix)
{
  auto envelope = makeCarrierEnvelope();
  auto wire = envelope.wireEncode();
  envelope.wrappedKeys.resize(DeploymentControlMessage::MAX_FIELD_VALUE -
    (wire.size() - envelope.wrappedKeys.size()), 0);
  wire = envelope.wireEncode();
  BOOST_REQUIRE_EQUAL(wire.size(), DeploymentControlMessage::MAX_FIELD_VALUE);
  auto request = makeRequest(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD,
    std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
  BOOST_CHECK(detail::decodeProtectedTargetedKeyEnvelope(request) == wire);

  wire = makeCarrierEnvelope().wireEncode();
  wire.parse();
  for (int variant = 0; variant < 4; ++variant) {
    ndn::Block invalid(variant == 0 ? AeadEnvelope::TYPE : SelectionKeyEnvelope::TYPE);
    for (const auto& field : wire.elements()) {
      if (variant == 1 && field.type() == 0xF835) continue; // missing expiry
      if (variant == 3 && field.type() == 0xF834) {
        const ndn::Buffer nonminimal{0, 0, 0, 0, 0, 0, 3, 232};
        invalid.push_back(ndn::makeBinaryBlock(0xF834, nonminimal.begin(), nonminimal.end()));
      }
      else invalid.push_back(field);
    }
    if (variant == 2) invalid.push_back(wire.elements().back()); // duplicate expiry
    invalid.encode();
    request = makeRequest(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD,
      std::string(reinterpret_cast<const char*>(invalid.data()), invalid.size()));
    BOOST_CHECK_THROW(detail::decodeProtectedTargetedKeyEnvelope(request), std::exception);
  }
}

BOOST_AUTO_TEST_CASE(BindEncodingChoiceAndAuthenticatedRequestFacts)
{
  const ndn::Name user("/user"), provider("/provider"), service("/service/echo");
  const auto id = detail::makeTargetedRequestId(provider, ndn::Name("/request-1"));
  BOOST_REQUIRE_EQUAL(id.size(), 1);
  BOOST_REQUIRE(detail::targetedRequestProvider(id).has_value());
  BOOST_REQUIRE_EQUAL(*detail::targetedRequestProvider(id), provider);
  auto request = makeRequest(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD, "");
  request.setTargetProvider(provider);
  request.setUserToken("user-token");
  request.setProviderToken("provider-token");
  request.setControllerVersion(ControllerVersion{123456, 1});
  EncryptionCertificateAdvertisement advertisement;
  advertisement.certificateName = ndn::Name("/user/KEY/key/issuer/v=1");
  advertisement.certificateDigest = "sha256:" + std::string(64, '1');
  advertisement.validFromMs = 1000;
  advertisement.validUntilMs = 2000;
  advertisement.supportedEnvelopeAlgorithms = {"RSA-OAEP-SHA256"};
  request.setUserEncryptionCertificate(advertisement);
  const auto before = detail::makeProtectedTargetedBindingSelection(user, provider, service, id, request);
  // Independent top-level field inventory and order, rather than asking the
  // production normalization helper to calculate its own expected transcript.
  ndn::Block normalized(128);
  normalized.push_back(ndn::makeStringBlock(170, "user-token"));
  normalized.push_back(ndn::makeStringBlock(171, "provider-token"));
  normalized.push_back(ndn::Block(151));
  normalized.push_back(ndn::makeNonNegativeIntegerBlock(155, tlv::FirstResponding));
  normalized.push_back(ndn::makeNonNegativeIntegerBlock(189, tlv::TargetedRequest));
  normalized.push_back(ndn::makeStringBlock(158, "/provider"));
  normalized.push_back(request.getControllerVersion().wireEncode());
  normalized.push_back(advertisement.WireEncode());
  ndn::Block expectedCaps(0xF61B), field(0xF618);
  expectedCaps.push_back(ndn::makeNonNegativeIntegerBlock(163, 1));
  field.push_back(ndn::makeStringBlock(0xF619, "RequestScopedConfidentialityV1"));
  field.push_back(ndn::makeStringBlock(0xF61A, "required"));
  expectedCaps.push_back(field);
  normalized.push_back(expectedCaps);
  normalized.encode();
  const std::string binaryDomain = "NDNSF-TARGETED-REQUEST-SCOPED-KEY-BINARY-V1:";
  ndn::Buffer expectedBinary(binaryDomain.begin(), binaryDomain.end());
  expectedBinary.insert(expectedBinary.end(), normalized.begin(), normalized.end());
  BOOST_CHECK(before.getAssignmentPayload() == expectedBinary);
  auto caps = request.getRequestCapabilities();
  caps.setField(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD, "wrapped-key-bytes");
  request.setRequestCapabilities(caps);
  const auto filled = detail::makeProtectedTargetedBindingSelection(user, provider, service, id, request);
  BOOST_CHECK(before.WireEncode() == filled.WireEncode());

  caps.Clear();
  caps.setField("RequestScopedConfidentialityV1", "required");
  caps.setField(detail::TARGETED_KEY_ENVELOPE_FIELD, "00");
  request.setRequestCapabilities(caps);
  const auto legacy = detail::makeProtectedTargetedBindingSelection(user, provider, service, id, request);
  const std::string legacyDomain = "NDNSF-TARGETED-REQUEST-SCOPED-KEY-V1:";
  ndn::Buffer expectedLegacy(legacyDomain.begin(), legacyDomain.end());
  expectedLegacy.insert(expectedLegacy.end(), normalized.begin(), normalized.end());
  BOOST_CHECK(legacy.getAssignmentPayload() == expectedLegacy);
  BOOST_CHECK(!(before.WireEncode() == legacy.WireEncode()));
  caps.Clear();
  caps.setField("RequestScopedConfidentialityV1", "required");
  caps.setField(detail::TARGETED_COMPACT_KEY_ENVELOPE_FIELD, "");
  request.setRequestCapabilities(caps);
  const auto compact = detail::makeProtectedTargetedBindingSelection(user, provider, service, id, request);
  const std::string compactDomain = "NDNSF-TARGETED-REQUEST-SCOPED-KEY-COMPACT-V1:";
  ndn::Buffer expectedCompact(compactDomain.begin(), compactDomain.end());
  expectedCompact.insert(expectedCompact.end(), normalized.begin(), normalized.end());
  BOOST_CHECK(compact.getAssignmentPayload() == expectedCompact);
  BOOST_CHECK(compact.hasCompactKeyEnvelopeMode());
  BOOST_CHECK(!(compact.WireEncode() == before.WireEncode()));
  BOOST_CHECK(!(compact.WireEncode() == legacy.WireEncode()));
  caps.setField(detail::TARGETED_COMPACT_KEY_ENVELOPE_FIELD, "wrapped-key-bytes");
  request.setRequestCapabilities(caps);
  BOOST_CHECK(compact.WireEncode() == detail::makeProtectedTargetedBindingSelection(
    user, provider, service, id, request).WireEncode());

  // Independently replace only the advertisement field with a Name reference.
  // Resolving its local metadata must never alter authenticated wire bytes.
  ndn::Name profileName("/user/NDNSF/SECURITY");
  profileName.append("name=encryption-profile");
  const ndn::Buffer profileDigest(32, 0x42);
  profileName.append(ndn::name::Component(profileDigest));
  request.setUserSecurityProfile(profileName);
  ndn::Block normalizedReference(128);
  normalized.parse();
  for (const auto& entry : normalized.elements()) {
    if (entry.type() == advertisement.WireEncode().type()) {
      ndn::Block reference(0xF855);
      reference.push_back(profileName.wireEncode());
      reference.encode();
      normalizedReference.push_back(reference);
    }
    else normalizedReference.push_back(entry);
  }
  normalizedReference.encode();
  ndn::Buffer expectedReference(compactDomain.begin(), compactDomain.end());
  expectedReference.insert(expectedReference.end(), normalizedReference.begin(), normalizedReference.end());
  const auto unresolvedWire = request.WireEncode();
  BOOST_CHECK_THROW(detail::makeProtectedTargetedBindingSelection(
    user, provider, service, id, request), std::invalid_argument);
  request.resolveUserSecurityProfile(
    std::make_shared<const EncryptionCertificateAdvertisement>(advertisement));
  BOOST_CHECK(request.WireEncode() == unresolvedWire);
  const auto resolved = detail::makeProtectedTargetedBindingSelection(user, provider, service, id, request);
  BOOST_CHECK(resolved.getAssignmentPayload() == expectedReference);
  BOOST_CHECK(resolved.hasCompactKeyEnvelopeMode());
  request.setUserToken("different-token");
  const auto changed = detail::makeProtectedTargetedBindingSelection(user, provider, service, id, request);
  BOOST_CHECK(!(resolved.WireEncode() == changed.WireEncode()));
  BOOST_CHECK_THROW(detail::makeProtectedTargetedBindingSelection(user, ndn::Name("/other"),
    service, id, request), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(BinaryCarrierRetainsRecipientAndFullRequestBindingChecks)
{
  ndn::security::KeyChain keyChain("pib-memory:spec205-carrier", "tpm-memory:spec205-carrier");
  const auto identity = keyChain.createIdentity(ndn::Name("/provider"), ndn::RsaKeyParams(2048));
  const auto certificate = identity.getDefaultKey().getDefaultCertificate();
  const auto publicKey = certificate.getPublicKey();
  auto binding = makeCarrierEnvelope().binding;
  binding.providerEncryptionCertName = certificate.getName();
  const auto keys = generateRequestKeyBundle(1000, 2000);
  const auto wrapped = wrapSelectionKeyEnvelope(keys, binding,
    ndn::span<const uint8_t>(publicKey.data(), publicKey.size()), 1500);
  const auto wire = wrapped.wireEncode();
  const auto request = makeRequest(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD,
    std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
  SelectionKeyEnvelope decoded;
  BOOST_REQUIRE(decoded.wireDecode(detail::decodeProtectedTargetedKeyEnvelope(request)));
  RequestKeyBundle recovered;
  BOOST_REQUIRE(unwrapSelectionKeyEnvelope(decoded, binding, certificate.getName(), keyChain,
    1500, recovered));
  BOOST_CHECK(recovered.inputKey == keys.inputKey);
  for (int variant = 0; variant < 8; ++variant) {
    auto wrong = binding;
    switch (variant) {
      case 0: wrong.requestId.append("other"); break;
      case 1: ++wrong.attempt; break;
      case 2: ++wrong.controllerVersion.controllerEpoch; break;
      case 3: wrong.userEncryptionCertName.append("other"); break;
      case 4: wrong.providerEncryptionCertName.append("other"); break;
      case 5: wrong.serviceName.append("other"); break;
      case 6: wrong.inputDataName.append("other"); break;
      case 7: wrong.selectionDigest = "sha256:" + std::string(64, '4'); break;
    }
    BOOST_CHECK(!unwrapSelectionKeyEnvelope(decoded, wrong, certificate.getName(), keyChain,
      1500, recovered));
  }
  BOOST_CHECK(!unwrapSelectionKeyEnvelope(decoded, binding, ndn::Name("/other/KEY/key"),
    keyChain, 1500, recovered));
  BOOST_CHECK(!unwrapSelectionKeyEnvelope(decoded, binding, certificate.getName(), keyChain,
    2000, recovered));
}

BOOST_AUTO_TEST_CASE(CompactEnvelopeReconstructsFullBindingBeforeRsaUnwrap)
{
  ndn::security::KeyChain chain("pib-memory:spec205-compact", "tpm-memory:spec205-compact");
  const auto cert = chain.createIdentity(ndn::Name("/provider"), ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  auto binding = makeCarrierEnvelope().binding;
  binding.providerEncryptionCertName = cert.getName();
  const auto keys = generateRequestKeyBundle(1000, 2000);
  auto envelope = wrapSelectionKeyEnvelope(keys, binding, cert.getPublicKey(), 1500);
  const auto fullSize = envelope.wireEncode().size();
  envelope.useCompactBinding();
  const auto wire = envelope.wireEncode();
  BOOST_CHECK_GT(fullSize, wire.size() + 200);
  wire.parse();
  BOOST_REQUIRE_EQUAL(wire.elements().size(), 5);
  BOOST_CHECK_EQUAL(wire.elements()[0].type(), 0xF856);
  BOOST_CHECK_EQUAL(wire.elements()[1].type(), 0xF857);
  const auto aad = binding.canonicalAad();
  ndn::Buffer independentDigest(SHA256_DIGEST_LENGTH);
  SHA256(aad.data(), aad.size(), independentDigest.data());
  BOOST_CHECK(envelope.bindingDigest == independentDigest);
  const auto request = makeRequest(detail::TARGETED_COMPACT_KEY_ENVELOPE_FIELD,
    std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
  SelectionKeyEnvelope decoded;
  BOOST_REQUIRE(decoded.wireDecode(detail::decodeProtectedTargetedKeyEnvelope(request)));
  BOOST_CHECK(!decoded.binding.isValid());
  RequestKeyBundle recovered;
  BOOST_REQUIRE(unwrapSelectionKeyEnvelope(decoded, binding, cert.getName(), chain, 1500, recovered));
  BOOST_CHECK(recovered.inputKey == keys.inputKey);
  BOOST_CHECK(recovered.responseKey == keys.responseKey);
  for (int mutation = 0; mutation < 9; ++mutation) {
    auto wrong = binding;
    switch (mutation) {
    case 0: wrong.serviceName.append("wrong"); break;
    case 1: wrong.requestId.append("wrong"); break;
    case 2: ++wrong.attempt; break;
    case 3: ++wrong.controllerVersion.controllerEpoch; break;
    case 4: wrong.userEncryptionCertDigest = "sha256:" + std::string(64, 'a'); break;
    case 5: wrong.providerEncryptionCertName.append("wrong"); break;
    case 6: wrong.providerEncryptionCertDigest = "sha256:" + std::string(64, 'b'); break;
    case 7: wrong.selectionDigest = "sha256:" + std::string(64, 'c'); break;
    case 8: wrong.inputDataName.append("wrong"); break;
    }
    RequestCryptoFailure failure = RequestCryptoFailure::NONE;
    BOOST_CHECK(!unwrapSelectionKeyEnvelope(decoded, wrong, cert.getName(), chain, 1500, recovered, &failure));
    BOOST_CHECK(failure == RequestCryptoFailure::INVALID_BINDING);
  }
  BOOST_CHECK(!unwrapSelectionKeyEnvelope(decoded, binding, cert.getName(), chain, 999, recovered));
  BOOST_CHECK(!unwrapSelectionKeyEnvelope(decoded, binding, cert.getName(), chain, 2000, recovered));
  const auto other = chain.createIdentity(ndn::Name("/other"), ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  BOOST_CHECK(!unwrapSelectionKeyEnvelope(decoded, binding, other.getName(), chain, 1500, recovered));
}

BOOST_AUTO_TEST_CASE(CompactSchemaRejectsAmbiguousCarrierAndMalformedFields)
{
  auto envelope = makeCarrierEnvelope();
  const auto full = envelope.wireEncode();
  envelope.useCompactBinding();
  const auto compact = envelope.wireEncode();
  const auto bytes = [](const auto& wire) {
    return std::string(reinterpret_cast<const char*>(wire.data()), wire.size());
  };
  BOOST_CHECK_THROW(detail::decodeProtectedTargetedKeyEnvelope(
    makeRequest(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD, bytes(compact))), std::invalid_argument);
  BOOST_CHECK_THROW(detail::decodeProtectedTargetedKeyEnvelope(
    makeRequest(detail::TARGETED_COMPACT_KEY_ENVELOPE_FIELD, bytes(full))), std::invalid_argument);
  auto request = makeRequest(detail::TARGETED_COMPACT_KEY_ENVELOPE_FIELD, bytes(compact));
  auto caps = request.getRequestCapabilities();
  caps.setField(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD, bytes(full));
  request.setRequestCapabilities(caps);
  BOOST_CHECK_THROW(detail::decodeProtectedTargetedKeyEnvelope(request), std::invalid_argument);
  compact.parse();
  for (int variant = 0; variant < 5; ++variant) {
    auto fields = compact.elements();
    if (variant == 0) std::swap(fields[0], fields[1]);
    if (variant == 1) fields.push_back(fields[0]);
    if (variant == 2) fields[0] = ndn::makeNonNegativeIntegerBlock(0xF856, 2);
    if (variant == 3) fields[1] = ndn::makeBinaryBlock(0xF857, ndn::Buffer(31, 0));
    if (variant == 4) fields[2] = ndn::makeBinaryBlock(0xF833, ndn::Buffer{});
    ndn::Block malformed(SelectionKeyEnvelope::TYPE);
    for (const auto& field : fields) malformed.push_back(field);
    malformed.encode();
    SelectionKeyEnvelope rejected;
    BOOST_CHECK(!rejected.wireDecode(malformed));
  }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
