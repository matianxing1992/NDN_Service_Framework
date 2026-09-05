#include "RequestConfidentiality.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>

namespace ndn_service_framework {
namespace {

enum : uint32_t {
  BindingServiceName = 0xF811,
  BindingRequestId = 0xF812,
  BindingAttempt = 0xF813,
  BindingUserCertName = 0xF814,
  BindingUserCertDigest = 0xF815,
  BindingProviderCertName = 0xF816,
  BindingProviderCertDigest = 0xF817,
  BindingSelectionDigest = 0xF818,
  BindingInputDataName = 0xF819,
  BindingSegmentOrEvent = 0xF81A,
  EnvelopeAlgorithm = 0xF831,
  EnvelopeBinding = 0xF832,
  EnvelopeWrappedKeys = 0xF833,
  EnvelopeCreatedAt = 0xF834,
  EnvelopeExpiresAt = 0xF835,
  AeadAlgorithm = 0xF821,
  AeadKeyId = 0xF822,
  AeadNonce = 0xF823,
  AeadCiphertext = 0xF824,
  AeadTag = 0xF825,
  AeadAadDigest = 0xF826,
  AeadSegmentOrEvent = 0xF827,
  KeyBundleType = 0xF840,
  KeyBundleInput = 0xF841,
  KeyBundleResponse = 0xF842,
  KeyBundleId = 0xF843,
  KeyBundleCreatedAt = 0xF844,
  KeyBundleExpiresAt = 0xF845,
};

ndn::Block
makeNameField(uint32_t type, const ndn::Name& name)
{
  if (name.empty()) {
    throw std::invalid_argument("request binding name field is empty");
  }
  ndn::Block field(type);
  field.push_back(name.wireEncode());
  field.encode();
  return field;
}

bool
decodeNameField(const ndn::Block& field, ndn::Name& name)
{
  try {
    // Blocks obtained from a nested opaque container are not guaranteed to
    // have parsed their child elements yet.  Parse the field before checking
    // its single Name child; otherwise a valid SelectionKeyEnvelope becomes
    // undecodable after the ServiceSelectionMessage round trip.
    field.parse();
    const auto& elements = field.elements();
    if (elements.size() != 1 || elements.front().type() != ndn::tlv::Name) {
      return false;
    }
    name.wireDecode(elements.front());
    return !name.empty();
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
decodeString(const ndn::Block& block, std::string& out)
{
  try {
    out.assign(reinterpret_cast<const char*>(block.value()), block.value_size());
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
decodeBuffer(const ndn::Block& block, ndn::Buffer& out)
{
  out = ndn::Buffer(block.value(), block.value_size());
  return true;
}

bool
decodeUint64(const ndn::Block& block, uint64_t& out)
{
  try {
    out = ndn::readNonNegativeInteger(block);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
isHexDigest(const std::string& value)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) {
    return false;
  }
  return std::all_of(value.begin() + 7, value.end(), [] (unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

std::string
hexEncode(ndn::span<const uint8_t> bytes)
{
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (const auto byte : bytes) {
    stream << std::setw(2) << static_cast<unsigned>(byte);
  }
  return stream.str();
}

ndn::Buffer
randomBytes(size_t size)
{
  ndn::Buffer output(size);
  if (size == 0 || RAND_bytes(output.data(), static_cast<int>(size)) != 1) {
    throw std::runtime_error("request-scoped random generation failed");
  }
  return output;
}

ndn::Buffer
sha256(ndn::span<const uint8_t> bytes)
{
  ndn::Buffer output(SHA256_DIGEST_LENGTH);
  SHA256(bytes.data(), bytes.size(), output.data());
  return output;
}

bool
sameBlock(const ndn::Block& left, const ndn::Block& right)
{
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

ndn::Block
encodeKeyBundle(const RequestKeyBundle& keys)
{
  ndn::Block block(KeyBundleType);
  block.push_back(ndn::makeBinaryBlock(KeyBundleInput, keys.inputKey.begin(),
                                       keys.inputKey.end()));
  block.push_back(ndn::makeBinaryBlock(KeyBundleResponse, keys.responseKey.begin(),
                                       keys.responseKey.end()));
  block.push_back(ndn::makeStringBlock(KeyBundleId, keys.keyId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(KeyBundleCreatedAt,
                                                     keys.createdAtMs));
  block.push_back(ndn::makeNonNegativeIntegerBlock(KeyBundleExpiresAt,
                                                     keys.expiresAtMs));
  block.encode();
  return block;
}

bool
decodeKeyBundle(const ndn::Block& block, RequestKeyBundle& keys)
{
  if (block.type() != KeyBundleType) {
    return false;
  }
  RequestKeyBundle decoded;
  bool inputSeen = false;
  bool responseSeen = false;
  bool idSeen = false;
  bool createdSeen = false;
  bool expiresSeen = false;
  try {
    block.parse();
    for (const auto& field : block.elements()) {
      switch (field.type()) {
      case KeyBundleInput:
        if (inputSeen) return false;
        inputSeen = true;
        decodeBuffer(field, decoded.inputKey);
        break;
      case KeyBundleResponse:
        if (responseSeen) return false;
        responseSeen = true;
        decodeBuffer(field, decoded.responseKey);
        break;
      case KeyBundleId:
        if (idSeen) return false;
        idSeen = true;
        decodeString(field, decoded.keyId);
        break;
      case KeyBundleCreatedAt:
        if (createdSeen || !decodeUint64(field, decoded.createdAtMs)) return false;
        createdSeen = true;
        break;
      case KeyBundleExpiresAt:
        if (expiresSeen || !decodeUint64(field, decoded.expiresAtMs)) return false;
        expiresSeen = true;
        break;
      default:
        return false;
      }
    }
  }
  catch (const std::exception&) {
    return false;
  }
  if (!inputSeen || !responseSeen || !idSeen || !createdSeen || !expiresSeen ||
      decoded.inputKey.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE ||
      decoded.responseKey.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE ||
      decoded.keyId.empty() || decoded.expiresAtMs <= decoded.createdAtMs) {
    return false;
  }
  keys = std::move(decoded);
  return true;
}

void
setFailure(RequestCryptoFailure* failure, RequestCryptoFailure value)
{
  if (failure != nullptr) {
    *failure = value;
  }
}

} // namespace

bool
RequestSecurityBinding::isValid() const
{
  const bool providerPair = providerEncryptionCertName.empty() &&
                             providerEncryptionCertDigest.empty();
  const bool providerComplete = !providerEncryptionCertName.empty() &&
                                isHexDigest(providerEncryptionCertDigest);
  const bool selectionComplete = selectionDigest.empty() ||
                                 isHexDigest(selectionDigest);
  return !serviceName.empty() && !requestId.empty() && attempt > 0 &&
         controllerVersion.isValid() && !userEncryptionCertName.empty() &&
         isHexDigest(userEncryptionCertDigest) &&
         (providerPair || providerComplete) && selectionComplete;
}

ndn::Block
RequestSecurityBinding::wireEncode() const
{
  if (!isValid()) {
    throw std::invalid_argument("invalid RequestSecurityBinding");
  }
  ndn::Block block(TYPE);
  block.push_back(makeNameField(BindingServiceName, serviceName));
  block.push_back(makeNameField(BindingRequestId, requestId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(BindingAttempt, attempt));
  block.push_back(controllerVersion.wireEncode());
  block.push_back(makeNameField(BindingUserCertName, userEncryptionCertName));
  block.push_back(ndn::makeStringBlock(BindingUserCertDigest,
                                       userEncryptionCertDigest));
  if (!providerEncryptionCertName.empty()) {
    block.push_back(makeNameField(BindingProviderCertName,
                                  providerEncryptionCertName));
    block.push_back(ndn::makeStringBlock(BindingProviderCertDigest,
                                         providerEncryptionCertDigest));
  }
  if (!selectionDigest.empty()) {
    block.push_back(ndn::makeStringBlock(BindingSelectionDigest, selectionDigest));
  }
  if (!inputDataName.empty()) {
    block.push_back(makeNameField(BindingInputDataName, inputDataName));
  }
  if (!segmentOrEventId.empty()) {
    block.push_back(ndn::makeStringBlock(BindingSegmentOrEvent, segmentOrEventId));
  }
  block.encode();
  return block;
}

bool
RequestSecurityBinding::wireDecode(const ndn::Block& block)
{
  if (block.type() != TYPE) {
    return false;
  }
  RequestSecurityBinding decoded;
  bool versionSeen = false;
  bool attemptSeen = false;
  std::set<uint32_t> seen;
  try {
    block.parse();
    for (const auto& field : block.elements()) {
      if (!seen.insert(field.type()).second) {
        return false;
      }
      switch (field.type()) {
      case BindingServiceName:
        if (!decodeNameField(field, decoded.serviceName)) {
          return false;
        }
        break;
      case BindingRequestId:
        if (!decodeNameField(field, decoded.requestId)) {
          return false;
        }
        break;
      case BindingAttempt:
        if (attemptSeen || !decodeUint64(field, decoded.attempt)) {
          return false;
        }
        attemptSeen = true;
        break;
      case ControllerVersion::TYPE:
        if (versionSeen || !decoded.controllerVersion.wireDecode(field)) {
          return false;
        }
        versionSeen = true;
        break;
      case BindingUserCertName:
        if (!decodeNameField(field, decoded.userEncryptionCertName)) {
          return false;
        }
        break;
      case BindingUserCertDigest:
        if (!decodeString(field, decoded.userEncryptionCertDigest)) {
          return false;
        }
        break;
      case BindingProviderCertName:
        if (!decodeNameField(field, decoded.providerEncryptionCertName)) {
          return false;
        }
        break;
      case BindingProviderCertDigest:
        if (!decodeString(field, decoded.providerEncryptionCertDigest)) {
          return false;
        }
        break;
      case BindingSelectionDigest:
        if (!decodeString(field, decoded.selectionDigest)) {
          return false;
        }
        break;
      case BindingInputDataName:
        if (!decodeNameField(field, decoded.inputDataName)) {
          return false;
        }
        break;
      case BindingSegmentOrEvent:
        if (!decodeString(field, decoded.segmentOrEventId)) {
          return false;
        }
        break;
      default:
        return false;
      }
    }
  }
  catch (const std::exception&) {
    return false;
  }
  if (!attemptSeen || !versionSeen || !decoded.isValid()) {
    return false;
  }
  *this = std::move(decoded);
  return true;
}

ndn::Buffer
RequestSecurityBinding::canonicalAad() const
{
  const auto wire = wireEncode();
  return ndn::Buffer(wire.data(), wire.size());
}

std::string
RequestSecurityBinding::digestHex() const
{
  const auto aad = canonicalAad();
  const auto digest = sha256(ndn::span<const uint8_t>(aad.data(), aad.size()));
  return "sha256:" + hexEncode(ndn::span<const uint8_t>(digest.data(), digest.size()));
}

bool
RequestKeyBundle::isValid(uint64_t nowMs) const
{
  return inputKey.size() == HybridMessageCrypto::MESSAGE_KEY_SIZE &&
         responseKey.size() == HybridMessageCrypto::MESSAGE_KEY_SIZE &&
         !keyId.empty() && createdAtMs > 0 && expiresAtMs > createdAtMs &&
         nowMs < expiresAtMs && !consumed;
}

void
RequestKeyBundle::zeroize() noexcept
{
  auto clear = [] (ndn::Buffer& value) {
    volatile uint8_t* ptr = value.data();
    for (size_t i = 0; i < value.size(); ++i) {
      ptr[i] = 0;
    }
    value.clear();
  };
  clear(inputKey);
  clear(responseKey);
  keyId.clear();
  createdAtMs = 0;
  expiresAtMs = 0;
  consumed = true;
}

bool
SelectionKeyEnvelope::isValid(uint64_t nowMs) const
{
  return binding.isValid() && envelopeAlgorithm == "RSA-OAEP-SHA256" &&
         !wrappedKeys.empty() && createdAtMs > 0 && expiresAtMs > createdAtMs &&
         nowMs < expiresAtMs;
}

ndn::Block
SelectionKeyEnvelope::wireEncode() const
{
  if (!isValid(createdAtMs)) {
    throw std::invalid_argument("invalid SelectionKeyEnvelope");
  }
  ndn::Block block(TYPE);
  block.push_back(binding.wireEncode());
  block.push_back(ndn::makeStringBlock(EnvelopeAlgorithm, envelopeAlgorithm));
  block.push_back(ndn::makeBinaryBlock(EnvelopeWrappedKeys,
                                       wrappedKeys.begin(), wrappedKeys.end()));
  block.push_back(ndn::makeNonNegativeIntegerBlock(EnvelopeCreatedAt, createdAtMs));
  block.push_back(ndn::makeNonNegativeIntegerBlock(EnvelopeExpiresAt, expiresAtMs));
  block.encode();
  return block;
}

bool
SelectionKeyEnvelope::wireDecode(const ndn::Block& block)
{
  if (block.type() != TYPE) return false;
  SelectionKeyEnvelope decoded;
  bool bindingSeen = false;
  bool algorithmSeen = false;
  bool wrappedSeen = false;
  bool createdSeen = false;
  bool expiresSeen = false;
  try {
    block.parse();
    for (const auto& field : block.elements()) {
      switch (field.type()) {
      case RequestSecurityBinding::TYPE:
        if (bindingSeen || !decoded.binding.wireDecode(field)) return false;
        bindingSeen = true;
        break;
      case EnvelopeAlgorithm:
        if (algorithmSeen || !decodeString(field, decoded.envelopeAlgorithm)) return false;
        algorithmSeen = true;
        break;
      case EnvelopeWrappedKeys:
        if (wrappedSeen || !decodeBuffer(field, decoded.wrappedKeys)) return false;
        wrappedSeen = true;
        break;
      case EnvelopeCreatedAt:
        if (createdSeen || !decodeUint64(field, decoded.createdAtMs)) return false;
        createdSeen = true;
        break;
      case EnvelopeExpiresAt:
        if (expiresSeen || !decodeUint64(field, decoded.expiresAtMs)) return false;
        expiresSeen = true;
        break;
      default:
        return false;
      }
    }
  }
  catch (const std::exception&) {
    return false;
  }
  if (!bindingSeen || !algorithmSeen || !wrappedSeen || !createdSeen || !expiresSeen ||
      decoded.envelopeAlgorithm != "RSA-OAEP-SHA256" || decoded.wrappedKeys.empty() ||
      decoded.expiresAtMs <= decoded.createdAtMs) return false;
  *this = std::move(decoded);
  return true;
}

bool
AeadEnvelope::isValid() const
{
  return algorithm == "AES-256-GCM" && !keyId.empty() &&
         nonce.size() == HybridMessageCrypto::NONCE_SIZE &&
         tag.size() == HybridMessageCrypto::TAG_SIZE &&
         aadDigest.size() == SHA256_DIGEST_LENGTH &&
         !segmentOrEventId.empty();
}

ndn::Block
AeadEnvelope::wireEncode() const
{
  if (!isValid()) throw std::invalid_argument("invalid AeadEnvelope");
  ndn::Block block(TYPE);
  block.push_back(ndn::makeStringBlock(AeadAlgorithm, algorithm));
  block.push_back(ndn::makeStringBlock(AeadKeyId, keyId));
  block.push_back(ndn::makeBinaryBlock(AeadNonce, nonce.begin(), nonce.end()));
  block.push_back(ndn::makeBinaryBlock(AeadCiphertext, ciphertext.begin(), ciphertext.end()));
  block.push_back(ndn::makeBinaryBlock(AeadTag, tag.begin(), tag.end()));
  block.push_back(ndn::makeBinaryBlock(AeadAadDigest, aadDigest.begin(), aadDigest.end()));
  block.push_back(ndn::makeStringBlock(AeadSegmentOrEvent, segmentOrEventId));
  block.encode();
  return block;
}

bool
AeadEnvelope::wireDecode(const ndn::Block& block)
{
  if (block.type() != TYPE) return false;
  AeadEnvelope decoded;
  std::set<uint32_t> seen;
  try {
    block.parse();
    for (const auto& field : block.elements()) {
      if (!seen.insert(field.type()).second) return false;
      switch (field.type()) {
      case AeadAlgorithm: if (!decodeString(field, decoded.algorithm)) return false; break;
      case AeadKeyId: if (!decodeString(field, decoded.keyId)) return false; break;
      case AeadNonce: if (!decodeBuffer(field, decoded.nonce)) return false; break;
      case AeadCiphertext: if (!decodeBuffer(field, decoded.ciphertext)) return false; break;
      case AeadTag: if (!decodeBuffer(field, decoded.tag)) return false; break;
      case AeadAadDigest: if (!decodeBuffer(field, decoded.aadDigest)) return false; break;
      case AeadSegmentOrEvent:
        if (!decodeString(field, decoded.segmentOrEventId)) return false;
        break;
      default: return false;
      }
    }
  }
  catch (const std::exception&) { return false; }
  if (!decoded.isValid()) return false;
  *this = std::move(decoded);
  return true;
}

const char*
requestCryptoFailureName(RequestCryptoFailure failure) noexcept
{
  switch (failure) {
  case RequestCryptoFailure::NONE: return "none";
  case RequestCryptoFailure::INVALID_BINDING: return "invalid_binding";
  case RequestCryptoFailure::INVALID_KEY_BUNDLE: return "invalid_key_bundle";
  case RequestCryptoFailure::EXPIRED_KEY: return "expired_key";
  case RequestCryptoFailure::WRONG_KEY_ID: return "wrong_key_id";
  case RequestCryptoFailure::INVALID_ENVELOPE: return "invalid_envelope";
  case RequestCryptoFailure::AUTHENTICATION_FAILED: return "authentication_failed";
  case RequestCryptoFailure::NONCE_REUSE: return "nonce_reuse";
  case RequestCryptoFailure::WRONG_RECIPIENT: return "wrong_recipient";
  }
  return "unknown";
}

RequestKeyBundle
generateRequestKeyBundle(uint64_t nowMs, uint64_t expiresAtMs)
{
  if (nowMs == 0 || expiresAtMs <= nowMs) {
    throw std::invalid_argument("invalid request key lifetime");
  }
  RequestKeyBundle keys;
  keys.inputKey = randomBytes(HybridMessageCrypto::MESSAGE_KEY_SIZE);
  keys.responseKey = randomBytes(HybridMessageCrypto::MESSAGE_KEY_SIZE);
  keys.keyId = hexEncode(randomBytes(16));
  keys.createdAtMs = nowMs;
  keys.expiresAtMs = expiresAtMs;
  return keys;
}

SelectionKeyEnvelope
wrapSelectionKeyEnvelope(const RequestKeyBundle& keys,
                         const RequestSecurityBinding& binding,
                         ndn::span<const uint8_t> recipientPublicKey,
                         uint64_t nowMs)
{
  if (!keys.isValid(nowMs)) throw std::invalid_argument("invalid request key bundle");
  if (!binding.isValid()) throw std::invalid_argument("invalid request binding");
  if (binding.providerEncryptionCertName.empty())
    throw std::invalid_argument("selection envelope requires provider certificate");
  const auto encoded = encodeKeyBundle(keys);
  SelectionKeyEnvelope result;
  result.binding = binding;
  result.createdAtMs = keys.createdAtMs;
  result.expiresAtMs = keys.expiresAtMs;
  result.wrappedKeys = wrapSelectionGatedInputKey(
      ndn::Buffer(encoded.data(), encoded.size()), recipientPublicKey);
  return result;
}

bool
unwrapSelectionKeyEnvelope(const SelectionKeyEnvelope& envelope,
                           const RequestSecurityBinding& expectedBinding,
                           const ndn::Name& recipientCertificateName,
                           const ndn::security::KeyChain& keyChain,
                           uint64_t nowMs,
                           RequestKeyBundle& keys,
                           RequestCryptoFailure* failure)
{
  setFailure(failure, RequestCryptoFailure::NONE);
  if (!envelope.binding.isValid() ||
      envelope.envelopeAlgorithm != "RSA-OAEP-SHA256" ||
      envelope.wrappedKeys.empty() || envelope.createdAtMs == 0 ||
      envelope.expiresAtMs <= envelope.createdAtMs) {
    setFailure(failure, RequestCryptoFailure::INVALID_ENVELOPE);
    return false;
  }
  if (nowMs < envelope.createdAtMs || nowMs >= envelope.expiresAtMs) {
    setFailure(failure, RequestCryptoFailure::EXPIRED_KEY);
    return false;
  }
  if (!expectedBinding.isValid() ||
      !sameBlock(envelope.binding.wireEncode(), expectedBinding.wireEncode())) {
    setFailure(failure, RequestCryptoFailure::INVALID_BINDING);
    return false;
  }
  ndn::Buffer decodedWire;
  try {
    decodedWire = unwrapSelectionGatedInputKey(envelope.wrappedKeys,
                                                recipientCertificateName, keyChain);
  }
  catch (const std::exception&) {
    setFailure(failure, RequestCryptoFailure::WRONG_RECIPIENT);
    return false;
  }
  try {
    auto parsed = ndn::Block::fromBuffer(decodedWire);
    if (!std::get<0>(parsed) || !decodeKeyBundle(std::get<1>(parsed), keys)) {
      setFailure(failure, RequestCryptoFailure::INVALID_KEY_BUNDLE);
      return false;
    }
  }
  catch (const std::exception&) {
    setFailure(failure, RequestCryptoFailure::INVALID_KEY_BUNDLE);
    return false;
  }
  if (!keys.isValid(nowMs) || keys.createdAtMs != envelope.createdAtMs ||
      keys.expiresAtMs != envelope.expiresAtMs) {
    keys.zeroize();
    setFailure(failure, RequestCryptoFailure::EXPIRED_KEY);
    return false;
  }
  return true;
}

AeadEnvelope
encryptRequestContent(const ndn::Buffer& key,
                      const std::string& keyId,
                      const RequestSecurityBinding& binding,
                      ndn::span<const uint8_t> plaintext)
{
  if (key.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE || keyId.empty() ||
      !binding.isValid() || binding.segmentOrEventId.empty()) {
    throw std::invalid_argument("invalid request content encryption input");
  }
  const auto aad = binding.canonicalAad();
  const auto encrypted = hybridAesGcmEncrypt(
      key, plaintext, ndn::span<const uint8_t>(aad.data(), aad.size()));
  AeadEnvelope result;
  result.keyId = keyId;
  result.nonce = encrypted.nonce;
  result.ciphertext = encrypted.ciphertext;
  result.tag = encrypted.tag;
  result.aadDigest = sha256(ndn::span<const uint8_t>(aad.data(), aad.size()));
  result.segmentOrEventId = binding.segmentOrEventId;
  return result;
}

bool
decryptRequestContent(const ndn::Buffer& key,
                      const std::string& expectedKeyId,
                      const RequestSecurityBinding& binding,
                      const AeadEnvelope& envelope,
                      ndn::Buffer& plaintext,
                      RequestCryptoFailure* failure)
{
  setFailure(failure, RequestCryptoFailure::NONE);
  plaintext.clear();
  if (!binding.isValid()) {
    setFailure(failure, RequestCryptoFailure::INVALID_BINDING);
    return false;
  }
  if (!envelope.isValid()) {
    setFailure(failure, RequestCryptoFailure::INVALID_ENVELOPE);
    return false;
  }
  if (expectedKeyId.empty() || envelope.keyId != expectedKeyId) {
    setFailure(failure, RequestCryptoFailure::WRONG_KEY_ID);
    return false;
  }
  if (envelope.segmentOrEventId != binding.segmentOrEventId) {
    setFailure(failure, RequestCryptoFailure::INVALID_BINDING);
    return false;
  }
  const auto aad = binding.canonicalAad();
  const auto expectedDigest = sha256(ndn::span<const uint8_t>(aad.data(), aad.size()));
  if (envelope.aadDigest.size() != expectedDigest.size() ||
      !std::equal(envelope.aadDigest.begin(), envelope.aadDigest.end(),
                  expectedDigest.begin())) {
    setFailure(failure, RequestCryptoFailure::INVALID_BINDING);
    return false;
  }
  HybridMessageEnvelope legacy;
  legacy.setNonce(envelope.nonce);
  legacy.setCipherText(envelope.ciphertext);
  legacy.setAuthTag(envelope.tag);
  if (!hybridAesGcmDecrypt(key, legacy,
                          ndn::span<const uint8_t>(aad.data(), aad.size()), plaintext)) {
    setFailure(failure, RequestCryptoFailure::AUTHENTICATION_FAILED);
    return false;
  }
  return true;
}

std::string
NonceRegistry::nonceKey(ndn::span<const uint8_t> nonce)
{
  return std::string(reinterpret_cast<const char*>(nonce.data()), nonce.size());
}

bool
NonceRegistry::reserve(const std::string& keyId,
                       ndn::span<const uint8_t> nonce,
                       std::chrono::steady_clock::time_point expiresAt)
{
  if (keyId.empty() || nonce.size() != NONCE_SIZE ||
      expiresAt <= std::chrono::steady_clock::now()) return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto now = std::chrono::steady_clock::now();
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    if (it->second.expiresAt <= now) it = m_entries.erase(it);
    else ++it;
  }
  auto& entry = m_entries[keyId];
  if (entry.expiresAt < expiresAt) entry.expiresAt = expiresAt;
  return entry.nonces.insert(nonceKey(nonce)).second;
}

bool
NonceRegistry::reserve(const std::string& keyId,
                       ndn::span<const uint8_t> nonce,
                       std::chrono::milliseconds lifetime)
{
  if (lifetime.count() <= 0) return false;
  return reserve(keyId, nonce, std::chrono::steady_clock::now() + lifetime);
}

bool
NonceRegistry::reserveBatch(const std::string& keyId,
                             const std::vector<ndn::Buffer>& nonces,
                             std::chrono::milliseconds lifetime)
{
  if (keyId.empty() || nonces.empty() || lifetime.count() <= 0)
    return false;
  for (const auto& nonce : nonces) {
    if (nonce.size() != NONCE_SIZE)
      return false;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  const auto now = std::chrono::steady_clock::now();
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    if (it->second.expiresAt <= now) it = m_entries.erase(it);
    else ++it;
  }

  auto& entry = m_entries[keyId];
  std::set<std::string> pending;
  for (const auto& nonce : nonces) {
    const auto key = nonceKey(nonce);
    if (entry.nonces.count(key) != 0 || !pending.insert(key).second) {
      // Do not mutate the registry when any nonce in the batch is already
      // present (or duplicated within the batch).
      if (entry.nonces.empty())
        m_entries.erase(keyId);
      return false;
    }
  }

  const auto expiresAt = now + lifetime;
  if (entry.expiresAt < expiresAt)
    entry.expiresAt = expiresAt;
  entry.nonces.insert(pending.begin(), pending.end());
  return true;
}

void
NonceRegistry::invalidate(const std::string& keyId)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_entries.erase(keyId);
}

void
NonceRegistry::clearExpired(std::chrono::steady_clock::time_point now)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    if (it->second.expiresAt <= now) it = m_entries.erase(it);
    else ++it;
  }
}

size_t
NonceRegistry::size() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_entries.size();
}

} // namespace ndn_service_framework
