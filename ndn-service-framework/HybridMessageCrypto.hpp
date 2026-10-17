#ifndef NDN_SERVICE_FRAMEWORK_HYBRID_MESSAGE_CRYPTO_HPP
#define NDN_SERVICE_FRAMEWORK_HYBRID_MESSAGE_CRYPTO_HPP

#include "NDNSFMessages.hpp"
#include "common.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace ndn_service_framework {

struct HybridAeadResult
{
    ndn::Buffer nonce;
    ndn::Buffer ciphertext;
    ndn::Buffer tag;
};

struct HybridMessageKey
{
    std::string keyId;
    std::string epochId;
    ndn::Name keyName;
    ndn::Buffer key;
    std::chrono::steady_clock::time_point createdAt;
    size_t uses = 0;
};

class HybridMessageCrypto
{
public:
    static constexpr size_t MESSAGE_KEY_SIZE = 32;
    static constexpr size_t NONCE_SIZE = 12;
    static constexpr size_t TAG_SIZE = 16;
    // Bound event-loop AES/TLV work; larger messages retain worker backpressure.
    static constexpr size_t INLINE_CRYPTO_MAX_BYTES = 4096;
    static constexpr size_t MAX_EPOCH_USES = 10000;
    static constexpr auto MAX_EPOCH_AGE = std::chrono::seconds(60);

    HybridMessageKey getOrCreateSendKey(const ndn::Name& serviceName,
                                        const ndn::Name& senderPrefix,
                                        const std::string& accessAttribute,
                                        const std::string& direction,
                                        HybridCryptoCounters& counters);

    HybridMessageKey getOrCreateStatusSendKey(const ndn::Name& requesterIdentity,
                                              const std::string& statusHandle,
                                              const std::string& recipientKeyId,
                                              HybridCryptoCounters& counters);

    void cacheReceiveKey(const std::string& keyId,
                         const std::string& epochId,
                         const ndn::Buffer& key);

    /** Cache a received MessageKey with its service scope for revocation. */
    void cacheReceiveKey(const ndn::Name& serviceName,
                         const std::string& keyId,
                         const std::string& epochId,
                         const ndn::Buffer& key);

    bool findReceiveKey(const std::string& keyId,
                        ndn::Buffer& key,
                        HybridCryptoCounters& counters);

    void markSendKeyWrapped(const std::string& keyId);
    void cacheWrappedSendKey(const std::string& keyId,
                             const ndn::Buffer& wrappedKey);
    /** Cache wrapped key material with its service scope for revocation. */
    void cacheWrappedSendKey(const ndn::Name& serviceName,
                             const std::string& keyId,
                             const ndn::Buffer& wrappedKey);
    /** Retain original signed wrapper/CK packets before any publication. */
    void cacheWrappedSendPackets(const ndn::Name& service, const std::string& keyId, const ndn::Buffer& wrappedKey,
      const std::vector<std::shared_ptr<ndn::Data>>& content,
      const std::vector<std::shared_ptr<ndn::Data>>& contentKeys);
    bool getWrappedSendPackets(const std::string& keyId,
      std::vector<std::shared_ptr<ndn::Data>>& content,
      std::vector<std::shared_ptr<ndn::Data>>& contentKeys) const;

    bool getWrappedSendKey(const std::string& keyId,
                           ndn::Buffer& wrappedKey) const;
    bool shouldAttachWrappedKey(const std::string& keyId) const;

    /** Remove one scoped wrapped send-key entry after a failed publication. */
    bool eraseWrappedSendKey(const ndn::Name& serviceName, const std::string& keyId);
    bool retainWrappedSendKey(const ndn::Name& serviceName, const std::string& keyId);

    /**
     * Evict all service-scoped send/receive/wrapped key material.  Unknown
     * unscoped legacy entries are retained rather than over-invalidated.
     * Returns the number of scoped entries removed.
     */
    size_t invalidateService(const ndn::Name& serviceName);

private:
    struct CachedKey
    {
        ndn::Name serviceName;
        std::string epochId;
        ndn::Buffer key;
        std::chrono::steady_clock::time_point createdAt;
        size_t uses = 0;
    };

    static std::string makeScope(const ndn::Name& serviceName,
                                 const ndn::Name& senderPrefix,
                                 const std::string& accessAttribute,
                                 const std::string& direction);

    mutable std::mutex m_mutex;
    std::map<std::string, HybridMessageKey> m_sendKeys;
    std::map<std::string, ndn::Name> m_sendKeyServices;
    std::map<std::string, CachedKey> m_receiveKeys;
    std::map<std::string, ndn::Buffer> m_wrappedSendKeysById;
    std::map<std::string, ndn::Name> m_wrappedSendKeyServices;
    std::map<std::string, std::size_t> m_wrappedSendKeyReferences;
    struct OriginalWrappedPackets {
        ndn::Name service;
        std::vector<std::shared_ptr<const ndn::Data>> content;
        std::vector<std::shared_ptr<const ndn::Data>> contentKeys;
        std::size_t wireBytes = 0;
    };
    std::map<std::string, OriginalWrappedPackets> m_wrappedSendPackets;
    std::size_t m_wrappedPacketBytes = 0;
    std::size_t m_wrappedPacketCount = 0;
    void eraseWrappedPacketsLocked(const std::string& keyId);
    std::set<std::string> m_wrappedSendKeys;
};

ndn::Name makeHybridMessageKeyName(const ndn::Name& serviceName,
                                   const ndn::Name& senderPrefix,
                                   const std::string& accessAttribute,
                                   const std::string& epochId);

/** Full producer-owned logical locator; final original objects append cipher digest and segment. */
ndn::Name makeHybridMessageKeyDataName(const ndn::Name& serviceName,
                                       const ndn::Name& senderPrefix,
                                       const std::string& accessAttribute,
                                       const std::string& epochId);

/** Return the 8-byte, hex-encoded key identifier carried by envelope v2. */
std::string hybridCompactKeyId(const std::string& keyId);

/** Prepare final original packets from an unpublished encrypted NAC wrapper.
 * The logical key identifier stays private; published object names bind cipher bytes. */
std::vector<std::shared_ptr<ndn::Data>> prepareHybridMessageKeyPackets(
  const ndn::Name& service, const ndn::Name& sender, const std::string& attribute,
  const std::string& epoch, ndn::span<const uint8_t> cipher,
  ndn::KeyChain& keyChain, const ndn::security::SigningInfo& signingInfo,
  ndn::time::milliseconds freshness, uint32_t contentType);
/** One asynchronous wrapper retrieval; admit the complete signed object and
 * cipher commitment before handing bytes to the existing NAC decryptor. */
void fetchHybridMessageKeyWrapper(
  ndn::Face& face, ndn::security::Validator& validator, const ndn::Name& expectedLocator,
  std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Name&, const ndn::Block&)> success,
  std::function<void(const std::string&)> failure);

/** Apply the wrapper admission above, then existing NAC decryption on the Face. */
void consumeHybridMessageKeyWrapper(
  ndn::Face& face, ndn::security::Validator& validator, ndn::nacabe::Consumer& consumer,
  const ndn::Name& locator, std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Buffer&)> success,
  std::function<void(const std::string&)> failure);

HybridAeadResult hybridAesGcmEncrypt(const ndn::Buffer& key,
                                     ndn::span<const uint8_t> plaintext,
                                     ndn::span<const uint8_t> associatedData);

HybridAeadResult hybridAesGcmEncryptWithNonce(
                                     const ndn::Buffer& key,
                                     ndn::span<const uint8_t> nonce,
                                     ndn::span<const uint8_t> plaintext,
                                     ndn::span<const uint8_t> associatedData);

bool hybridAesGcmDecrypt(const ndn::Buffer& key,
                         const HybridMessageEnvelope& envelope,
                         ndn::span<const uint8_t> associatedData,
                         ndn::Buffer& plaintext);

std::string hybridMessageTypeForName(const ndn::Name& name);
std::string hybridMessageTypeForName(const ndn::Name& name,
                                    const ndn::Name& expectedActor);
/** OpenABE attribute spelling is independent of process URI display mode.
 * Only installed DSL-safe service spellings are admitted. */
std::string makeAbeServiceAttribute(const ndn::Name& service,
                                    const std::string& kind);

std::string hybridAccessAttributeForName(const ndn::Name& name,
                                         const ndn::Name& serviceName);
std::string hybridAccessAttributeForName(const ndn::Name& name,
                                         const ndn::Name& serviceName,
                                         const ndn::Name& expectedActor);
ndn::Buffer hybridAssociatedData(const ndn::Name& messageName,
                                 const std::string& messageType,
                                 const ndn::Name& requestId,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& senderPrefix,
                                 const std::string& keyId,
                                 const std::string& epochId);

ndn::Buffer secureStatusAssociatedData(const ndn::Name& dataName,
                                       uint64_t version,
                                       const std::string& statusHandle,
                                       const ndn::Name& requesterIdentity,
                                       const ndn::Name& providerIdentity,
                                       uint64_t attempt,
                                       const std::string& keyId,
                                       const std::string& epochId);

std::string generateSecureStatusKeyHex();
ndn::Buffer decodeSecureStatusKeyHex(const std::string& value);

std::string selectionGatedHex(ndn::span<const uint8_t> value);
ndn::Buffer selectionGatedUnhex(const std::string& value);
ndn::Buffer selectionGatedInputAssociatedData(
    const ndn::Name& requester, const ndn::Name& serviceName,
    const ndn::Name& requestId);
std::pair<EncryptedRequestInput, ndn::Buffer> encryptSelectionGatedInput(
    const ndn::Name& requester, const ndn::Name& serviceName,
    const ndn::Name& requestId, ndn::span<const uint8_t> plaintext);
ndn::Buffer wrapSelectionGatedInputKey(
    const ndn::Buffer& key, ndn::span<const uint8_t> recipientPublicKey);
ndn::Buffer unwrapSelectionGatedInputKey(
    const ndn::Buffer& wrappedKey, const ndn::Name& recipientCertName,
    const ndn::security::KeyChain& keyChain);
bool decryptSelectionGatedInput(
    const EncryptedRequestInput& encrypted, const ndn::Buffer& key,
    const ndn::Name& requester, const ndn::Name& serviceName,
    const ndn::Name& requestId, ndn::Buffer& plaintext);
RecipientEncryptedAssignment encryptRecipientAssignment(
    ndn::span<const uint8_t> plaintext,
    ndn::span<const uint8_t> recipientPublicKey,
    const ndn::Name& recipient, const ndn::Name& recipientCertName,
    ndn::span<const uint8_t> associatedData);
ndn::Buffer recipientAssignmentAssociatedData(
    const ndn::Name& requester, const ndn::Name& provider,
    const ndn::Name& serviceName, const ndn::Name& requestId,
    const std::string& reservationId, const std::string& planDigest);
bool decryptRecipientAssignment(
    const RecipientEncryptedAssignment& encrypted,
    const ndn::Name& expectedRecipient,
    const ndn::Name& expectedRecipientCertName,
    const ndn::security::KeyChain& keyChain,
    ndn::span<const uint8_t> associatedData,
    ndn::Buffer& plaintext);

} // namespace ndn_service_framework

#endif
