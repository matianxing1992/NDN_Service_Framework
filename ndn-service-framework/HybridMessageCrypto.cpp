#include "HybridMessageCrypto.hpp"
#include "utils.hpp"
#include "ProducerDataNaming.hpp"
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/util/segmenter.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>
#include <nac-abe/consumer.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/post.hpp>

#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <ndn-cxx/security/transform/public-key.hpp>

namespace ndn_service_framework {

NDN_LOG_INIT(ndn_service_framework.HybridMessageCrypto);

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

std::string
sha256Hex(ndn::span<const uint8_t> bytes)
{
    uint8_t digest[SHA256_DIGEST_LENGTH];
    if (!SHA256(bytes.data(), bytes.size(), digest))
        throw std::runtime_error("SHA-256 digest failed");
    return hexEncode(ndn::span<const uint8_t>(digest, sizeof(digest)));
}

std::string
wrappedCipherDigest(ndn::span<const uint8_t> cipher)
{
    return sha256Hex(cipher);
}

bool
isCompactHexKeyId(const std::string& value)
{
    if (value.size() != 16) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [] (unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

} // namespace

std::string
hybridCompactKeyId(const std::string& keyId)
{
    if (isCompactHexKeyId(keyId)) {
        std::string normalized = keyId;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return normalized;
    }
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t*>(keyId.data()), keyId.size(), digest);
    return hexEncode(ndn::span<const uint8_t>(digest, 8));
}

std::string
HybridMessageCrypto::makeScope(const ndn::Name& serviceName,
                               const ndn::Name& senderPrefix,
                               const std::string& accessAttribute,
                               const std::string& direction)
{
    return direction + "|" + serviceName.toUri(ndn::name::UriFormat::CANONICAL) + "|" +
           senderPrefix.toUri(ndn::name::UriFormat::CANONICAL) + "|" + accessAttribute;
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
        key.keyId = key.keyName.toUri(ndn::name::UriFormat::CANONICAL);
        it = m_sendKeys.insert_or_assign(scope, key).first;
        m_sendKeyServices[scope] = serviceName;
        m_wrappedSendKeys.erase(key.keyId);
        m_wrappedSendKeysById.erase(key.keyId);
        m_wrappedSendKeyServices.erase(key.keyId);
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
    cacheReceiveKey(ndn::Name(), keyId, epochId, key);
}

void
HybridMessageCrypto::cacheReceiveKey(const ndn::Name& serviceName,
                                     const std::string& keyId,
                                     const std::string& epochId,
                                     const ndn::Buffer& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_receiveKeys[hybridCompactKeyId(keyId)] =
        CachedKey{serviceName, epochId, key, std::chrono::steady_clock::now(), 0};
}

bool
HybridMessageCrypto::findReceiveKey(const std::string& keyId,
                                    ndn::Buffer& key,
                                    HybridCryptoCounters& counters)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_receiveKeys.find(hybridCompactKeyId(keyId));
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
    cacheWrappedSendKey(ndn::Name(), keyId, wrappedKey);
}

void
HybridMessageCrypto::cacheWrappedSendKey(const ndn::Name& serviceName,
                                         const std::string& keyId,
                                         const ndn::Buffer& wrappedKey)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = m_wrappedSendKeysById.find(keyId);
    if (existing != m_wrappedSendKeysById.end() && existing->second != wrappedKey)
        throw std::invalid_argument("immutable wrapped key ciphertext conflict");
    const auto owner = m_wrappedSendKeyServices.find(keyId);
    if (!serviceName.empty() && owner != m_wrappedSendKeyServices.end() && owner->second != serviceName)
        throw std::invalid_argument("immutable wrapped key service conflict");
    m_wrappedSendKeysById.emplace(keyId, wrappedKey);
    m_wrappedSendKeys.insert(keyId);
    if (!serviceName.empty()) {
        m_wrappedSendKeyServices[keyId] = serviceName;
        ++m_wrappedSendKeyReferences[keyId];
    }
}

void
HybridMessageCrypto::cacheWrappedSendPackets(const ndn::Name& service, const std::string& keyId, const ndn::Buffer& wrappedKey,
  const std::vector<std::shared_ptr<ndn::Data>>& content,
  const std::vector<std::shared_ptr<ndn::Data>>& contentKeys)
{
    if (service.empty() || keyId.empty() || wrappedKey.empty() || content.empty() || contentKeys.empty())
        throw std::invalid_argument("incomplete original wrapped key publication");
    OriginalWrappedPackets prepared;
    prepared.service = service;
    const auto retain = [&prepared](const auto& input, auto& output) {
        for (const auto& packet : input) {
            if (!packet || packet->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
                throw std::invalid_argument("invalid original wrapped key packet");
            prepared.wireBytes += packet->wireEncode().size();
            output.push_back(std::make_shared<const ndn::Data>(*packet));
        }
    };
    retain(content, prepared.content);
    retain(contentKeys, prepared.contentKeys);
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = m_wrappedSendPackets.find(keyId);
    if (existing != m_wrappedSendPackets.end()) {
        const auto same = [](const auto& a, const auto& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (!(a[i]->wireEncode() == b[i]->wireEncode())) return false;
            return true;
        };
        const auto cipher = m_wrappedSendKeysById.find(keyId);
        if (cipher == m_wrappedSendKeysById.end() || cipher->second != wrappedKey ||
            existing->second.service != service || !same(existing->second.content, prepared.content) ||
            !same(existing->second.contentKeys, prepared.contentKeys))
            throw std::invalid_argument("immutable wrapped key original publication conflict");
        return;
    }
    const auto count = prepared.content.size() + prepared.contentKeys.size();
    if (prepared.wireBytes > 64ULL * 1024 * 1024 - m_wrappedPacketBytes ||
        count > 8192 - m_wrappedPacketCount)
        throw std::runtime_error("original wrapped key publication capacity exhausted");
    const auto oldCipher = m_wrappedSendKeysById.find(keyId);
    const auto oldService = m_wrappedSendKeyServices.find(keyId);
    if ((oldCipher != m_wrappedSendKeysById.end() && oldCipher->second != wrappedKey) ||
        (oldService != m_wrappedSendKeyServices.end() && oldService->second != service))
        throw std::invalid_argument("immutable wrapped key context conflict");
    bool insertedCipher = false, insertedWrapped = false, insertedService = false, insertedReference = false;
    try {
        insertedCipher = m_wrappedSendKeysById.emplace(keyId, wrappedKey).second;
        insertedWrapped = m_wrappedSendKeys.insert(keyId).second;
        insertedService = m_wrappedSendKeyServices.emplace(keyId, service).second;
        insertedReference = m_wrappedSendKeyReferences.emplace(keyId, 0).second;
        // Allocate all metadata before acceptance; any allocation failure rolls
        // back only this attempt's entries, preserving previously held owners.
        m_wrappedSendPackets.emplace(keyId, std::move(prepared));
    }
    catch (...) {
        if (insertedCipher) m_wrappedSendKeysById.erase(keyId);
        if (insertedWrapped) m_wrappedSendKeys.erase(keyId);
        if (insertedService) m_wrappedSendKeyServices.erase(keyId);
        if (insertedReference) m_wrappedSendKeyReferences.erase(keyId);
        throw;
    }
    m_wrappedPacketBytes += m_wrappedSendPackets.at(keyId).wireBytes;
    m_wrappedPacketCount += count;
    ++m_wrappedSendKeyReferences.at(keyId);
}

bool
HybridMessageCrypto::getWrappedSendPackets(const std::string& keyId,
  std::vector<std::shared_ptr<ndn::Data>>& content,
  std::vector<std::shared_ptr<ndn::Data>>& contentKeys) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_wrappedSendPackets.find(keyId);
    if (found == m_wrappedSendPackets.end()) return false;
    content.clear(); contentKeys.clear();
    for (const auto& original : found->second.content)
        content.push_back(std::make_shared<ndn::Data>(*original));
    for (const auto& original : found->second.contentKeys)
        contentKeys.push_back(std::make_shared<ndn::Data>(*original));
    return true;
}

std::shared_ptr<const ndn::Data>
HybridMessageCrypto::findWrappedSendData(const ndn::Interest& interest) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& entry : m_wrappedSendPackets) {
        for (const auto* packets : {&entry.second.content, &entry.second.contentKeys}) {
            for (const auto& packet : *packets) {
                if (packet && interest.matchesData(*packet)) return packet;
            }
        }
    }
    return nullptr;
}

void
HybridMessageCrypto::eraseWrappedPacketsLocked(const std::string& keyId)
{
    const auto found = m_wrappedSendPackets.find(keyId);
    if (found == m_wrappedSendPackets.end()) return;
    m_wrappedPacketBytes -= found->second.wireBytes;
    m_wrappedPacketCount -= found->second.content.size() + found->second.contentKeys.size();
    m_wrappedSendPackets.erase(found);
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

bool
HybridMessageCrypto::retainWrappedSendKey(const ndn::Name& serviceName,
                                           const std::string& keyId)
{
    if (serviceName.empty() || keyId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto scope = m_wrappedSendKeyServices.find(keyId);
    if (scope == m_wrappedSendKeyServices.end() || scope->second != serviceName ||
        m_wrappedSendKeysById.find(keyId) == m_wrappedSendKeysById.end()) {
        return false;
    }
    ++m_wrappedSendKeyReferences[keyId];
    return true;
}

bool
HybridMessageCrypto::eraseWrappedSendKey(const ndn::Name& serviceName,
                                          const std::string& keyId)
{
    if (serviceName.empty() || keyId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto scope = m_wrappedSendKeyServices.find(keyId);
    if (scope == m_wrappedSendKeyServices.end() || scope->second != serviceName) {
        return false;
    }
    auto references = m_wrappedSendKeyReferences.find(keyId);
    if (references != m_wrappedSendKeyReferences.end() && references->second > 1) {
        --references->second;
        return false;
    }
    if (references != m_wrappedSendKeyReferences.end()) {
        m_wrappedSendKeyReferences.erase(references);
    }
    eraseWrappedPacketsLocked(keyId);
    m_wrappedSendKeys.erase(keyId);
    const auto wrapped = m_wrappedSendKeysById.find(keyId);
    if (wrapped != m_wrappedSendKeysById.end()) {
        std::fill(wrapped->second.begin(), wrapped->second.end(), 0);
        m_wrappedSendKeysById.erase(wrapped);
    }
    m_wrappedSendKeyServices.erase(scope);
    return true;
}

size_t
HybridMessageCrypto::invalidateService(const ndn::Name& serviceName)
{
    if (serviceName.empty()) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_wrappedSendPackets.begin(); it != m_wrappedSendPackets.end();) {
        if (it->second.service != serviceName) { ++it; continue; }
        const auto keyId = it->first;
        ++it;
        eraseWrappedPacketsLocked(keyId);
    }

    size_t removed = 0;
    for (auto it = m_sendKeys.begin(); it != m_sendKeys.end();) {
        const auto serviceIt = m_sendKeyServices.find(it->first);
        if (serviceIt == m_sendKeyServices.end() || serviceIt->second != serviceName) {
            ++it;
            continue;
        }
        const auto keyId = it->second.keyId;
        std::fill(it->second.key.begin(), it->second.key.end(), 0);
        it = m_sendKeys.erase(it);
        m_sendKeyServices.erase(serviceIt);
        m_wrappedSendKeys.erase(keyId);
        auto wrappedIt = m_wrappedSendKeysById.find(keyId);
        if (wrappedIt != m_wrappedSendKeysById.end()) {
            std::fill(wrappedIt->second.begin(), wrappedIt->second.end(), 0);
            m_wrappedSendKeysById.erase(wrappedIt);
        }
        m_wrappedSendKeyServices.erase(keyId);
        m_wrappedSendKeyReferences.erase(keyId);
        ++removed;
    }

    for (auto it = m_receiveKeys.begin(); it != m_receiveKeys.end();) {
        if (it->second.serviceName != serviceName) {
            ++it;
            continue;
        }
        std::fill(it->second.key.begin(), it->second.key.end(), 0);
        it = m_receiveKeys.erase(it);
        ++removed;
    }

    // Wrapped entries can outlive the corresponding rotating send-key scope.
    // Remove only entries carrying an explicit matching service scope; leave
    // legacy unscoped entries untouched to avoid collateral invalidation.
    for (auto it = m_wrappedSendKeyServices.begin();
         it != m_wrappedSendKeyServices.end();) {
        if (it->second != serviceName) {
            ++it;
            continue;
        }
        const auto keyId = it->first;
        m_wrappedSendKeys.erase(keyId);
        auto wrappedIt = m_wrappedSendKeysById.find(keyId);
        if (wrappedIt != m_wrappedSendKeysById.end()) {
            std::fill(wrappedIt->second.begin(), wrappedIt->second.end(), 0);
            m_wrappedSendKeysById.erase(wrappedIt);
        }
        it = m_wrappedSendKeyServices.erase(it);
        m_wrappedSendKeyReferences.erase(keyId);
        ++removed;
    }

    return removed;
}

ndn::Name
makeHybridMessageKeyName(const ndn::Name& serviceName,
                         const ndn::Name& senderPrefix,
                         const std::string& accessAttribute,
                         const std::string& epochId)
{
    // Existing private cryptographic key identifier; this is never published Data.
    ndn::Name name("/NDNSF/HYBRID-KEY");
    name.append(serviceName).append("SENDER").append(senderPrefix)
        .append("ATTRIBUTE").append(ndn::Name(accessAttribute)).append(epochId);
    return name;
}

ndn::Name
makeHybridMessageKeyDataName(const ndn::Name& serviceName,
                             const ndn::Name& senderPrefix,
                             const std::string& accessAttribute,
                             const std::string& epochId)
{
    const auto bytes = [](const ndn::Name& value) {
        const auto& wire = value.wireEncode();
        return std::vector<uint8_t>(wire.data(), wire.data() + wire.size());
    };
    if (serviceName.empty() || senderPrefix.empty() || accessAttribute.empty() || epochId.empty())
        throw std::invalid_argument("incomplete hybrid key scope");
    const auto scope = computeNamingDigest("ndnsf-naming/hybrid-key-scope",
      {{1, 1, bytes(serviceName)}, {2, 1, bytes(senderPrefix)},
       {3, 2, std::vector<uint8_t>(accessAttribute.begin(), accessAttribute.end())}});
    const auto epoch = computeNamingDigest("ndnsf-naming/opaque/protection-epoch",
      {{1, 4, std::vector<uint8_t>(epochId.begin(), epochId.end())}});
    auto name = makeProducerDataPrefix(senderPrefix, "KEY");
    name.append("kind=HYBRID-MESSAGE").append("SCOPE").append("SERVICE")
        .append(makeNamingDigestComponent(scope)).append("EPOCH")
        .append(makeNamingDigestComponent(epoch)).append("OBJECT");
    checkNamingNameSize(name);
    return name;
}

std::vector<std::shared_ptr<ndn::Data>>
prepareHybridMessageKeyPackets(const ndn::Name& service, const ndn::Name& sender,
  const std::string& attribute, const std::string& epoch,
  ndn::span<const uint8_t> cipher, ndn::KeyChain& keyChain,
  const ndn::security::SigningInfo& signingInfo, ndn::time::milliseconds freshness,
  uint32_t contentType)
{
    if (cipher.empty() || cipher.size() > 65536)
        throw std::invalid_argument("hybrid wrapped key exceeds envelope limit");
    const auto digest = wrappedCipherDigest(cipher);
    auto name = makeHybridMessageKeyDataName(service, sender, attribute, epoch);
    name.append(makeNamingDigestComponent(digest));
    checkNamingNameSize(name);
    const auto payloadBudget = std::min<std::size_t>(8000,
      ndn::MAX_NDN_PACKET_SIZE - name.wireEncode().size() - 1024);
    ndn::Segmenter segmenter(keyChain, signingInfo);
    auto originals = segmenter.segment(cipher, name, payloadBudget, freshness, contentType);
    for (const auto& packet : originals) {
        checkNamingNameSize(packet->getName());
        if (packet->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
            throw std::invalid_argument("hybrid wrapped key signed packet exceeds wire limit");
    }
    return originals;
}

VerifiedHybridMessageKeyWrapperPacketSetV1
validateHybridMessageKeyWrapperPacketSet(
  const ndn::Name& locator,
  const std::vector<std::vector<uint8_t>>& packetWires,
  const std::function<bool(const ndn::Data&)>& authorizeProducer)
{
    constexpr std::size_t MaxSegments = 128;
    constexpr std::size_t MaxCipherBytes = 65536;
    if (locator.empty())
        throw std::invalid_argument("hybrid key wrapper locator is empty");
    checkNamingNameSize(locator);
    if (packetWires.empty() || packetWires.size() > MaxSegments)
        throw std::invalid_argument("hybrid key wrapper segment budget exceeded");

    ndn::Name object;
    ndn::name::Component commitment;
    std::optional<uint64_t> finalSegment;
    std::map<uint64_t, std::vector<uint8_t>> contents;
    std::map<uint64_t, std::pair<ndn::Name, std::string>> wireFingerprints;
    std::size_t totalContentBytes = 0;

    for (const auto& wire : packetWires) {
        if (wire.empty() || wire.size() > ndn::MAX_NDN_PACKET_SIZE)
            throw std::invalid_argument("hybrid key wrapper packet wire size invalid");
        ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
        const auto& encoded = packet.wireEncode();
        if (encoded.size() != wire.size() ||
            !std::equal(encoded.begin(), encoded.end(), wire.begin()))
            throw std::invalid_argument("hybrid key wrapper packet wire changed during parse");

        bool authorized = false;
        try { authorized = authorizeProducer && authorizeProducer(packet); }
        catch (const std::exception&) { }
        if (!authorized)
            throw std::invalid_argument("hybrid key wrapper signer is not authorized for Producer");

        const auto& name = packet.getName();
        if (!locator.isPrefixOf(name) || name.size() != locator.size() + 2 ||
            name[-2].type() != 8 || name[-2].value_size() != 32 ||
            !name[-1].isSegment() || !packet.getFinalBlock() ||
            !packet.getFinalBlock()->isSegment())
            throw std::invalid_argument("hybrid key wrapper namespace or geometry mismatch");

        const auto segment = name[-1].toSegment();
        const auto final = packet.getFinalBlock()->toSegment();
        if (final >= MaxSegments || segment > final ||
            name[-1] != ndn::name::Component::fromSegment(segment) ||
            *packet.getFinalBlock() != ndn::name::Component::fromSegment(final))
            throw std::invalid_argument("hybrid key wrapper segment budget exceeded");
        if (finalSegment && *finalSegment != final)
            throw std::invalid_argument("hybrid key wrapper FinalBlock changed during packet-set validation");

        const auto packetObject = name.getPrefix(-1);
        if (object.empty()) {
            object = packetObject;
            commitment = name[-2];
            finalSegment = final;
        }
        else if (object != packetObject || commitment != name[-2]) {
            throw std::invalid_argument("hybrid key wrapper object changed during packet-set validation");
        }

        const auto content = packet.getContent();
        if (content.value_size() > MaxCipherBytes - totalContentBytes)
            throw std::invalid_argument("hybrid key wrapper exceeds envelope limit");
        std::vector<uint8_t> payload;
        if (content.value_size() != 0)
            payload.assign(content.value(), content.value() + content.value_size());
        if (!contents.emplace(segment, std::move(payload)).second)
            throw std::invalid_argument("hybrid key wrapper duplicate segment");
        auto wireSpan = ndn::span<const uint8_t>(wire.data(), wire.size());
        if (!wireFingerprints.emplace(segment,
              std::make_pair(name, sha256Hex(wireSpan))).second)
            throw std::invalid_argument("hybrid key wrapper duplicate segment");
        totalContentBytes += content.value_size();
    }

    if (!finalSegment || contents.size() != *finalSegment + 1 ||
        totalContentBytes == 0 || totalContentBytes > MaxCipherBytes)
        throw std::invalid_argument("hybrid key wrapper packet set is incomplete");

    std::vector<uint8_t> cipher;
    cipher.reserve(totalContentBytes);
    for (uint64_t segment = 0; segment <= *finalSegment; ++segment) {
        const auto found = contents.find(segment);
        if (found == contents.end())
            throw std::invalid_argument("hybrid key wrapper packet set is incomplete");
        cipher.insert(cipher.end(), found->second.begin(), found->second.end());
    }
    if (commitment != makeNamingDigestComponent(wrappedCipherDigest(
          ndn::span<const uint8_t>(cipher.data(), cipher.size()))))
        throw std::invalid_argument("hybrid key wrapper cipher commitment mismatch");

    ndn::Block cipherBlock(ndn::span<const uint8_t>(cipher.data(), cipher.size()));
    try {
        if (cipherBlock.size() != cipher.size())
            throw std::invalid_argument("trailing NAC block bytes");
        cipherBlock.parse();
    }
    catch (const std::exception&) {
        throw std::invalid_argument("hybrid key wrapper is not a valid NAC Block");
    }
    std::vector<std::pair<ndn::Name, std::string>> packetFingerprints;
    packetFingerprints.reserve(wireFingerprints.size());
    for (auto& entry : wireFingerprints)
        packetFingerprints.push_back(std::move(entry.second));
    return VerifiedHybridMessageKeyWrapperPacketSetV1(std::move(object),
                                                       std::move(cipherBlock),
                                                       std::move(packetFingerprints));
}

VerifiedHybridMessageKeyEnvelopeV1
validateHybridMessageKeyWrapperEnvelopeV1(
  const VerifiedHybridMessageKeyWrapperPacketSetV1& packetSet)
{
    const auto& encoded = packetSet.cipherBlock();
    try {
        if (encoded.type() != ndn::tlv::Content || encoded.size() == 0 || encoded.size() > 65536)
            throw std::invalid_argument("hybrid key wrapper NAC Content size or type invalid");

        const auto& fields = encoded.elements();
        if (fields.size() != 3 ||
            fields[0].type() != ndn::nacabe::TLV_EncryptedContent ||
            fields[1].type() != ndn::nacabe::TLV_PlainTextSize ||
            fields[2].type() != ndn::tlv::Name ||
            fields[0].value_size() == 0 || fields[0].value_size() > 65536)
            throw std::invalid_argument("hybrid key wrapper NAC fields invalid");

        const auto plaintextBytes = ndn::readNonNegativeInteger(fields[1]);
        if (plaintextBytes != HybridMessageCrypto::MESSAGE_KEY_SIZE)
            throw std::invalid_argument("hybrid key wrapper plaintext key size invalid");

        ndn::Name contentKeyName(fields[2]);
        if (contentKeyName.empty() || contentKeyName.wireEncode().size() > 4096)
            throw std::invalid_argument("hybrid key wrapper CK name invalid");

        ndn::Block canonical(ndn::tlv::Content);
        canonical.push_back(ndn::makeBinaryBlock(
          ndn::nacabe::TLV_EncryptedContent, fields[0].value_bytes()));
        canonical.push_back(ndn::makeNonNegativeIntegerBlock(
          ndn::nacabe::TLV_PlainTextSize, plaintextBytes));
        canonical.push_back(contentKeyName.wireEncode());
        canonical.encode();
        if (canonical.size() != encoded.size() ||
            !std::equal(canonical.begin(), canonical.end(), encoded.begin()))
            throw std::invalid_argument("hybrid key wrapper NAC Content is not canonical");

        return VerifiedHybridMessageKeyEnvelopeV1(
          packetSet.objectName(), packetSet.packetFingerprints(),
          std::move(contentKeyName), plaintextBytes, fields[0].value_size());
    }
    catch (const std::invalid_argument&) {
        throw;
    }
    catch (const std::exception&) {
        throw std::invalid_argument("hybrid key wrapper NAC envelope malformed");
    }
}

VerifiedNacContentKeyPacketSetV1
validateNacContentKeyPacketSetV1(
  const ndn::Name& contentKeyName, const ndn::Name& producerPrefix,
  const std::vector<std::vector<uint8_t>>& packetWires,
  const std::function<bool(const ndn::Data&)>& authorizeProducer)
{
    constexpr std::size_t MaxPacketCount = 65536;
    if (contentKeyName.empty() || contentKeyName[-1].isSegment() ||
        producerPrefix.empty() || !producerPrefix.isPrefixOf(contentKeyName) ||
        packetWires.empty() || packetWires.size() > MaxPacketCount || !authorizeProducer)
        throw std::invalid_argument("NAC CK packet-set request invalid");

    std::map<std::uint64_t, std::pair<ndn::Name, std::string>> segments;
    std::optional<std::uint64_t> finalSegment;
    std::optional<std::uint32_t> contentType;
    std::optional<ndn::time::milliseconds> freshness;
    std::uint64_t contentBytes = 0;
    for (const auto& wire : packetWires) {
        if (wire.empty() || wire.size() > ndn::MAX_NDN_PACKET_SIZE)
            throw std::invalid_argument("NAC CK packet wire size invalid");
        ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
        const auto& encoded = packet.wireEncode();
        if (encoded.size() != wire.size() ||
            !std::equal(encoded.begin(), encoded.end(), wire.begin()))
            throw std::invalid_argument("NAC CK packet wire changed during parse");

        const auto& name = packet.getName();
        if (name == contentKeyName || contentKeyName.isPrefixOf(name)) {
            if (name.size() != contentKeyName.size() + 1 || !name[-1].isSegment() ||
                name[-1] != ndn::name::Component::fromSegment(name[-1].toSegment()) ||
                !producerPrefix.isPrefixOf(name) || !packet.getFinalBlock() ||
                !packet.getFinalBlock()->isSegment() ||
                *packet.getFinalBlock() != ndn::name::Component::fromSegment(
                  packet.getFinalBlock()->toSegment()))
                throw std::invalid_argument("NAC CK packet name or segment geometry invalid");

            const auto segment = name[-1].toSegment();
            const auto final = packet.getFinalBlock()->toSegment();
            if (final >= MaxPacketCount || segment > final ||
                (finalSegment && *finalSegment != final))
                throw std::invalid_argument("NAC CK packet FinalBlock mismatch");
            bool authorized = false;
            try { authorized = authorizeProducer(packet); }
            catch (...) { }
            if (!authorized)
                throw std::invalid_argument("NAC CK signer is not authorized for Producer");

            const auto type = packet.getContentType();
            const auto period = packet.getFreshnessPeriod();
            if ((contentType && *contentType != type) ||
                (freshness && *freshness != period))
                throw std::invalid_argument("NAC CK segment metadata changed");
            contentType = type;
            freshness = period;

            const auto bytes = packet.getContent().value_size();
            const auto maxContentBytes =
              static_cast<std::uint64_t>(MaxPacketCount) * ndn::MAX_NDN_PACKET_SIZE;
            if (bytes == 0 || contentBytes > maxContentBytes ||
                bytes > maxContentBytes - contentBytes)
                throw std::invalid_argument("NAC CK packet content budget exceeded");
            auto wireSpan = ndn::span<const uint8_t>(wire.data(), wire.size());
            if (!segments.emplace(segment, std::make_pair(name, sha256Hex(wireSpan))).second)
                throw std::invalid_argument("NAC CK duplicate segment");
            contentBytes += bytes;
            finalSegment = final;
        }
    }

    if (!finalSegment || segments.empty() || segments.size() != *finalSegment + 1 ||
        segments.begin()->first != 0 || segments.rbegin()->first != *finalSegment)
        throw std::invalid_argument("NAC CK packet set incomplete");
    std::vector<std::pair<ndn::Name, std::string>> fingerprints;
    fingerprints.reserve(segments.size());
    for (auto& entry : segments)
        fingerprints.push_back(std::move(entry.second));
    return VerifiedNacContentKeyPacketSetV1(contentKeyName, producerPrefix,
                                             std::move(fingerprints), contentBytes);
}

void
requireOriginalPacketFingerprintsPresentV1(
  const std::vector<std::string>& packetNames,
  const std::vector<std::vector<uint8_t>>& packetWires,
  const std::vector<std::pair<ndn::Name, std::string>>& requiredPackets,
  const std::function<void()>& requireActive)
{
    constexpr std::size_t MaxPacketCount = 65536;
    constexpr std::size_t MaxNameBytes = 16ULL << 20;
    if (packetNames.empty() || packetNames.size() != packetWires.size() ||
        packetNames.size() > MaxPacketCount || requiredPackets.empty() ||
        requiredPackets.size() > MaxPacketCount)
        throw std::invalid_argument("original packet bundle index invalid");

    std::map<ndn::Name, std::size_t> packetIndexes;
    std::size_t nameBytes = 0;
    for (std::size_t index = 0; index < packetNames.size(); ++index) {
        if (requireActive)
            requireActive();
        if (packetNames[index].empty() || packetNames[index].size() > 4096 ||
            packetNames[index].size() > MaxNameBytes - nameBytes)
            throw std::invalid_argument("original packet bundle name budget exceeded");
        nameBytes += packetNames[index].size();
        const ndn::Name name(packetNames[index]);
        if (name.empty() ||
            (name.toUri(ndn::name::UriFormat::CANONICAL) != packetNames[index] &&
             name.toUri(ndn::name::UriFormat::ALTERNATE) != packetNames[index]) ||
            !packetIndexes.emplace(name, index).second)
            throw std::invalid_argument("original packet bundle name index invalid");
    }

    std::set<ndn::Name> uniqueRequiredNames;
    for (const auto& [name, expectedDigest] : requiredPackets) {
        if (requireActive)
            requireActive();
        if (name.empty() || expectedDigest.size() != 64 ||
            !std::all_of(expectedDigest.begin(), expectedDigest.end(), [](char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }) || !uniqueRequiredNames.insert(name).second)
            throw std::invalid_argument("required original packet fingerprint invalid");
        const auto found = packetIndexes.find(name);
        if (found == packetIndexes.end())
            throw std::invalid_argument("required original packet is absent from bundle");

        const auto& wire = packetWires[found->second];
        if (wire.empty() || wire.size() > ndn::MAX_NDN_PACKET_SIZE)
            throw std::invalid_argument("required original packet wire size invalid");
        ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
        const auto& encoded = packet.wireEncode();
        if (packet.getName() != name || encoded.size() != wire.size() ||
            !std::equal(encoded.begin(), encoded.end(), wire.begin()) ||
            sha256Hex(ndn::span<const uint8_t>(wire.data(), wire.size())) != expectedDigest)
            throw std::invalid_argument("required original packet wire differs from bundle");
    }
    if (requireActive)
        requireActive();
}

static void
fetchHybridMessageKeyWrapperImpl(ndn::Face& face, ndn::security::Validator& validator,
  const ndn::Name& locator,
  std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Name&, const ndn::Block&)> success,
  std::function<void(const std::string&)> failure,
  std::optional<HybridKeyFetchOptions> route)
{
    struct State : std::enable_shared_from_this<State> {
        explicit State(boost::asio::io_context& io) : timer(io) {}
        bool done = false;
        ndn::Name object;
        std::optional<uint64_t> finalSegment;
        std::map<uint64_t, std::vector<uint8_t>> wires;
        std::size_t bytes = 0;
        std::weak_ptr<ndn::SegmentFetcher> fetcher;
        boost::asio::steady_timer timer;

        bool current(const HybridKeyFetchOptions& options) const {
            try {
                return options.isCurrent && options.isCurrent() &&
                    std::chrono::steady_clock::now() < options.deadline;
            }
            catch (...) { return false; }
        }
        void poll(const HybridKeyFetchOptions& options,
                  const std::function<void(const std::string&)>& reject) {
            timer.expires_at(std::min(options.deadline,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
            timer.async_wait([self = shared_from_this(), options, reject](const auto& error) {
                if (error || self->done) return;
                if (!self->current(options)) reject("hybrid key authority or deadline expired");
                else self->poll(options, reject);
            });
        }
    };
    checkNamingNameSize(locator);
    auto state = std::make_shared<State>(face.getIoContext());
    if (route && !state->current(*route)) {
        if (failure) failure("hybrid key authority or deadline expired");
        return;
    }
    ndn::Interest interest(locator);
    interest.setCanBePrefix(true);
    interest.setMustBeFresh(false);
    if (route && !route->forwardingHint.empty())
        interest.setForwardingHint(route->forwardingHint);
    ndn::SegmentFetcher::Options options;
    options.probeLatestVersion = false;
    options.flowControlWindow = 16;
    if (route) options.maxTimeout = ndn::time::milliseconds(std::max<int64_t>(1,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            route->deadline - std::chrono::steady_clock::now()).count()));
    auto fetcher = ndn::SegmentFetcher::start(face, interest, validator, options);
    state->fetcher = fetcher;
    const auto reject = [state, failure, &face](
                            const std::string& reason) {
        if (state->done) return;
        state->done = true;
        state->timer.cancel();
        if (auto owner = state->fetcher.lock()) owner->stop();
        // stop posts its self release. This acknowledgement runs after that
        // release, so any Interests created by the remainder of the installed
        // fetcher's current signal stack are cancelled by destruction first.
        boost::asio::post(face.getIoContext(), [state, failure, reason] {
            if (failure) failure(reason);
        });
    };
    if (route) state->poll(*route, reject);
    fetcher->afterSegmentValidated.connect(
      [state, locator, authorizeProducer, reject, route](const ndn::Data& packet) {
        if (state->done) return;
        if (route && !state->current(*route)) {
            reject("hybrid key authority or deadline expired"); return;
        }
        bool authorized = false;
        try { authorized = authorizeProducer && authorizeProducer(packet); }
        catch (const std::exception&) { }
        if (!authorized) { reject("hybrid key wrapper signer is not authorized for Producer"); return; }
        const auto& name = packet.getName();
        if (!locator.isPrefixOf(name) || name.size() != locator.size() + 2 ||
            name[-2].type() != 8 || name[-2].value_size() != 32 || !name[-1].isSegment() ||
            !packet.getFinalBlock() || !packet.getFinalBlock()->isSegment() ||
            packet.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE) {
            reject("hybrid key wrapper namespace or geometry mismatch"); return;
        }
        const auto segment = name[-1].toSegment();
        const auto final = packet.getFinalBlock()->toSegment();
        if (state->finalSegment && *state->finalSegment != final) {
            reject("hybrid key wrapper FinalBlock changed during fetch"); return;
        }
        if (final >= 128 || segment > final || name[-1] != ndn::name::Component::fromSegment(segment) ||
            *packet.getFinalBlock() != ndn::name::Component::fromSegment(final)) {
            reject("hybrid key wrapper segment budget exceeded"); return;
        }
        if (state->object.empty()) {
            state->finalSegment = final;
            state->object = name.getPrefix(-1);
        }
        if (state->object != name.getPrefix(-1)) {
            reject("hybrid key wrapper object changed during fetch"); return;
        }
        const auto existing = state->wires.find(segment);
        const auto encoded = packet.wireEncode();
        const std::vector<uint8_t> packetWire(encoded.begin(), encoded.end());
        if (existing != state->wires.end()) {
            if (existing->second != packetWire)
                reject("hybrid key wrapper immutable segment conflict");
            return;
        }
        if (packet.getContent().value_size() > 65536 - state->bytes) {
            reject("hybrid key wrapper exceeds envelope limit"); return;
        }
        state->bytes += packet.getContent().value_size();
        state->wires.emplace(segment, packetWire);
    });
    fetcher->onComplete.connect([state, success, reject, route, locator,
                                 authorizeProducer](ndn::ConstBufferPtr cipher) {
        if (state->done) return;
        if (route && !state->current(*route)) {
            reject("hybrid key authority or deadline expired"); return;
        }
        std::optional<VerifiedHybridMessageKeyWrapperPacketSetV1> verified;
        try {
            if (!cipher || cipher->empty() || cipher->size() > 65536 ||
                state->object.empty() || state->wires.empty()) {
                reject("hybrid key wrapper packet set is incomplete"); return;
            }
            std::vector<std::vector<uint8_t>> packetWires;
            packetWires.reserve(state->wires.size());
            for (const auto& [segment, wire] : state->wires) {
                (void)segment;
                packetWires.push_back(wire);
            }
            auto packetSet = validateHybridMessageKeyWrapperPacketSet(
                locator, packetWires, authorizeProducer);
            (void)validateHybridMessageKeyWrapperEnvelopeV1(packetSet);
            verified.emplace(std::move(packetSet));
            if (verified->objectName() != state->object ||
                !(verified->cipherBlock() == ndn::Block(
                    ndn::span<const uint8_t>(cipher->data(), cipher->size())))) {
                reject("hybrid key wrapper reassembled packet set mismatch"); return;
            }
            if (route && !state->current(*route)) {
                reject("hybrid key authority or deadline expired"); return;
            }
        }
        catch (const std::exception& error) { reject(error.what()); return; }
        state->done = true;
        state->timer.cancel();
        if (success) success(verified->objectName(), verified->cipherBlock());
    });
    fetcher->onError.connect([reject](uint32_t, const std::string& reason) {
        reject("hybrid key wrapper fetch failed: " + reason);
    });
}

void
fetchHybridMessageKeyWrapper(ndn::Face& face, ndn::security::Validator& validator,
  const ndn::Name& locator, std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Name&, const ndn::Block&)> success,
  std::function<void(const std::string&)> failure)
{
    fetchHybridMessageKeyWrapperImpl(face, validator, locator, std::move(authorizeProducer),
        std::move(success), std::move(failure), std::nullopt);
}

void
fetchHybridMessageKeyWrapper(ndn::Face& face, ndn::security::Validator& validator,
  const ndn::Name& locator, const HybridKeyFetchOptions& options,
  std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Name&, const ndn::Block&)> success,
  std::function<void(const std::string&)> failure)
{
    fetchHybridMessageKeyWrapperImpl(face, validator, locator, std::move(authorizeProducer),
        std::move(success), std::move(failure), options);
}

void
consumeHybridMessageKeyWrapper(ndn::Face& face, ndn::security::Validator& validator,
  ndn::nacabe::Consumer& consumer, const ndn::Name& locator,
  const HybridKeyFetchOptions& options,
  std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Buffer&)> success,
  std::function<void(const std::string&)> failure)
{
    const auto unwrapSuccess = [success = std::move(success)](
                                   const ndn::Buffer& key) {
        if (success) success(key);
    };
    const auto unwrapFailure = [failure](const std::string& reason) {
        if (failure) failure(reason);
    };
    fetchHybridMessageKeyWrapper(face, validator, locator, options, std::move(authorizeProducer),
      [&consumer, options, unwrapSuccess, unwrapFailure](
          const ndn::Name& object, const ndn::Block& cipher) mutable {
        try {
            ndn::Interest interest(object);
            interest.setForwardingHint(options.forwardingHint);
            ndn::nacabe::Consumer::CkFetchOptions limits;
            limits.deadline = options.deadline;
            limits.isCurrent = options.isCurrent;
            consumer.consume(interest, cipher, limits, unwrapSuccess, unwrapFailure);
        }
        catch (const std::exception& e) { unwrapFailure(e.what()); }
      }, failure);
}

void
consumeHybridMessageKeyWrapper(ndn::Face& face, ndn::security::Validator& validator,
  ndn::nacabe::Consumer& consumer, const ndn::Name& locator,
  std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Buffer&)> success,
  std::function<void(const std::string&)> failure)
{
    fetchHybridMessageKeyWrapper(face, validator, locator, std::move(authorizeProducer),
      [&consumer, success = std::move(success), failure](const ndn::Name& object,
                                                       const ndn::Block& cipher) mutable {
        try { consumer.consume(object, cipher, std::move(success), failure); }
        catch (const std::exception& e) { if (failure) failure(e.what()); }
      }, failure);
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
        for (const auto* type : {"REQUEST", "ACK", "SELECTION", "RESPONSE", "EVENT"}) {
            if (component == ndn::name::Component(type)) return type;
        }
    }
    return "UNKNOWN";
}

std::string
hybridMessageTypeForName(const ndn::Name& name, const ndn::Name& actor)
{
    if (actor.empty() || !actor.isPrefixOf(name) || name.size() <= actor.size() + 1 ||
        name[actor.size()] != ndn::name::Component("NDNSF")) return "UNKNOWN";
    const auto& category = name[actor.size() + 1];
    if (category == ndn::name::Component("event")) return "EVENT";
    for (const auto* value : {"REQUEST", "ACK", "SELECTION", "RESPONSE", "EVENT"}) {
        if (category == ndn::name::Component(value)) return value;
    }
    return "UNKNOWN";
}

std::string
makeAbeServiceAttribute(const ndn::Name& service, const std::string& kind)
{
    if (service.empty() || (kind != "SERVICE" && kind != "PERMISSION"))
        throw std::invalid_argument("invalid ABE service attribute context");
    const auto spelling = service.toUri(ndn::name::UriFormat::ALTERNATE);
    if (!std::all_of(spelling.begin(), spelling.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.';
    })) throw std::invalid_argument("service Name is not supported by the OpenABE attribute DSL");
    return "/" + kind + spelling;
}

std::string
hybridAccessAttributeForName(const ndn::Name& name, const ndn::Name& service,
                            const ndn::Name& actor)
{
    const auto type = hybridMessageTypeForName(name, actor);
    if (type == "UNKNOWN") throw std::invalid_argument("unknown contextual hybrid message category");
    // OpenABE attributes retain their existing readable syntax independently
    // of the process-wide Name URI preference.
    return makeAbeServiceAttribute(service,
      type == "REQUEST" || type == "SELECTION" ? "SERVICE" : "PERMISSION");
}

std::string
hybridAccessAttributeForName(const ndn::Name& name, const ndn::Name& serviceName)
{
    const auto type = hybridMessageTypeForName(name);
    if (type == "REQUEST" || type == "SELECTION") {
        return makeAbeServiceAttribute(serviceName, "SERVICE");
    }
    return makeAbeServiceAttribute(serviceName, "PERMISSION");
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
    const std::string text = messageName.toUri(ndn::name::UriFormat::CANONICAL) + "|" + messageType + "|" +
                             requestId.toUri(ndn::name::UriFormat::CANONICAL) + "|" + serviceName.toUri(ndn::name::UriFormat::CANONICAL) + "|" +
                             senderPrefix.toUri(ndn::name::UriFormat::CANONICAL) + "|" + hybridCompactKeyId(keyId) + "|" + epochId;
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
    const std::string text = dataName.toUri(ndn::name::UriFormat::CANONICAL) + "|SECURE-SELECTION-STATUS|" +
      std::to_string(version) + "|" + statusHandle + "|" +
      requesterIdentity.toUri(ndn::name::UriFormat::CANONICAL) + "|" + providerIdentity.toUri(ndn::name::UriFormat::CANONICAL) + "|" +
      std::to_string(attempt) + "|" + hybridCompactKeyId(keyId) + "|" + epochId;
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
      requester.toUri(ndn::name::UriFormat::CANONICAL) + "|" + serviceName.toUri(ndn::name::UriFormat::CANONICAL) + "|" + requestId.toUri(ndn::name::UriFormat::CANONICAL);
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
    result.setField("recipient", recipient.toUri(ndn::name::UriFormat::CANONICAL));
    result.setField("recipientCertName", recipientCertName.toUri(ndn::name::UriFormat::CANONICAL));
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
      requester.toUri(ndn::name::UriFormat::CANONICAL) + "|" + provider.toUri(ndn::name::UriFormat::CANONICAL) + "|" + serviceName.toUri(ndn::name::UriFormat::CANONICAL) +
      "|" + requestId.toUri(ndn::name::UriFormat::CANONICAL) + "|" + reservationId + "|" + planDigest;
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
        encrypted.getField("recipient") != expectedRecipient.toUri(ndn::name::UriFormat::CANONICAL) ||
        !encrypted.hasField("recipientCertName") ||
        encrypted.getField("recipientCertName") != expectedRecipientCertName.toUri(ndn::name::UriFormat::CANONICAL) ||
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
