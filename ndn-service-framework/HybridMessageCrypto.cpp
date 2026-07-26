#include "HybridMessageCrypto.hpp"
#include "utils.hpp"

#include <iomanip>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <ndn-cxx/security/transform/public-key.hpp>

namespace ndn_service_framework {
namespace {

ndn::Buffer
randomBuffer(size_t size)
{
    ndn::Buffer buffer(size);
    if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return buffer;
}

std::string
hexEncode(ndn::span<const uint8_t> bytes)
{
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        os << std::setw(2) << static_cast<unsigned>(b);
    }
    return os.str();
}

} // namespace

std::string
HybridMessageCrypto::makeScope(const ndn::Name& serviceName,
                               const ndn::Name& senderPrefix,
                               const std::string& accessAttribute,
                               const std::string& direction)
{
    return direction + "|" + serviceName.toUri() + "|" +
           senderPrefix.toUri() + "|" + accessAttribute;
}

HybridMessageKey
HybridMessageCrypto::getOrCreateSendKey(const ndn::Name& serviceName,
                                        const ndn::Name& senderPrefix,
                                        const std::string& accessAttribute,
                                        const std::string& direction,
                                        HybridCryptoCounters& counters)
{
    const auto now = std::chrono::steady_clock::now();
    const auto scope = makeScope(serviceName, senderPrefix, accessAttribute, direction);

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sendKeys.find(scope);
    bool rotate = it == m_sendKeys.end();
    if (!rotate) {
        if (now - it->second.createdAt >= MAX_EPOCH_AGE) {
            ++counters.hybrid_key_rotation_age;
            rotate = true;
        }
        else if (it->second.uses >= MAX_EPOCH_USES) {
            ++counters.hybrid_key_rotation_uses;
            rotate = true;
        }
    }

    if (rotate) {
        const auto epochBytes = randomBuffer(8);
        HybridMessageKey key;
        key.epochId = hexEncode(ndn::span<const uint8_t>(epochBytes.data(), epochBytes.size()));
        key.key = randomBuffer(MESSAGE_KEY_SIZE);
        key.createdAt = now;
        key.uses = 0;
        key.keyName = makeHybridMessageKeyName(serviceName, senderPrefix,
                                               accessAttribute, key.epochId);
        key.keyId = key.keyName.toUri();
        it = m_sendKeys.insert_or_assign(scope, key).first;
        m_wrappedSendKeys.erase(key.keyId);
        ++counters.hybrid_key_epoch_created;
    }

    ++it->second.uses;
    return it->second;
}

HybridMessageKey
HybridMessageCrypto::getOrCreateStatusSendKey(const ndn::Name& requesterIdentity,
                                              const std::string& statusHandle,
                                              const std::string& recipientKeyId,
                                              HybridCryptoCounters& counters)
{
    if (!isValidOpaqueControlHandle(statusHandle) || recipientKeyId.empty()) {
        throw std::invalid_argument("invalid requester-scoped status key binding");
    }
    // A distinct STATUS scope prevents service invocation token/key material
    // from becoming status encryption authority.
    return getOrCreateSendKey(ndn::Name("/NDNSF/SECURE-STATUS"), requesterIdentity,
                              statusHandle + "|" + recipientKeyId, "STATUS", counters);
}

void
HybridMessageCrypto::cacheReceiveKey(const std::string& keyId,
                                     const std::string& epochId,
                                     const ndn::Buffer& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_receiveKeys[keyId] = CachedKey{epochId, key, std::chrono::steady_clock::now(), 0};
}

bool
HybridMessageCrypto::findReceiveKey(const std::string& keyId,
                                    ndn::Buffer& key,
                                    HybridCryptoCounters& counters)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_receiveKeys.find(keyId);
    if (it == m_receiveKeys.end()) {
        ++counters.key_cache_miss_count;
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - it->second.createdAt >= MAX_EPOCH_AGE ||
        it->second.uses >= MAX_EPOCH_USES) {
        m_receiveKeys.erase(it);
        ++counters.key_cache_miss_count;
        return false;
    }
    ++it->second.uses;
    key = it->second.key;
    ++counters.key_cache_hit_count;
    return true;
}

void
HybridMessageCrypto::markSendKeyWrapped(const std::string& keyId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_wrappedSendKeys.insert(keyId);
}

void
HybridMessageCrypto::cacheWrappedSendKey(const std::string& keyId,
                                         const ndn::Buffer& wrappedKey)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_wrappedSendKeysById[keyId] = wrappedKey;
    m_wrappedSendKeys.insert(keyId);
}

bool
HybridMessageCrypto::getWrappedSendKey(const std::string& keyId,
                                       ndn::Buffer& wrappedKey) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_wrappedSendKeysById.find(keyId);
    if (it == m_wrappedSendKeysById.end()) {
        return false;
    }
    wrappedKey = it->second;
    return true;
}

bool
HybridMessageCrypto::shouldAttachWrappedKey(const std::string& keyId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_wrappedSendKeys.find(keyId) == m_wrappedSendKeys.end();
}

ndn::Name
makeHybridMessageKeyName(const ndn::Name& serviceName,
                         const ndn::Name& senderPrefix,
                         const std::string& accessAttribute,
                         const std::string& epochId)
{
    ndn::Name name("/NDNSF/HYBRID-KEY");
    name.append(serviceName);
    name.append("SENDER");
    name.append(senderPrefix);
    name.append("ATTRIBUTE");
    name.append(ndn::Name(accessAttribute));
    name.append(epochId);
    return name;
}

HybridAeadResult
hybridAesGcmEncrypt(const ndn::Buffer& key,
                    ndn::span<const uint8_t> plaintext,
                    ndn::span<const uint8_t> associatedData)
{
    const auto nonce = randomBuffer(HybridMessageCrypto::NONCE_SIZE);
    return hybridAesGcmEncryptWithNonce(
        key, ndn::span<const uint8_t>(nonce.data(), nonce.size()),
        plaintext, associatedData);
}

HybridAeadResult
hybridAesGcmEncryptWithNonce(const ndn::Buffer& key,
                            ndn::span<const uint8_t> nonce,
                            ndn::span<const uint8_t> plaintext,
                            ndn::span<const uint8_t> associatedData)
{
    if (key.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
        throw std::runtime_error("invalid AES-GCM key size");
    }
    if (nonce.size() != HybridMessageCrypto::NONCE_SIZE) {
        throw std::invalid_argument("invalid AES-GCM nonce size");
    }

    HybridAeadResult result;
    result.nonce = ndn::Buffer(nonce.data(), nonce.size());
    result.ciphertext = ndn::Buffer(plaintext.size());
    result.tag = ndn::Buffer(HybridMessageCrypto::TAG_SIZE);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    }
    int len = 0;
    int ciphertextLen = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                  static_cast<int>(result.nonce.size()), nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), result.nonce.data()) == 1;
    if (ok && !associatedData.empty()) {
        ok = EVP_EncryptUpdate(ctx, nullptr, &len,
                               associatedData.data(),
                               static_cast<int>(associatedData.size())) == 1;
    }
    if (ok && !plaintext.empty()) {
        ok = EVP_EncryptUpdate(ctx, result.ciphertext.data(), &len,
                               plaintext.data(),
                               static_cast<int>(plaintext.size())) == 1;
        ciphertextLen = len;
    }
    if (ok) {
        ok = EVP_EncryptFinal_ex(ctx, result.ciphertext.data() + ciphertextLen, &len) == 1;
        ciphertextLen += len;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                                 static_cast<int>(result.tag.size()),
                                 result.tag.data()) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        throw std::runtime_error("AES-GCM encrypt failed");
    }
    result.ciphertext.resize(ciphertextLen);
    return result;
}

bool
hybridAesGcmDecrypt(const ndn::Buffer& key,
                    const HybridMessageEnvelope& envelope,
                    ndn::span<const uint8_t> associatedData,
                    ndn::Buffer& plaintext)
{
    if (key.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
        return false;
    }

    plaintext = ndn::Buffer(envelope.getCipherText().size());
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        return false;
    }
    int len = 0;
    int plaintextLen = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                  static_cast<int>(envelope.getNonce().size()), nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                  key.data(), envelope.getNonce().data()) == 1;
    if (ok && !associatedData.empty()) {
        ok = EVP_DecryptUpdate(ctx, nullptr, &len,
                               associatedData.data(),
                               static_cast<int>(associatedData.size())) == 1;
    }
    if (ok && !envelope.getCipherText().empty()) {
        ok = EVP_DecryptUpdate(ctx, plaintext.data(), &len,
                               envelope.getCipherText().data(),
                               static_cast<int>(envelope.getCipherText().size())) == 1;
        plaintextLen = len;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                                 static_cast<int>(envelope.getAuthTag().size()),
                                 const_cast<uint8_t*>(envelope.getAuthTag().data())) == 1 &&
             EVP_DecryptFinal_ex(ctx, plaintext.data() + plaintextLen, &len) == 1;
        plaintextLen += len;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        plaintext.clear();
        return false;
    }
    plaintext.resize(plaintextLen);
    return true;
}

std::string
hybridMessageTypeForName(const ndn::Name& name)
{
    for (const auto& component : name) {
        const auto text = component.toUri();
        if (text == "REQUEST" || text == "ACK" ||
            text == "SELECTION" || text == "RESPONSE") {
            return text;
        }
    }
    return "UNKNOWN";
}

std::string
hybridAccessAttributeForName(const ndn::Name& name, const ndn::Name& serviceName)
{
    const auto type = hybridMessageTypeForName(name);
    if (type == "REQUEST" || type == "SELECTION") {
        return "/SERVICE" + serviceName.toUri();
    }
    return "/PERMISSION" + serviceName.toUri();
}

ndn::Buffer
hybridAssociatedData(const ndn::Name& messageName,
                     const std::string& messageType,
                     const ndn::Name& requestId,
                     const ndn::Name& serviceName,
                     const ndn::Name& senderPrefix,
                     const std::string& keyId,
                     const std::string& epochId)
{
    const std::string text = messageName.toUri() + "|" + messageType + "|" +
                             requestId.toUri() + "|" + serviceName.toUri() + "|" +
                             senderPrefix.toUri() + "|" + keyId + "|" + epochId;
    return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

ndn::Buffer
secureStatusAssociatedData(const ndn::Name& dataName,
                           uint64_t version,
                           const std::string& statusHandle,
                           const ndn::Name& requesterIdentity,
                           const ndn::Name& providerIdentity,
                           uint64_t attempt,
                           const std::string& keyId,
                           const std::string& epochId)
{
    if (version != DeploymentControlMessage::VERSION ||
        !isValidOpaqueControlHandle(statusHandle)) {
        throw std::invalid_argument("invalid secure status associated-data binding");
    }
    const std::string text = dataName.toUri() + "|SECURE-SELECTION-STATUS|" +
      std::to_string(version) + "|" + statusHandle + "|" +
      requesterIdentity.toUri() + "|" + providerIdentity.toUri() + "|" +
      std::to_string(attempt) + "|" + keyId + "|" + epochId;
    return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string
generateSecureStatusKeyHex()
{
    const auto key = randomBuffer(HybridMessageCrypto::MESSAGE_KEY_SIZE);
    return hexEncode(ndn::span<const uint8_t>(key.data(), key.size()));
}

ndn::Buffer
decodeSecureStatusKeyHex(const std::string& value)
{
    if (value.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE * 2) {
        return {};
    }
    ndn::Buffer key(HybridMessageCrypto::MESSAGE_KEY_SIZE);
    for (size_t i = 0; i < key.size(); ++i) {
        const auto part = value.substr(i * 2, 2);
        char* end = nullptr;
        const auto byte = std::strtoul(part.c_str(), &end, 16);
        if (end == nullptr || *end != '\0' || byte > 255) return {};
        key[i] = static_cast<uint8_t>(byte);
    }
    return key;
}

std::string selectionGatedHex(ndn::span<const uint8_t> value) { return hexEncode(value); }

ndn::Buffer selectionGatedUnhex(const std::string& value)
{
    if (value.size() % 2 != 0) return {};
    ndn::Buffer result(value.size() / 2);
    for (size_t i = 0; i < result.size(); ++i) {
        const auto part = value.substr(i * 2, 2);
        char* end = nullptr;
        const auto byte = std::strtoul(part.c_str(), &end, 16);
        if (end == nullptr || *end != '\0' || byte > 255) return {};
        result[i] = static_cast<uint8_t>(byte);
    }
    return result;
}

ndn::Buffer selectionGatedInputAssociatedData(const ndn::Name& requester,
                                               const ndn::Name& serviceName,
                                               const ndn::Name& requestId)
{
    const std::string text = "NDNSF-SELECTION-GATED-INPUT-V1|" +
      requester.toUri() + "|" + serviceName.toUri() + "|" + requestId.toUri();
    return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::pair<EncryptedRequestInput, ndn::Buffer>
encryptSelectionGatedInput(const ndn::Name& requester, const ndn::Name& serviceName,
                           const ndn::Name& requestId,
                           ndn::span<const uint8_t> plaintext)
{
    if (plaintext.size() * 2 > DeploymentControlMessage::MAX_FIELD_VALUE)
        throw std::invalid_argument("SelectionGatedInputV1 inline input exceeds bound");
    const auto key = randomBuffer(HybridMessageCrypto::MESSAGE_KEY_SIZE);
    const auto aad = selectionGatedInputAssociatedData(requester, serviceName, requestId);
    const auto encrypted = hybridAesGcmEncrypt(key, plaintext, aad);
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(aad.data(), aad.size(), digest);
    EncryptedRequestInput result;
    result.setField("schemaVersion", "1");
    result.setField("algorithm", "AES-256-GCM");
    result.setField("nonce", hexEncode(encrypted.nonce));
    result.setField("ciphertext", hexEncode(encrypted.ciphertext));
    result.setField("authTag", hexEncode(encrypted.tag));
    result.setField("aadDigest", hexEncode(ndn::span<const uint8_t>(digest, sizeof(digest))));
    return {result, key};
}

ndn::Buffer wrapSelectionGatedInputKey(const ndn::Buffer& key,
                                        ndn::span<const uint8_t> publicKeyBytes)
{
    ndn::security::transform::PublicKey publicKey;
    publicKey.loadPkcs8(publicKeyBytes);
    if (publicKey.getKeyType() != ndn::KeyType::RSA)
        throw std::invalid_argument("Selection input key recipient must be RSA");
    auto wrapped = publicKey.encrypt(key);
    return ndn::Buffer(wrapped->begin(), wrapped->end());
}

ndn::Buffer unwrapSelectionGatedInputKey(const ndn::Buffer& wrappedKey,
                                          const ndn::Name& recipientCertName,
                                          const ndn::security::KeyChain& keyChain)
{
    const auto keyName = ndn::security::extractKeyNameFromCertName(recipientCertName);
    auto plaintext = keyChain.getTpm().decrypt(wrappedKey, keyName);
    return plaintext ? ndn::Buffer(plaintext->begin(), plaintext->end()) : ndn::Buffer();
}

bool decryptSelectionGatedInput(const EncryptedRequestInput& encrypted,
                                const ndn::Buffer& key,
                                const ndn::Name& requester,
                                const ndn::Name& serviceName,
                                const ndn::Name& requestId,
                                ndn::Buffer& plaintext)
{
    if (!encrypted.hasField("algorithm") || encrypted.getField("algorithm") != "AES-256-GCM" ||
        !encrypted.hasField("nonce") || !encrypted.hasField("ciphertext") ||
        !encrypted.hasField("authTag") || !encrypted.hasField("aadDigest")) return false;
    const auto aad = selectionGatedInputAssociatedData(requester, serviceName, requestId);
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(aad.data(), aad.size(), digest);
    if (encrypted.getField("aadDigest") !=
        hexEncode(ndn::span<const uint8_t>(digest, sizeof(digest)))) return false;
    HybridMessageEnvelope envelope;
    envelope.setNonce(selectionGatedUnhex(encrypted.getField("nonce")));
    envelope.setCipherText(selectionGatedUnhex(encrypted.getField("ciphertext")));
    envelope.setAuthTag(selectionGatedUnhex(encrypted.getField("authTag")));
    return hybridAesGcmDecrypt(key, envelope, aad, plaintext);
}

RecipientEncryptedAssignment
encryptRecipientAssignment(ndn::span<const uint8_t> plaintext,
                            ndn::span<const uint8_t> recipientPublicKey,
                            const ndn::Name& recipient,
                            const ndn::Name& recipientCertName,
                            ndn::span<const uint8_t> associatedData)
{
    if (plaintext.size() * 2 > DeploymentControlMessage::MAX_FIELD_VALUE)
        throw std::invalid_argument("recipient assignment exceeds inline bound");
    const auto key = randomBuffer(HybridMessageCrypto::MESSAGE_KEY_SIZE);
    const auto encrypted = hybridAesGcmEncrypt(key, plaintext, associatedData);
    const auto wrapped = wrapSelectionGatedInputKey(key, recipientPublicKey);
    uint8_t aadDigest[SHA256_DIGEST_LENGTH];
    uint8_t payloadDigest[SHA256_DIGEST_LENGTH];
    SHA256(associatedData.data(), associatedData.size(), aadDigest);
    SHA256(plaintext.data(), plaintext.size(), payloadDigest);
    RecipientEncryptedAssignment result;
    result.setField("schemaVersion", "1");
    result.setField("algorithm", "RSA-WRAPPED-AES-256-GCM");
    result.setField("recipient", recipient.toUri());
    result.setField("recipientCertName", recipientCertName.toUri());
    result.setField("wrappedAssignmentKey", hexEncode(wrapped));
    result.setField("nonce", hexEncode(encrypted.nonce));
    result.setField("ciphertext", hexEncode(encrypted.ciphertext));
    result.setField("authTag", hexEncode(encrypted.tag));
    result.setField("aadDigest", hexEncode(
      ndn::span<const uint8_t>(aadDigest, sizeof(aadDigest))));
    result.setField("assignmentDigest", hexEncode(
      ndn::span<const uint8_t>(payloadDigest, sizeof(payloadDigest))));
    return result;
}

ndn::Buffer
recipientAssignmentAssociatedData(const ndn::Name& requester,
                                  const ndn::Name& provider,
                                  const ndn::Name& serviceName,
                                  const ndn::Name& requestId,
                                  const std::string& reservationId,
                                  const std::string& planDigest)
{
    const std::string text = "NDNSF-RECIPIENT-ASSIGNMENT-V1|" +
      requester.toUri() + "|" + provider.toUri() + "|" + serviceName.toUri() +
      "|" + requestId.toUri() + "|" + reservationId + "|" + planDigest;
    return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool
decryptRecipientAssignment(const RecipientEncryptedAssignment& encrypted,
                           const ndn::Name& expectedRecipient,
                           const ndn::Name& expectedRecipientCertName,
                           const ndn::security::KeyChain& keyChain,
                           ndn::span<const uint8_t> associatedData,
                           ndn::Buffer& plaintext)
{
    if (!encrypted.hasField("algorithm") ||
        encrypted.getField("algorithm") != "RSA-WRAPPED-AES-256-GCM" ||
        !encrypted.hasField("recipient") ||
        encrypted.getField("recipient") != expectedRecipient.toUri() ||
        !encrypted.hasField("recipientCertName") ||
        encrypted.getField("recipientCertName") != expectedRecipientCertName.toUri() ||
        !encrypted.hasField("wrappedAssignmentKey") ||
        !encrypted.hasField("nonce") || !encrypted.hasField("ciphertext") ||
        !encrypted.hasField("authTag") || !encrypted.hasField("aadDigest") ||
        !encrypted.hasField("assignmentDigest")) return false;
    uint8_t aadDigest[SHA256_DIGEST_LENGTH];
    SHA256(associatedData.data(), associatedData.size(), aadDigest);
    if (encrypted.getField("aadDigest") != hexEncode(
          ndn::span<const uint8_t>(aadDigest, sizeof(aadDigest)))) return false;
    const auto key = unwrapSelectionGatedInputKey(
      selectionGatedUnhex(encrypted.getField("wrappedAssignmentKey")),
      expectedRecipientCertName, keyChain);
    HybridMessageEnvelope envelope;
    envelope.setNonce(selectionGatedUnhex(encrypted.getField("nonce")));
    envelope.setCipherText(selectionGatedUnhex(encrypted.getField("ciphertext")));
    envelope.setAuthTag(selectionGatedUnhex(encrypted.getField("authTag")));
    if (!hybridAesGcmDecrypt(key, envelope, associatedData, plaintext)) return false;
    uint8_t payloadDigest[SHA256_DIGEST_LENGTH];
    SHA256(plaintext.data(), plaintext.size(), payloadDigest);
    return encrypted.getField("assignmentDigest") == hexEncode(
      ndn::span<const uint8_t>(payloadDigest, sizeof(payloadDigest)));
}

} // namespace ndn_service_framework
