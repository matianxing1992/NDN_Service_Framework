#ifndef NDN_SERVICE_FRAMEWORK_HYBRID_MESSAGE_CRYPTO_HPP
#define NDN_SERVICE_FRAMEWORK_HYBRID_MESSAGE_CRYPTO_HPP

#include "NDNSFMessages.hpp"
#include "common.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/interest.hpp>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
    std::shared_ptr<const ndn::Data>
    findWrappedSendData(const ndn::Interest& interest) const;

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

/** Complete, authorized original wrapper packet set after offline validation. */
class VerifiedHybridMessageKeyWrapperPacketSetV1
{
public:
  VerifiedHybridMessageKeyWrapperPacketSetV1(
    VerifiedHybridMessageKeyWrapperPacketSetV1&&) noexcept = default;
  VerifiedHybridMessageKeyWrapperPacketSetV1& operator=(
    VerifiedHybridMessageKeyWrapperPacketSetV1&&) noexcept = default;
  VerifiedHybridMessageKeyWrapperPacketSetV1(
    const VerifiedHybridMessageKeyWrapperPacketSetV1&) = delete;
  VerifiedHybridMessageKeyWrapperPacketSetV1& operator=(
    const VerifiedHybridMessageKeyWrapperPacketSetV1&) = delete;

  const ndn::Name& objectName() const noexcept { return m_objectName; }
  const ndn::Block& cipherBlock() const noexcept { return m_cipherBlock; }
  const std::vector<std::pair<ndn::Name, std::string>>& packetFingerprints() const noexcept
  {
    return m_packetFingerprints;
  }

private:
  VerifiedHybridMessageKeyWrapperPacketSetV1(ndn::Name objectName,
                                             ndn::Block cipherBlock,
                                             std::vector<std::pair<ndn::Name, std::string>> packetFingerprints)
    : m_objectName(std::move(objectName)), m_cipherBlock(std::move(cipherBlock)),
      m_packetFingerprints(std::move(packetFingerprints))
  {}

  ndn::Name m_objectName;
  ndn::Block m_cipherBlock;
  std::vector<std::pair<ndn::Name, std::string>> m_packetFingerprints;

  friend VerifiedHybridMessageKeyWrapperPacketSetV1
  validateHybridMessageKeyWrapperPacketSet(
    const ndn::Name&, const std::vector<std::vector<uint8_t>>&,
    const std::function<bool(const ndn::Data&)>&);
};

/** Canonical NAC envelope fields derived from a verified original wrapper set. */
class VerifiedHybridMessageKeyEnvelopeV1
{
public:
  VerifiedHybridMessageKeyEnvelopeV1(VerifiedHybridMessageKeyEnvelopeV1&&) noexcept = default;
  VerifiedHybridMessageKeyEnvelopeV1& operator=(VerifiedHybridMessageKeyEnvelopeV1&&) noexcept = default;
  VerifiedHybridMessageKeyEnvelopeV1(const VerifiedHybridMessageKeyEnvelopeV1&) = delete;
  VerifiedHybridMessageKeyEnvelopeV1& operator=(const VerifiedHybridMessageKeyEnvelopeV1&) = delete;

  const ndn::Name& wrapperObjectName() const noexcept { return m_wrapperObjectName; }
  const std::vector<std::pair<ndn::Name, std::string>>& packetFingerprints() const noexcept
  {
    return m_packetFingerprints;
  }
  const ndn::Name& contentKeyName() const noexcept { return m_contentKeyName; }
  std::uint64_t plaintextBytes() const noexcept { return m_plaintextBytes; }
  std::size_t encryptedContentBytes() const noexcept { return m_encryptedContentBytes; }

private:
  VerifiedHybridMessageKeyEnvelopeV1(
    ndn::Name wrapperObjectName,
    std::vector<std::pair<ndn::Name, std::string>> packetFingerprints,
    ndn::Name contentKeyName, std::uint64_t plaintextBytes, std::size_t encryptedContentBytes)
    : m_wrapperObjectName(std::move(wrapperObjectName)),
      m_packetFingerprints(std::move(packetFingerprints)),
      m_contentKeyName(std::move(contentKeyName)), m_plaintextBytes(plaintextBytes),
      m_encryptedContentBytes(encryptedContentBytes)
  {}

  ndn::Name m_wrapperObjectName;
  std::vector<std::pair<ndn::Name, std::string>> m_packetFingerprints;
  ndn::Name m_contentKeyName;
  std::uint64_t m_plaintextBytes;
  std::size_t m_encryptedContentBytes;

  friend VerifiedHybridMessageKeyEnvelopeV1
  validateHybridMessageKeyWrapperEnvelopeV1(const VerifiedHybridMessageKeyWrapperPacketSetV1&);
};

/** Complete signed NAC content-key Data segment group found in an original packet bundle. */
class VerifiedNacContentKeyPacketSetV1
{
public:
  VerifiedNacContentKeyPacketSetV1(VerifiedNacContentKeyPacketSetV1&&) noexcept = default;
  VerifiedNacContentKeyPacketSetV1& operator=(VerifiedNacContentKeyPacketSetV1&&) noexcept = default;
  VerifiedNacContentKeyPacketSetV1(const VerifiedNacContentKeyPacketSetV1&) = delete;
  VerifiedNacContentKeyPacketSetV1& operator=(const VerifiedNacContentKeyPacketSetV1&) = delete;

  const ndn::Name& contentKeyName() const noexcept { return m_contentKeyName; }
  const ndn::Name& producerPrefix() const noexcept { return m_producerPrefix; }
  std::size_t segmentCount() const noexcept { return m_packetFingerprints.size(); }
  std::uint64_t contentBytes() const noexcept { return m_contentBytes; }
  const std::vector<std::pair<ndn::Name, std::string>>& packetFingerprints() const noexcept
  {
    return m_packetFingerprints;
  }

private:
  VerifiedNacContentKeyPacketSetV1(ndn::Name contentKeyName, ndn::Name producerPrefix,
                                   std::vector<std::pair<ndn::Name, std::string>> packetFingerprints,
                                   std::uint64_t contentBytes)
    : m_contentKeyName(std::move(contentKeyName)), m_producerPrefix(std::move(producerPrefix)),
      m_packetFingerprints(std::move(packetFingerprints)), m_contentBytes(contentBytes)
  {}

  ndn::Name m_contentKeyName;
  ndn::Name m_producerPrefix;
  std::vector<std::pair<ndn::Name, std::string>> m_packetFingerprints;
  std::uint64_t m_contentBytes;

  friend VerifiedNacContentKeyPacketSetV1
  validateNacContentKeyPacketSetV1(
    const ndn::Name&, const ndn::Name&, const std::vector<std::vector<uint8_t>>&,
    const std::function<bool(const ndn::Data&)>&);
};

/**
 * Validate a caller-supplied set of original Producer Data wires. The callback
 * must verify and authorize each packet's Producer. This checks namespace,
 * segment, FinalBlock, envelope and cipher commitment without issuing
 * Interests; it does not establish transport provenance or claim freshness.
 */
VerifiedHybridMessageKeyWrapperPacketSetV1
validateHybridMessageKeyWrapperPacketSet(
  const ndn::Name& expectedLocator,
  const std::vector<std::vector<uint8_t>>& packetWires,
  const std::function<bool(const ndn::Data&)>& authorizeProducer);

/** Validate NAC's exact Content(EncryptedContent, PlainTextSize, Name) structure. */
VerifiedHybridMessageKeyEnvelopeV1
validateHybridMessageKeyWrapperEnvelopeV1(
  const VerifiedHybridMessageKeyWrapperPacketSetV1& packetSet);

/**
 * Validate the complete NAC CK Data segment group among caller-supplied packet
 * wires. This does not establish that the wires came from a current Repo claim.
 */
VerifiedNacContentKeyPacketSetV1
validateNacContentKeyPacketSetV1(
  const ndn::Name& contentKeyName, const ndn::Name& producerPrefix,
  const std::vector<std::vector<uint8_t>>& packetWires,
  const std::function<bool(const ndn::Data&)>& authorizeProducer);

/** Require each exact name/wire fingerprint in one ordered original packet bundle. */
void requireOriginalPacketFingerprintsPresentV1(
  const std::vector<std::string>& packetNames,
  const std::vector<std::vector<uint8_t>>& packetWires,
  const std::vector<std::pair<ndn::Name, std::string>>& requiredPackets,
  const std::function<void()>& requireActive = {});

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

/** Owned route and original dispatch fence for offline original-key retrieval. */
struct HybridKeyFetchOptions {
    std::vector<ndn::Name> forwardingHint;
    std::chrono::steady_clock::time_point deadline;
    std::function<bool()> isCurrent;
};
void fetchHybridMessageKeyWrapper(
  ndn::Face& face, ndn::security::Validator& validator, const ndn::Name& locator,
  const HybridKeyFetchOptions& options,
  std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Name&, const ndn::Block&)> success,
  std::function<void(const std::string&)> failure);
void consumeHybridMessageKeyWrapper(
  ndn::Face& face, ndn::security::Validator& validator, ndn::nacabe::Consumer& consumer,
  const ndn::Name& locator, const HybridKeyFetchOptions& options,
  std::function<bool(const ndn::Data&)> authorizeProducer,
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
