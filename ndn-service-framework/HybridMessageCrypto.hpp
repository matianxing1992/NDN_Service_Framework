#ifndef NDN_SERVICE_FRAMEWORK_HYBRID_MESSAGE_CRYPTO_HPP
#define NDN_SERVICE_FRAMEWORK_HYBRID_MESSAGE_CRYPTO_HPP

#include "NDNSFMessages.hpp"
#include "common.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>

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
    static constexpr size_t MAX_EPOCH_USES = 100;
    static constexpr auto MAX_EPOCH_AGE = std::chrono::seconds(10);

    HybridMessageKey getOrCreateSendKey(const ndn::Name& serviceName,
                                        const ndn::Name& senderPrefix,
                                        const std::string& accessAttribute,
                                        const std::string& direction,
                                        HybridCryptoCounters& counters);

    void cacheReceiveKey(const std::string& keyId,
                         const std::string& epochId,
                         const ndn::Buffer& key);

    bool findReceiveKey(const std::string& keyId,
                        ndn::Buffer& key,
                        HybridCryptoCounters& counters);

    void markSendKeyWrapped(const std::string& keyId);
    bool shouldAttachWrappedKey(const std::string& keyId) const;

private:
    struct CachedKey
    {
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
    std::map<std::string, CachedKey> m_receiveKeys;
    std::set<std::string> m_wrappedSendKeys;
};

ndn::Name makeHybridMessageKeyName(const ndn::Name& serviceName,
                                   const ndn::Name& senderPrefix,
                                   const std::string& accessAttribute,
                                   const std::string& epochId);

HybridAeadResult hybridAesGcmEncrypt(const ndn::Buffer& key,
                                     ndn::span<const uint8_t> plaintext,
                                     ndn::span<const uint8_t> associatedData);

bool hybridAesGcmDecrypt(const ndn::Buffer& key,
                         const HybridMessageEnvelope& envelope,
                         ndn::span<const uint8_t> associatedData,
                         ndn::Buffer& plaintext);

std::string hybridMessageTypeForName(const ndn::Name& name);
std::string hybridAccessAttributeForName(const ndn::Name& name,
                                         const ndn::Name& serviceName);
ndn::Buffer hybridAssociatedData(const ndn::Name& messageName,
                                 const std::string& messageType,
                                 const ndn::Name& requestId,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& senderPrefix,
                                 const std::string& keyId,
                                 const std::string& epochId);

} // namespace ndn_service_framework

#endif
