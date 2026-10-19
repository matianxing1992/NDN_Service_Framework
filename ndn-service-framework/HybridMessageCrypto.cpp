#include "HybridMessageCrypto.hpp"
#include "utils.hpp"
#include "ProducerDataNaming.hpp"
#include <ndn-cxx/util/segmenter.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/scope_exit.hpp>
#include <nac-abe/consumer.hpp>

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
wrappedCipherDigest(ndn::span<const uint8_t> cipher)
{
    uint8_t digest[SHA256_DIGEST_LENGTH];
    if (!SHA256(cipher.data(), cipher.size(), digest))
        throw std::runtime_error("hybrid wrapper digest failed");
    return hexEncode(ndn::span<const uint8_t>(digest, sizeof(digest)));
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

void
fetchHybridMessageKeyWrapper(ndn::Face& face, ndn::security::Validator& validator,
  const ndn::Name& locator,
  std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Name&, const ndn::Block&)> success,
  std::function<void(const std::string&)> failure)
{
    fetchHybridMessageKeyWrapper(face, validator, locator, std::move(authorizeProducer),
      std::move(success), std::move(failure), {});
}

void
fetchHybridMessageKeyWrapper(ndn::Face& face, ndn::security::Validator& validator,
  const ndn::Name& locator,
  std::function<bool(const ndn::Data&)> authorizeProducer,
  std::function<void(const ndn::Name&, const ndn::Block&)> success,
  std::function<void(const std::string&)> failure,
  std::function<bool()> current)
{
    struct State : std::enable_shared_from_this<State> {
        boost::asio::io_context* io = nullptr;
        std::unique_ptr<ndn::Scheduler> scheduler;
        ndn::scheduler::EventId guard;
        std::function<bool()> current;
        std::function<void(const std::string&)> reject;
        void poll() {
            std::weak_ptr<State> weakSelf = shared_from_this();
            guard = scheduler->schedule(ndn::time::milliseconds(20), [weakSelf] {
                auto self = weakSelf.lock();
                if (!self) return;
                BOOST_SCOPE_EXIT_ALL(self) {
                    boost::asio::post(*self->io, [self] {});
                };
                if (self->done) return;
                bool valid = false;
                try { valid = self->current(); } catch (...) {}
                if (!valid) self->reject("hybrid key wrapper operation canceled");
                else self->poll();
            });
        }
        bool done = false;
        ndn::Name object;
        ndn::name::Component commitment;
        std::optional<uint64_t> finalSegment;
        std::map<uint64_t, ndn::Block> wires;
        std::size_t bytes = 0;
        std::weak_ptr<ndn::SegmentFetcher> fetcher;
    };
    if (current) {
        bool valid = false;
        try { valid = current(); } catch (...) {}
        if (!valid) {
            boost::asio::post(face.getIoContext(), [failure] {
                if (failure) failure("hybrid key wrapper operation canceled");
            });
            return;
        }
    }
    checkNamingNameSize(locator);
    auto state = std::make_shared<State>();
    ndn::Interest interest(locator);
    interest.setCanBePrefix(true);
    interest.setMustBeFresh(false);
    ndn::SegmentFetcher::Options options;
    options.probeLatestVersion = false;
    options.flowControlWindow = 16;
    auto fetcher = ndn::SegmentFetcher::start(face, interest, validator, options);
    state->fetcher = fetcher;
    std::weak_ptr<State> weakState = state;
    state->reject = [weakState, failure, &face](const std::string& reason) {
        auto active = weakState.lock();
        if (!active || active->done) return;
        active->done = true;
        active->guard.cancel();
        auto weakFetcher = active->fetcher;
        // Leave the emitting signal before stopping it. Acknowledge after the
        // fetcher's posted self-release has canceled any remaining Interests.
        boost::asio::post(face.getIoContext(), [weakFetcher, failure, reason, &face] {
            if (auto f = weakFetcher.lock()) f->stop();
            boost::asio::post(face.getIoContext(), [failure, reason] {
                if (failure) failure(reason);
            });
        });
    };
    const auto reject = [state](const std::string& reason) { state->reject(reason); };
    if (current) {
        state->current = std::move(current);
        state->io = &face.getIoContext();
        state->scheduler = std::make_unique<ndn::Scheduler>(*state->io);
        bool valid = false;
        try { valid = state->current(); } catch (...) {}
        if (!valid) { state->reject("hybrid key wrapper operation canceled"); return; }
        state->poll();
    }
    fetcher->afterSegmentValidated.connect([state, locator, authorizeProducer, reject](const ndn::Data& packet) {
        if (state->done) return;
        if (state->current) {
            bool valid = false;
            try { valid = state->current(); } catch (...) {}
            if (!valid) { state->reject("hybrid key wrapper operation canceled"); return; }
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
            state->commitment = name[-2];
        }
        if (state->object != name.getPrefix(-1)) {
            reject("hybrid key wrapper object changed during fetch"); return;
        }
        const auto existing = state->wires.find(segment);
        if (existing != state->wires.end()) {
            if (!(existing->second == packet.wireEncode()))
                reject("hybrid key wrapper immutable segment conflict");
            return;
        }
        if (packet.getContent().value_size() > 65536 - state->bytes) {
            reject("hybrid key wrapper exceeds envelope limit"); return;
        }
        state->bytes += packet.getContent().value_size();
        state->wires.emplace(segment, packet.wireEncode());
    });
    fetcher->onComplete.connect([state, success, reject](ndn::ConstBufferPtr cipher) {
        if (state->done) return;
        if (state->current) {
            bool valid = false;
            try { valid = state->current(); } catch (...) {}
            if (!valid) { state->reject("hybrid key wrapper operation canceled"); return; }
        }
        std::optional<ndn::Block> block;
        try {
            if (!cipher || cipher->empty() || cipher->size() > 65536 || state->object.empty() ||
                state->commitment != makeNamingDigestComponent(wrappedCipherDigest(
                  ndn::span<const uint8_t>(cipher->data(), cipher->size())))) {
                reject("hybrid key wrapper cipher commitment mismatch"); return;
            }
            block.emplace(ndn::span<const uint8_t>(cipher->data(), cipher->size()));
            if (block->size() != cipher->size()) {
                reject("hybrid key wrapper is not a valid NAC Block"); return;
            }
            block->parse();
        }
        catch (const std::exception&) { reject("hybrid key wrapper is not a valid NAC Block"); return; }
        state->done = true;
        state->guard.cancel();
        if (success) success(state->object, *block);
    });
    fetcher->onError.connect([reject](uint32_t, const std::string& reason) {
        reject("hybrid key wrapper fetch failed: " + reason);
    });
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
