#include "ProviderDurableLargeDataPublisher.hpp"

#include "HybridMessageCrypto.hpp"
#include "ProtectedBlobManifest.hpp"
#include "ProducerDataNaming.hpp"
#include "ServiceProvider.hpp"
#include "detail/OriginalPacketFile.hpp"
#include "utils.hpp"

#include <boost/asio/post.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <unistd.h>

namespace ndn_service_framework {
namespace {

namespace tlv = ndn_service_framework::tlv;
constexpr std::size_t SegmentBytes = 7000;
constexpr std::uintmax_t EnvelopeOverheadBudget = 64ULL * 1024ULL;
constexpr std::uintmax_t PacketRecordBudget =
    ndn::MAX_NDN_PACKET_SIZE + 2 * sizeof(std::uint64_t);

struct ProtectedBlobIdentity
{
    ndn::Name dataPrefix;
    ndn::Buffer nonce;
    std::string representationSpec;
    std::string protectionSpec;
};

std::string
digestHex(const std::uint8_t* digest, std::size_t digestSize)
{
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digestSize * 2);
    for (std::size_t i = 0; i < digestSize; ++i) {
        result.push_back(Hex[digest[i] >> 4]);
        result.push_back(Hex[digest[i] & 0x0f]);
    }
    return result;
}

std::string
hexDigest(const std::uint8_t* bytes, std::size_t size)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    if (EVP_Digest(bytes, size, digest.data(), &digestSize, EVP_sha256(), nullptr) != 1 ||
        digestSize != 32) {
        throw std::runtime_error("SHA-256 digest failed");
    }
    return digestHex(digest.data(), digestSize);
}

std::string
sha256DigestString(const std::uint8_t* bytes, std::size_t size,
                   const std::function<void()>& requireActive = {})
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 initialization failed");
    constexpr std::size_t ChunkBytes = 1024 * 1024;
    std::size_t offset = 0;
    while (offset < size) {
        if (requireActive)
            requireActive();
        const auto chunk = std::min(ChunkBytes, size - offset);
        if (EVP_DigestUpdate(context.get(), bytes + offset, chunk) != 1)
            throw std::runtime_error("SHA-256 update failed");
        offset += chunk;
    }
    if (requireActive)
        requireActive();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestSize) != 1 || digestSize != 32)
        throw std::runtime_error("SHA-256 digest failed");
    return "sha256:" + digestHex(digest.data(), digestSize);
}

std::string
quoteNamingJson(const std::string& value)
{
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        switch (byte) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20) {
                result += "\\u00";
                result.push_back(Hex[byte >> 4]);
                result.push_back(Hex[byte & 0x0f]);
            }
            else {
                result.push_back(static_cast<char>(byte));
            }
        }
    }
    result.push_back('"');
    return result;
}

ProtectedBlobIdentity
makeProtectedBlobDataPrefix(const ndn::Name& producer,
                            const std::string& contentDigest,
                            const std::string& keyReference,
                            const std::string& epoch,
                            const std::string& scope,
                            const std::string& publicationIdentity,
                            ndn::time::milliseconds freshness,
                            std::size_t segmentBytes,
                            const PublishedContentName& contentName)
{
    if (contentDigest.size() != 71 || contentDigest.compare(0, 7, "sha256:") != 0)
        throw std::invalid_argument("protected BLOB content digest is invalid");
    ndn::Buffer instance(HybridMessageCrypto::NONCE_SIZE);
    if (RAND_bytes(instance.data(), static_cast<int>(instance.size())) != 1)
        throw std::runtime_error("RAND_bytes failed for BLOB representation identity");
    const auto instanceDigest = computeNamingDigest(
        "ndnsf-naming/opaque/protection-instance", {{1, 4, instance}});
    const auto representation =
        std::string("{\"aad\":\"canonical-name-and-service-uri\",\"encoding\":\"ndnsf-hybrid-envelope\",\"freshnessMs\":") +
        std::to_string(freshness.count()) + ",\"schema\":1,\"segmentBytes\":" +
        std::to_string(segmentBytes) + "}";
    const auto protection = std::string("{\"cipher\":\"AES-256-GCM\",\"epoch\":") +
        quoteNamingJson(epoch) + ",\"keyReference\":" + quoteNamingJson(keyReference) +
        ",\"noncePolicy\":\"unique-per-new-publication\",\"publication\":" +
        quoteNamingJson(publicationIdentity) + ",\"representationNonce\":" +
        quoteNamingJson(instanceDigest) + ",\"scope\":" + quoteNamingJson(scope) + "}";
    const auto protectionDigest = computeNamingDigest("ndnsf-naming/protection",
        {{1, 5, std::vector<std::uint8_t>(representation.begin(), representation.end())},
         {2, 5, std::vector<std::uint8_t>(protection.begin(), protection.end())}});
    auto name = contentName.contentPrefix(producer, contentDigest.substr(7));
    name.append("PROTECTED").append(makeNamingDigestComponent(protectionDigest)).append("DATA");
    checkNamingNameSize(ndn::Name(name).appendSegment(
        std::numeric_limits<std::uint64_t>::max()));
    return {std::move(name), std::move(instance), representation, protection};
}

std::vector<std::uint8_t>
encodeVarNumber(std::uint64_t value)
{
    if (value < 253)
        return {static_cast<std::uint8_t>(value)};
    if (value <= 0xffff)
        return {0xfd, static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    if (value <= 0xffffffffULL)
        return {0xfe, static_cast<std::uint8_t>(value >> 24),
                static_cast<std::uint8_t>(value >> 16),
                static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return {0xff, static_cast<std::uint8_t>(value >> 56),
            static_cast<std::uint8_t>(value >> 48),
            static_cast<std::uint8_t>(value >> 40),
            static_cast<std::uint8_t>(value >> 32),
            static_cast<std::uint8_t>(value >> 24),
            static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
}

void
writeAll(int fd, const std::uint8_t* data, std::size_t size)
{
    std::size_t written = 0;
    while (written < size) {
        const auto count = ::write(fd, data + written, size - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::system_error(errno, std::generic_category(),
                                    "short write to Provider large-data file");
        written += static_cast<std::size_t>(count);
    }
}

std::filesystem::path
largeDataFileDirectory()
{
    if (const char* raw = std::getenv("NDNSF_REQUEST_LARGE_DATA_DIR");
        raw != nullptr && *raw != '\0') {
        return std::filesystem::path(raw);
    }
    return std::filesystem::temp_directory_path() / "ndnsf-large-data";
}

std::uintmax_t
largeDataFileMinimumFreeBytes()
{
    constexpr std::uintmax_t DefaultMinimum = 256ULL * 1024ULL * 1024ULL;
    const char* raw = std::getenv("NDNSF_REQUEST_LARGE_DATA_MIN_FREE_BYTES");
    if (raw == nullptr || *raw == '\0')
        return DefaultMinimum;
    try {
        const std::string value(raw);
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size())
            throw std::invalid_argument("trailing characters");
        return static_cast<std::uintmax_t>(parsed);
    }
    catch (const std::exception&) {
        throw std::invalid_argument(
            "NDNSF_REQUEST_LARGE_DATA_MIN_FREE_BYTES must be an integer");
    }
}

class DiskReservation
{
public:
    DiskReservation(const std::filesystem::path& directory, std::uintmax_t bytes)
      : m_bytes(bytes)
    {
        std::error_code error;
        const auto available = std::filesystem::space(directory, error).available;
        if (error)
            throw std::system_error(error, "cannot inspect Provider large-data filesystem");
        const auto minimum = largeDataFileMinimumFreeBytes();
        std::lock_guard<std::mutex> lock(m_mutex);
        if (available < minimum || m_reserved > available - minimum ||
            bytes > available - minimum - m_reserved) {
            throw std::runtime_error("large-data file publication has insufficient reserved disk space");
        }
        m_reserved += bytes;
        m_held = true;
    }

    ~DiskReservation()
    {
        if (!m_held)
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_reserved = m_reserved >= m_bytes ? m_reserved - m_bytes : 0;
    }

    DiskReservation(const DiskReservation&) = delete;
    DiskReservation& operator=(const DiskReservation&) = delete;

private:
    inline static std::mutex m_mutex;
    inline static std::uintmax_t m_reserved = 0;
    std::uintmax_t m_bytes = 0;
    bool m_held = false;
};

std::filesystem::path
streamEncryptLargeDataEnvelope(const ndn::Buffer& key,
                              ndn::span<const std::uint8_t> plaintext,
                              ndn::span<const std::uint8_t> associatedData,
                              HybridMessageEnvelope& envelope,
                              const ndn::Name& baseName,
                              std::size_t& wireBytes,
                              const std::function<void()>& requireActive)
{
    if (key.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE)
        throw std::runtime_error("invalid AES-GCM key size");
    constexpr std::size_t NonceSize = HybridMessageCrypto::NONCE_SIZE;
    constexpr std::size_t TagSize = HybridMessageCrypto::TAG_SIZE;
    ndn::Buffer nonce = envelope.getNonce();
    if (nonce.empty()) {
        nonce.resize(NonceSize);
        if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
            throw std::runtime_error("RAND_bytes failed for large-data envelope");
    }
    if (nonce.size() != NonceSize)
        throw std::runtime_error("invalid nonce size for large-data envelope");
    envelope.setNonce(nonce);
    envelope.setCipherText(ndn::Buffer{});
    envelope.setAuthTag(ndn::Buffer(TagSize));
    auto skeleton = envelope.WireEncode();
    skeleton.parse();

    std::size_t innerLength = 0;
    for (const auto& element : skeleton.elements()) {
        if (element.type() == tlv::CipherTextType) {
            innerLength += encodeVarNumber(element.type()).size() +
                           encodeVarNumber(plaintext.size()).size() + plaintext.size();
        }
        else {
            innerLength += element.size();
        }
    }

    const auto directory = largeDataFileDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        throw std::system_error(error, "cannot create Provider large-data directory");
    static std::atomic<std::uint64_t> sequence{0};
    const auto& baseWire = baseName.wireEncode();
    const auto baseDigest = hexDigest(baseWire.value(), baseWire.value_size());
    const auto finalPath = directory / ("ndnsf-provider-large-data-" + baseDigest.substr(0, 16) + "-" +
        std::to_string(::getpid()) + "-" + std::to_string(sequence.fetch_add(1)) + ".wire");
    auto stagingPath = finalPath;
    stagingPath += ".staging";
    const int fd = ::open(stagingPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        throw std::system_error(errno, std::generic_category(),
                                "cannot create Provider large-data staging file");
    EVP_CIPHER_CTX* cipher = nullptr;
    bool closed = false;
    const auto cleanup = [&] {
        if (cipher != nullptr) {
            EVP_CIPHER_CTX_free(cipher);
            cipher = nullptr;
        }
        if (!closed) {
            ::close(fd);
            closed = true;
        }
        std::error_code removeError;
        std::filesystem::remove(stagingPath, removeError);
    };
    try {
        const auto emit = [&](const std::vector<std::uint8_t>& bytes) {
            writeAll(fd, bytes.data(), bytes.size());
        };
        const auto topType = encodeVarNumber(skeleton.type());
        writeAll(fd, topType.data(), topType.size());
        const auto topLength = encodeVarNumber(innerLength);
        writeAll(fd, topLength.data(), topLength.size());

        cipher = EVP_CIPHER_CTX_new();
        if (cipher == nullptr ||
            EVP_EncryptInit_ex(cipher, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
            EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(nonce.size()), nullptr) != 1 ||
            EVP_EncryptInit_ex(cipher, nullptr, nullptr, key.data(), nonce.data()) != 1) {
            throw std::runtime_error("AES-GCM streaming initialization failed");
        }
        int produced = 0;
        if (!associatedData.empty() &&
            EVP_EncryptUpdate(cipher, nullptr, &produced, associatedData.data(),
                              static_cast<int>(associatedData.size())) != 1) {
            throw std::runtime_error("AES-GCM streaming AAD failed");
        }
        std::vector<std::uint8_t> output(1024 * 1024);
        ndn::Buffer tag(TagSize);
        bool ciphertextEmitted = false;
        for (const auto& element : skeleton.elements()) {
            if (element.type() != tlv::CipherTextType) {
                if (element.type() == tlv::AuthTagType) {
                    if (!ciphertextEmitted)
                        throw std::runtime_error("large-data envelope missing ciphertext");
                    if (EVP_EncryptFinal_ex(cipher, output.data(), &produced) != 1 ||
                        EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG,
                                            static_cast<int>(tag.size()), tag.data()) != 1) {
                        throw std::runtime_error("AES-GCM streaming finalization failed");
                    }
                    const auto type = encodeVarNumber(element.type());
                    const auto length = encodeVarNumber(tag.size());
                    emit(type);
                    emit(length);
                    emit(std::vector<std::uint8_t>(tag.begin(), tag.end()));
                    envelope.setAuthTag(tag);
                    continue;
                }
                writeAll(fd, element.data(), element.size());
                continue;
            }

            const auto type = encodeVarNumber(element.type());
            const auto length = encodeVarNumber(plaintext.size());
            emit(type);
            emit(length);
            std::size_t offset = 0;
            while (offset < plaintext.size()) {
                if (requireActive)
                    requireActive();
                const auto chunk = std::min(output.size(), plaintext.size() - offset);
                if (EVP_EncryptUpdate(cipher, output.data(), &produced,
                                      plaintext.data() + offset,
                                      static_cast<int>(chunk)) != 1) {
                    throw std::runtime_error("AES-GCM streaming encryption failed");
                }
                writeAll(fd, output.data(), static_cast<std::size_t>(produced));
                offset += chunk;
            }
            ciphertextEmitted = true;
        }
        if (!ciphertextEmitted)
            throw std::runtime_error("large-data envelope has no ciphertext element");
        if (requireActive)
            requireActive();
        if (::fsync(fd) != 0)
            throw std::system_error(errno, std::generic_category(),
                                    "cannot fsync Provider large-data wire file");
        if (::close(fd) != 0) {
            closed = true;
            throw std::system_error(errno, std::generic_category(),
                                    "cannot close Provider large-data wire file");
        }
        closed = true;
        EVP_CIPHER_CTX_free(cipher);
        cipher = nullptr;
        std::filesystem::rename(stagingPath, finalPath, error);
        if (error)
            throw std::system_error(error, "cannot commit Provider large-data wire file");
        wireBytes = static_cast<std::size_t>(std::filesystem::file_size(finalPath));
        return finalPath;
    }
    catch (...) {
        cleanup();
        throw;
    }
}

template<typename IoContext, typename Function>
void
onProviderFace(IoContext& io, const std::function<void()>& requireActive,
               Function&& function)
{
    struct Task
    {
        std::mutex mutex;
        std::condition_variable completed;
        std::function<void()> function;
        std::exception_ptr error;
        bool running = false;
        bool done = false;
    };
    auto task = std::make_shared<Task>();
    task->function = std::forward<Function>(function);
    if (requireActive)
        requireActive();
    boost::asio::post(io, [task] {
        std::function<void()> action;
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            if (task->done)
                return;
            task->running = true;
            action = std::move(task->function);
        }
        std::exception_ptr error;
        try { action(); }
        catch (...) { error = std::current_exception(); }
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->error = error;
            task->done = true;
        }
        task->completed.notify_all();
    });

    std::unique_lock<std::mutex> lock(task->mutex);
    while (!task->done) {
        lock.unlock();
        try {
            if (requireActive)
                requireActive();
        }
        catch (...) {
            const auto cancellation = std::current_exception();
            lock.lock();
            if (!task->running) {
                // Remove the borrowed closure before its caller unwinds. A
                // late Face callback retains only this completed task state.
                task->function = {};
                task->done = true;
            }
            else {
                // An already executing synchronous Face action must settle
                // before its borrowed publication variables can be released.
                task->completed.wait(lock, [&] { return task->done; });
            }
            std::rethrow_exception(cancellation);
        }
        lock.lock();
        task->completed.wait_for(lock, std::chrono::milliseconds(20),
                                 [&] { return task->done; });
    }
    const auto error = task->error;
    lock.unlock();
    if (error)
        std::rethrow_exception(error);
    if (requireActive)
        requireActive();
}

} // namespace

ProviderDurableLargeDataPublishResultV1
ProviderDurableLargeDataPublisher::publish(
    std::shared_ptr<ServiceProvider> producer,
    ProviderDurableLargeDataPublishRequestV1 request,
    std::shared_ptr<EncryptedLargeDataRangeStore> store,
    DurableStorageGrantResolver resolveGrant,
    DurableStorageGrantChallengeObserverV1 onGrantRequired,
    const std::function<void()>& requireActive,
    std::function<void()> onDurablePacketSetBoundary)
{
    ProviderDurableLargeDataPublishResultV1 result;
    bool wrappedKeyReferenceHeld = false;
    bool durableCommitStarted = false;
    std::string wrappedKeyId;
    const auto releaseWrappedKeyReference = [&] {
        if (!wrappedKeyReferenceHeld || durableCommitStarted || !producer || wrappedKeyId.empty())
            return;
        boost::asio::post(producer->m_face.getIoContext(),
          [producer, serviceName = request.serviceName, keyId = wrappedKeyId] {
            producer->m_hybridMessageCrypto.eraseWrappedSendKey(serviceName, keyId);
        });
        wrappedKeyReferenceHeld = false;
    };
    try {
        if (!producer || !store)
            throw std::invalid_argument("Provider durable publication requires Producer and packet store");
        if (producer->m_face.getIoContext().get_executor().running_in_this_thread())
            throw std::logic_error("Provider durable publication cannot block the Face event thread");
        if (!store->supportsDurableRetention() || !store->supportsOriginalDataPackets() ||
            !store->supportsAuxiliaryDataPackets())
            throw std::runtime_error("DURABLE_ORIGINAL_PACKET_STORE_UNSUPPORTED");
        if (!resolveGrant)
            throw std::runtime_error("DURABLE_STORAGE_GRANT_RESOLVER_REQUIRED");
        if (request.serviceName.empty() || request.requestId.empty() ||
            request.protectedPayload.empty() || request.objectLabel.empty())
            throw std::invalid_argument("Provider durable publication request is incomplete");
        if (request.contentName.label != request.objectLabel)
            throw std::invalid_argument("Provider durable publication label binding mismatch");
        request.contentName.encode();
        ProtectedBlobManifest::validateSuffix(request.manifestSuffix);
        if (request.commitOptions.retention != EncryptedLargeDataRetention::Durable)
            throw std::invalid_argument("Provider durable publication requires durable retention");
        const auto publicationIdentity = request.commitOptions.publicationIdentity;
        if (publicationIdentity.size() != 71 || publicationIdentity.compare(0, 7, "sha256:") != 0)
            throw std::invalid_argument("durable large-data publication identity is invalid");
        if (!std::all_of(publicationIdentity.begin() + 7, publicationIdentity.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }))
            throw std::invalid_argument("durable large-data publication identity is invalid");
        if (requireActive)
            requireActive();

        result.objectId = request.objectLabel;
        result.plaintextSize = request.protectedPayload.size();
        result.contentDigest = sha256DigestString(
            request.protectedPayload.data(), request.protectedPayload.size(), requireActive);
        result.publicationIdentity = publicationIdentity;
        result.authorizationScope = makeAbeServiceAttribute(request.serviceName, "SERVICE");
        result.keyReferenceVersion = "v1";

        if (auto candidate = store->lookupDurable(publicationIdentity, requireActive)) {
            const ndn::Name encryptedName(candidate->encryptedName);
            const ndn::Name manifestName(candidate->manifestName);
            const ndn::Name dataPrefix(candidate->dataPrefix);
            if (!candidate->source || !candidate->source->isDurable() ||
                candidate->publicationIdentity != publicationIdentity ||
                candidate->contentDigest != result.contentDigest ||
                candidate->plaintextSize != result.plaintextSize ||
                candidate->packetStorageFormat != "original-packet-set" ||
                candidate->manifestName.empty() || candidate->dataPrefix.empty() ||
                candidate->keyReferenceId.empty() || candidate->keyReferenceVersion.empty() ||
                candidate->protectionEpoch.empty() || candidate->ciphertextManifestDigest.empty() ||
                candidate->packetWireBytes == 0 || candidate->source->size() == 0 ||
                !producer->identity.isPrefixOf(manifestName) ||
                !producer->identity.isPrefixOf(dataPrefix) ||
                encryptedName != dataPrefix || ndn::Name(candidate->servingLocator) != dataPrefix) {
                throw std::runtime_error("DURABLE_LOOKUP_METADATA_MISMATCH");
            }

            auto auxiliaryNames = candidate->auxiliaryPacketNames;
            if (auxiliaryNames.empty() ||
                auxiliaryNames.size() > EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketCount) {
                throw std::runtime_error("DURABLE_AUXILIARY_PACKETS_MISSING");
            }
            EncryptedLargeDataCommitOptions retainOptions;
            retainOptions.retention = EncryptedLargeDataRetention::Durable;
            retainOptions.publicationIdentity = candidate->publicationIdentity;
            retainOptions.protectionEpoch = candidate->protectionEpoch;
            retainOptions.keyReferenceId = candidate->keyReferenceId;
            retainOptions.keyReferenceVersion = candidate->keyReferenceVersion;
            retainOptions.ciphertextManifestDigest = candidate->ciphertextManifestDigest;
            retainOptions.servingLocator = candidate->servingLocator;
            retainOptions.contentDigest = candidate->contentDigest;
            retainOptions.plaintextSize = candidate->plaintextSize;
            retainOptions.packetStorageFormat = candidate->packetStorageFormat;
            retainOptions.manifestName = candidate->manifestName;
            retainOptions.dataPrefix = candidate->dataPrefix;
            retainOptions.packetWireBytes = candidate->packetWireBytes;
            retainOptions.auxiliaryPacketNames = auxiliaryNames;

            DurableStorageGrantChallengeV1 challenge;
            challenge.manifestName = manifestName;
            challenge.writerIdentity = producer->identity.toUri(ndn::name::UriFormat::ALTERNATE);
            challenge.publicationIdentity = publicationIdentity;
            challenge.protectionEpoch = candidate->protectionEpoch;
            challenge.dataPrefix = dataPrefix;
            challenge.packetWireBytes = candidate->packetWireBytes;
            challenge.auxiliaryPacketNames = auxiliaryNames;
            if (requireActive)
                requireActive();
            if (onGrantRequired)
                onGrantRequired(challenge);
            if (requireActive)
                requireActive();
            auto grant = resolveGrant(manifestName, challenge.writerIdentity, requireActive);
            if (grant.repoNode.empty() || grant.ownerIdentity.empty() || grant.grantId.empty())
                throw std::runtime_error("DURABLE_STORAGE_GRANT_INVALID");
            (void)ndn::Name(grant.ownerIdentity);
            retainOptions.storageActor = DurableStorageActorContext{
                challenge.writerIdentity, std::move(grant)};
            auto source = store->retainDurableSource(*candidate, retainOptions, requireActive);
            if (!source || !source->isDurable() || source->size() != candidate->source->size())
                throw std::runtime_error("DURABLE_OWNER_RETAIN_INVALID");
            if (source->auxiliaryDataPacketNames() != auxiliaryNames)
                throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INDEX_MISMATCH");
            source->requireCurrentDurableStorageOwner(
                manifestName, retainOptions.storageActor->grant.ownerIdentity);

            const auto rootWire = source->readOriginalDataPacket(manifestName);
            if (rootWire.empty() || rootWire.size() > ndn::MAX_NDN_PACKET_SIZE)
                throw std::runtime_error("DURABLE_LOOKUP_UNREADABLE");
            const auto rootPacket = std::make_shared<ndn::Data>(ndn::Block(
                ndn::span<const std::uint8_t>(rootWire.data(), rootWire.size())));
            if (rootPacket->getName() != manifestName)
                throw std::runtime_error("DURABLE_MANIFEST_NAME_MISMATCH");
            const auto& rootContent = rootPacket->getContent();
            if (rootContent.value_size() == 0)
                throw std::runtime_error("DURABLE_MANIFEST_EMPTY");
            const std::vector<std::uint8_t> manifestPayload(
                rootContent.value(), rootContent.value() + rootContent.value_size());
            const auto restoredManifest = ProtectedBlobManifest::decode(
                manifestPayload, producer->identity, manifestName);
            if (!restoredManifest.contentName ||
                restoredManifest.contentName->encode() != request.contentName.encode() ||
                restoredManifest.manifestSuffix != request.manifestSuffix ||
                restoredManifest.authorizationScope != result.authorizationScope ||
                restoredManifest.dataPrefix != dataPrefix ||
                restoredManifest.envelopeBytes != source->size() ||
                restoredManifest.plaintextBytes != result.plaintextSize ||
                restoredManifest.protectionEpoch != candidate->protectionEpoch ||
                restoredManifest.keyReferenceId != candidate->keyReferenceId ||
                restoredManifest.segmentBytes != SegmentBytes || restoredManifest.segmentCount == 0) {
                throw std::runtime_error("DURABLE_MANIFEST_METADATA_MISMATCH");
            }
            const auto digestComponent = makeNamingDigestComponent(result.contentDigest.substr(7));
            if (ndn::name::Component(ndn::span<const std::uint8_t>(
                    restoredManifest.plaintextDigest.data(), restoredManifest.plaintextDigest.size())) !=
                digestComponent) {
                throw std::runtime_error("DURABLE_MANIFEST_CONTENT_MISMATCH");
            }
            const auto keyReferenceText = restoredManifest.keyIdentifier + "|" +
                restoredManifest.protectionEpoch + "|" +
                request.serviceName.toUri(ndn::name::UriFormat::CANONICAL) + "|" +
                result.authorizationScope;
            if (sha256DigestString(reinterpret_cast<const std::uint8_t*>(keyReferenceText.data()),
                                   keyReferenceText.size()) != candidate->keyReferenceId ||
                sha256DigestString(manifestPayload.data(), manifestPayload.size()) !=
                    candidate->ciphertextManifestDigest) {
                throw std::runtime_error("DURABLE_MANIFEST_DIGEST_MISMATCH");
            }
            const auto expectedSegmentCount = 1 + (source->size() - 1) / SegmentBytes;
            if (restoredManifest.segmentCount != expectedSegmentCount)
                throw std::runtime_error("DURABLE_SEGMENT_COUNT_MISMATCH");

            std::uint64_t packetWireBytes = rootWire.size();
            std::uint64_t envelopeBytes = 0;
            std::uint64_t auxiliaryWireBytes = 0;
            std::vector<std::shared_ptr<ndn::Data>> auxiliaryPackets;
            auxiliaryPackets.reserve(auxiliaryNames.size());
            std::set<ndn::Name> uniqueAuxiliaryNames;
            const auto checkedPacket = [&](const ndn::Name& name,
                                           const std::vector<std::uint8_t>& wire) {
                if (wire.empty() || wire.size() > ndn::MAX_NDN_PACKET_SIZE ||
                    wire.size() > std::numeric_limits<std::uint64_t>::max() - packetWireBytes) {
                    throw std::runtime_error("DURABLE_ORIGINAL_PACKET_INVALID");
                }
                auto packet = std::make_shared<ndn::Data>(ndn::Block(
                    ndn::span<const std::uint8_t>(wire.data(), wire.size())));
                if (packet->getName() != name)
                    throw std::runtime_error("DURABLE_ORIGINAL_PACKET_NAME_MISMATCH");
                packetWireBytes += wire.size();
                return packet;
            };
            std::vector<std::shared_ptr<ndn::Data>> segmentBatch;
            segmentBatch.reserve(64);
            const auto validateProducerPackets = [&](
                const std::vector<std::shared_ptr<ndn::Data>>& packets) {
                onProviderFace(producer->m_face.getIoContext(), requireActive, [&] {
                    for (const auto& packet : packets) {
                        if (!producer->validator->isProducerSignerAuthorized(*packet, producer->identity))
                            throw std::runtime_error("DURABLE_ORIGINAL_PACKET_SIGNER_INVALID");
                    }
                });
            };
            for (std::uint64_t segment = 0; segment < restoredManifest.segmentCount; ++segment) {
                if (requireActive)
                    requireActive();
                auto segmentName = dataPrefix;
                segmentName.appendSegment(segment);
                auto packet = checkedPacket(segmentName, source->readOriginalDataPacket(segmentName));
                const auto& content = packet->getContent();
                const auto expectedContentBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                    SegmentBytes, source->size() - envelopeBytes));
                if (content.value_size() != expectedContentBytes)
                    throw std::runtime_error("DURABLE_SEGMENT_CONTENT_LENGTH_MISMATCH");
                const auto expectedFinalBlock = ndn::name::Component::fromSegment(
                    restoredManifest.segmentCount - 1);
                if (!packet->getFinalBlock() || *packet->getFinalBlock() != expectedFinalBlock)
                    throw std::runtime_error("DURABLE_SEGMENT_GEOMETRY_INVALID");
                envelopeBytes += content.value_size();
                segmentBatch.push_back(std::move(packet));
                if (segmentBatch.size() == 64) {
                    validateProducerPackets(segmentBatch);
                    segmentBatch.clear();
                }
            }
            if (!segmentBatch.empty())
                validateProducerPackets(segmentBatch);
            if (envelopeBytes != source->size())
                throw std::runtime_error("DURABLE_ENVELOPE_SIZE_MISMATCH");
            for (const auto& auxiliaryName : auxiliaryNames) {
                if (!producer->identity.isPrefixOf(auxiliaryName) ||
                    auxiliaryName.size() <= producer->identity.size() ||
                    dataPrefix.isPrefixOf(auxiliaryName) ||
                    !uniqueAuxiliaryNames.insert(auxiliaryName).second) {
                    throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INVALID");
                }
                if (requireActive)
                    requireActive();
                auto packet = checkedPacket(auxiliaryName,
                    source->readOriginalDataPacket(auxiliaryName));
                const auto wireBytes = packet->wireEncode().size();
                if (wireBytes > EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketWireBytes -
                                    auxiliaryWireBytes) {
                    throw std::runtime_error("DURABLE_AUXILIARY_PACKET_LIMIT");
                }
                auxiliaryWireBytes += wireBytes;
                auxiliaryPackets.push_back(std::move(packet));
            }
            if (packetWireBytes != candidate->packetWireBytes)
                throw std::runtime_error("DURABLE_PACKET_WIRE_BYTES_MISMATCH");
            source->requireCurrentDurableStorageOwner(
                manifestName, retainOptions.storageActor->grant.ownerIdentity);
            const auto cacheFreshness = request.freshness <= ndn::time::milliseconds::zero() ?
                ndn::time::milliseconds(60000) : request.freshness;
            onProviderFace(producer->m_face.getIoContext(), requireActive, [&] {
                if (!producer->validator->isProducerSignerAuthorized(*rootPacket, producer->identity))
                    throw std::runtime_error("protected manifest signer is not authorized for Producer");
                for (const auto& packet : auxiliaryPackets) {
                    if (!producer->validator->isProducerSignerAuthorized(*packet, producer->identity))
                        throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INVALID");
                }
                producer->insertDataIntoIMS(*rootPacket, cacheFreshness);
                for (const auto& packet : auxiliaryPackets)
                    producer->insertDataIntoIMS(*packet, cacheFreshness);
            });
            source->requireCurrentDurableStorageOwner(
                manifestName, retainOptions.storageActor->grant.ownerIdentity);
            if (onDurablePacketSetBoundary)
                onDurablePacketSetBoundary();
            result.encryptedDataName = dataPrefix;
            result.manifestName = manifestName;
            result.dataPrefix = dataPrefix;
            result.envelopeSize = source->size();
            result.protectionEpoch = candidate->protectionEpoch;
            result.keyReferenceId = candidate->keyReferenceId;
            result.keyReferenceVersion = candidate->keyReferenceVersion;
            result.manifestDigest = candidate->ciphertextManifestDigest;
            result.ciphertextManifestDigest = candidate->ciphertextManifestDigest;
            result.servingLocator = candidate->servingLocator;
            result.auxiliaryPacketNames = std::move(auxiliaryNames);
            result.durableStorageSource = std::move(source);
            result.success = true;
            return result;
        }

        const auto directory = largeDataFileDirectory();
        std::error_code directoryError;
        std::filesystem::create_directories(directory, directoryError);
        if (directoryError)
            throw std::system_error(directoryError, "cannot create Provider large-data directory");
        const auto envelopeBudget = static_cast<std::uintmax_t>(request.protectedPayload.size()) +
                                    EnvelopeOverheadBudget;
        const auto packetCountBudget = 1 + (envelopeBudget - 1) / SegmentBytes;
        if (packetCountBudget > (std::numeric_limits<std::uintmax_t>::max() - envelopeBudget) /
                                    PacketRecordBudget)
            throw std::runtime_error("ORIGINAL_PACKET_STORAGE_BUDGET_OVERFLOW");
        const auto packetStorageBudget = packetCountBudget * PacketRecordBudget;
        if (envelopeBudget > std::numeric_limits<std::uintmax_t>::max() - packetStorageBudget)
            throw std::runtime_error("ORIGINAL_PACKET_STORAGE_BUDGET_OVERFLOW");
        DiskReservation diskReservation(directory, envelopeBudget + packetStorageBudget);

        HybridMessageKey key;
        ndn::nacabe::SPtrVector<ndn::Data> wrappedContentData;
        ndn::nacabe::SPtrVector<ndn::Data> wrappedCkData;
        std::vector<std::uint8_t> wrappedMessageKey;
        const std::vector<std::string> attributes{result.authorizationScope};
        onProviderFace(producer->m_face.getIoContext(), requireActive, [&] {
            key = producer->m_hybridMessageCrypto.getOrCreateSendKey(
                request.serviceName, producer->identity, result.authorizationScope,
                "REQUEST-LARGE", producer->m_hybridCryptoCounters);
            wrappedKeyId = key.keyId;
            if (producer->m_hybridMessageCrypto.shouldAttachWrappedKey(key.keyId)) {
                std::tie(wrappedContentData, wrappedCkData) =
                    (producer->m_testNacProducer ? *producer->m_testNacProducer : producer->nacProducer)
                        .produce(key.keyName, attributes,
                                 ndn::span<const std::uint8_t>(key.key.data(), key.key.size()),
                                 producer->m_signingInfo);
                const auto wrapped = mergeDataContents(wrappedContentData);
                wrappedMessageKey.assign(wrapped.begin(), wrapped.end());
                if (wrappedMessageKey.empty() || wrappedContentData.empty())
                    throw std::runtime_error("NAC-ABE produced no wrapped large-data MessageKey");
                wrappedContentData = prepareHybridMessageKeyPackets(
                    request.serviceName, producer->identity, result.authorizationScope,
                    key.epochId,
                    ndn::span<const std::uint8_t>(wrappedMessageKey.data(), wrappedMessageKey.size()),
                    producer->m_testSigningKeyChain ? *producer->m_testSigningKeyChain : producer->m_keyChain,
                    producer->m_signingInfo, wrappedContentData.front()->getFreshnessPeriod(),
                    wrappedContentData.front()->getContentType());
                producer->m_hybridMessageCrypto.cacheWrappedSendPackets(
                    request.serviceName, key.keyId,
                    ndn::Buffer(wrappedMessageKey.begin(), wrappedMessageKey.end()),
                    wrappedContentData, wrappedCkData);
                wrappedKeyReferenceHeld = true;
                ++producer->m_hybridCryptoCounters.nac_abe_key_wrap_count;
            }
            else {
                if (!producer->m_hybridMessageCrypto.retainWrappedSendKey(
                        request.serviceName, key.keyId)) {
                    throw std::runtime_error("wrapped large-data key disappeared before publication");
                }
                wrappedKeyReferenceHeld = true;
            }
            if (!producer->m_hybridMessageCrypto.getWrappedSendPackets(
                    key.keyId, wrappedContentData, wrappedCkData) ||
                wrappedContentData.empty() || wrappedCkData.empty()) {
                throw std::runtime_error("DURABLE_AUXILIARY_PACKETS_MISSING");
            }
            const auto wrapped = mergeDataContents(wrappedContentData);
            wrappedMessageKey.assign(wrapped.begin(), wrapped.end());
            if (wrappedMessageKey.empty())
                throw std::runtime_error("DURABLE_AUXILIARY_PACKETS_MISSING");
            if (wrappedContentData.size() > EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketCount ||
                wrappedCkData.size() > EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketCount -
                                           wrappedContentData.size()) {
                throw std::runtime_error("DURABLE_AUXILIARY_PACKET_LIMIT");
            }
        });
        result.protectionEpoch = key.epochId;
        const auto keyReferenceText = key.keyId + "|" + key.epochId + "|" +
            request.serviceName.toUri(ndn::name::UriFormat::CANONICAL) + "|" +
            result.authorizationScope;
        result.keyReferenceId = sha256DigestString(
            reinterpret_cast<const std::uint8_t*>(keyReferenceText.data()), keyReferenceText.size());

        auto blobIdentity = makeProtectedBlobDataPrefix(
            producer->identity, result.contentDigest, result.keyReferenceId, key.epochId,
            result.authorizationScope, publicationIdentity,
            request.freshness <= ndn::time::milliseconds::zero() ?
                ndn::time::milliseconds(60000) : request.freshness,
            SegmentBytes, request.contentName);
        const auto encryptedDataName = blobIdentity.dataPrefix;
        result.encryptedDataName = encryptedDataName;
        result.dataPrefix = encryptedDataName;

        HybridMessageEnvelope envelope;
        envelope.setKeyId(key.keyId);
        envelope.setEpochId(key.epochId);
        envelope.setMessageType("REQUEST-LARGE");
        envelope.setNonce(blobIdentity.nonce);
        envelope.setWrappedMessageKey(ndn::Buffer(wrappedMessageKey.begin(), wrappedMessageKey.end()));
        const auto associatedText = encryptedDataName.toUri(ndn::name::UriFormat::CANONICAL) +
            "|REQUEST-LARGE|" + request.serviceName.toUri(ndn::name::UriFormat::CANONICAL);
        const ndn::Buffer associatedData(
            reinterpret_cast<const std::uint8_t*>(associatedText.data()), associatedText.size());
        std::size_t encodedBytes = 0;
        const auto filePath = streamEncryptLargeDataEnvelope(
            key.key,
            ndn::span<const std::uint8_t>(request.protectedPayload.data(), request.protectedPayload.size()),
            ndn::span<const std::uint8_t>(associatedData.data(), associatedData.size()),
            envelope, encryptedDataName, encodedBytes, requireActive);
        struct RemoveFile
        {
            std::filesystem::path path;
            ~RemoveFile() { if (!path.empty()) { std::error_code ec; std::filesystem::remove(path, ec); } }
        } removeEnvelope{filePath};
        if (encodedBytes == 0)
            throw std::runtime_error("empty protected large-data envelope");
        result.envelopeSize = encodedBytes;

        const auto segmentCount = 1 + (encodedBytes - 1) / SegmentBytes;
        ProtectedBlobManifest manifest;
        manifest.producer = producer->identity;
        manifest.dataPrefix = encryptedDataName;
        const auto digestComponent = makeNamingDigestComponent(result.contentDigest.substr(7));
        manifest.plaintextDigest.assign(digestComponent.value(),
                                        digestComponent.value() + digestComponent.value_size());
        manifest.plaintextBytes = result.plaintextSize;
        manifest.envelopeBytes = encodedBytes;
        manifest.segmentBytes = SegmentBytes;
        manifest.segmentCount = segmentCount;
        manifest.representationSpec = blobIdentity.representationSpec;
        manifest.protectionSpec = blobIdentity.protectionSpec;
        manifest.authorizationScope = result.authorizationScope;
        manifest.protectionEpoch = key.epochId;
        manifest.keyReferenceId = result.keyReferenceId;
        manifest.keyIdentifier = key.keyId;
        manifest.manifestSuffix = request.manifestSuffix;
        manifest.contentName = request.contentName;
        const auto manifestName = manifest.getName();
        const auto manifestPayload = manifest.encode();
        auto manifestPacket = std::make_shared<ndn::Data>(manifest.getName());
        manifestPacket->setContent(manifestPayload);
        const auto freshness = request.freshness <= ndn::time::milliseconds::zero() ?
            ndn::time::milliseconds(60000) : request.freshness;
        manifestPacket->setFreshnessPeriod(freshness);
        onProviderFace(producer->m_face.getIoContext(), requireActive, [&] {
            (producer->m_testSigningKeyChain ? *producer->m_testSigningKeyChain : producer->m_keyChain)
                .sign(*manifestPacket, producer->m_signingInfo);
            if (!producer->validator->isProducerSignerAuthorized(*manifestPacket, producer->identity))
                throw std::invalid_argument("protected manifest signer is not authorized for Producer");
        });
        if (manifestPacket->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
            throw std::runtime_error("protected manifest exceeds NDN packet limit");
        const auto manifestWire = manifestPacket->wireEncode();
        const std::vector<std::uint8_t> manifestWireBytes(manifestWire.begin(), manifestWire.end());
        result.manifestName = manifestName;
        result.manifestDigest = sha256DigestString(manifestPayload.data(), manifestPayload.size());
        result.ciphertextManifestDigest = result.manifestDigest;
        result.servingLocator = encryptedDataName.toUri(ndn::name::UriFormat::CANONICAL);

        std::vector<std::pair<ndn::Name, std::vector<std::uint8_t>>> auxiliaryPackets;
        std::set<ndn::Name> auxiliaryNames;
        std::uint64_t auxiliaryWireBytes = 0;
        const auto retainOriginal = [&](const auto& packets) {
            for (const auto& packet : packets) {
                if (!packet || !producer->identity.isPrefixOf(packet->getName()) ||
                    packet->getName().size() <= producer->identity.size() ||
                    encryptedDataName.isPrefixOf(packet->getName()) ||
                    !auxiliaryNames.insert(packet->getName()).second ||
                    !producer->validator->isProducerSignerAuthorized(*packet, producer->identity)) {
                    throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INVALID");
                }
                const auto& wire = packet->wireEncode();
                if (wire.size() > ndn::MAX_NDN_PACKET_SIZE ||
                    wire.size() > EncryptedLargeDataCommitOptions::MaxAuxiliaryPacketWireBytes -
                                      auxiliaryWireBytes) {
                    throw std::runtime_error("DURABLE_AUXILIARY_PACKET_LIMIT");
                }
                auxiliaryWireBytes += wire.size();
                auxiliaryPackets.emplace_back(packet->getName(),
                    std::vector<std::uint8_t>(wire.begin(), wire.end()));
            }
        };
        onProviderFace(producer->m_face.getIoContext(), requireActive, [&] {
            retainOriginal(wrappedContentData);
            retainOriginal(wrappedCkData);
        });
        result.auxiliaryPacketNames.reserve(auxiliaryPackets.size());
        for (const auto& packet : auxiliaryPackets)
            result.auxiliaryPacketNames.push_back(packet.first);

        std::ifstream encryptedFile(filePath, std::ios::binary);
        if (!encryptedFile)
            throw std::runtime_error("cannot open Provider encrypted envelope file");
        auto originalPackets = detail::OriginalPacketFile::create(
            directory, encryptedDataName, segmentCount, packetStorageBudget,
            [&](std::uint64_t segment) {
                if (requireActive)
                    requireActive();
                if (segment >= segmentCount)
                    throw std::out_of_range("Provider large-data segment index is invalid");
                const auto offset = segment * SegmentBytes;
                const auto length = static_cast<std::size_t>(
                    std::min<std::uint64_t>(SegmentBytes, encodedBytes - offset));
                std::vector<std::uint8_t> content(length);
                encryptedFile.clear();
                encryptedFile.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                encryptedFile.read(reinterpret_cast<char*>(content.data()),
                                   static_cast<std::streamsize>(content.size()));
                if (encryptedFile.gcount() != static_cast<std::streamsize>(content.size()))
                    throw std::runtime_error("cannot read Provider encrypted envelope segment");
                auto data = std::make_shared<ndn::Data>(
                    ndn::Name(encryptedDataName).appendSegment(segment));
                data->setFreshnessPeriod(freshness);
                data->setFinalBlock(ndn::name::Component::fromSegment(segmentCount - 1));
                data->setContent(content);
                onProviderFace(producer->m_face.getIoContext(), requireActive, [&] {
                    (producer->m_testSigningKeyChain ? *producer->m_testSigningKeyChain : producer->m_keyChain)
                        .sign(*data, producer->m_signingInfo);
                    if (!producer->validator->isProducerSignerAuthorized(*data, producer->identity))
                        throw std::invalid_argument("protected segment signer is not authorized for Producer");
                });
                const auto& wire = data->wireEncode();
                if (wire.size() > ndn::MAX_NDN_PACKET_SIZE)
                    throw std::runtime_error("protected segment exceeds NDN packet limit");
                return std::vector<std::uint8_t>(wire.begin(), wire.end());
            }, requireActive);

        EncryptedLargeDataCommitOptions commitOptions;
        commitOptions.retention = EncryptedLargeDataRetention::Durable;
        commitOptions.publicationIdentity = publicationIdentity;
        commitOptions.protectionEpoch = result.protectionEpoch;
        commitOptions.keyReferenceId = result.keyReferenceId;
        commitOptions.keyReferenceVersion = result.keyReferenceVersion;
        commitOptions.ciphertextManifestDigest = result.ciphertextManifestDigest;
        commitOptions.servingLocator = result.servingLocator;
        commitOptions.contentDigest = result.contentDigest;
        commitOptions.plaintextSize = result.plaintextSize;
        commitOptions.packetStorageFormat = "original-packet-set";
        commitOptions.manifestName = manifestName.toUri(ndn::name::UriFormat::CANONICAL);
        commitOptions.dataPrefix = encryptedDataName.toUri(ndn::name::UriFormat::CANONICAL);
        const auto segmentIndexBytes = segmentCount * 2 * sizeof(std::uint64_t);
        if (originalPackets->storageBytes() < segmentIndexBytes)
            throw std::runtime_error("ORIGINAL_PACKET_STORAGE_BUDGET_INVALID");
        commitOptions.packetWireBytes = manifestWireBytes.size() +
            originalPackets->storageBytes() - segmentIndexBytes;
        for (const auto& packet : auxiliaryPackets) {
            if (packet.first == manifestName ||
                packet.second.size() > std::numeric_limits<std::uint64_t>::max() -
                                           commitOptions.packetWireBytes) {
                throw std::runtime_error("DURABLE_AUXILIARY_PACKET_INVALID");
            }
            commitOptions.auxiliaryPacketNames.push_back(packet.first);
            commitOptions.packetWireBytes += packet.second.size();
        }
        DurableStorageGrantChallengeV1 challenge;
        challenge.writerIdentity = producer->identity.toUri(ndn::name::UriFormat::ALTERNATE);
        challenge.manifestName = manifestName;
        challenge.publicationIdentity = publicationIdentity;
        challenge.protectionEpoch = result.protectionEpoch;
        challenge.dataPrefix = encryptedDataName;
        challenge.packetWireBytes = commitOptions.packetWireBytes;
        challenge.auxiliaryPacketNames = commitOptions.auxiliaryPacketNames;
        if (requireActive)
            requireActive();
        if (onGrantRequired)
            onGrantRequired(challenge);
        if (requireActive)
            requireActive();
        auto grant = resolveGrant(manifestName, challenge.writerIdentity, requireActive);
        if (grant.repoNode.empty() || grant.ownerIdentity.empty() || grant.grantId.empty())
            throw std::runtime_error("DURABLE_STORAGE_GRANT_INVALID");
        (void)ndn::Name(grant.ownerIdentity);
        commitOptions.storageActor = DurableStorageActorContext{
            challenge.writerIdentity, std::move(grant)};

        std::uint64_t next = 0;
        if (onDurablePacketSetBoundary)
            onDurablePacketSetBoundary();
        durableCommitStarted = true;
        auto source = store->commitOriginalDataPackets(
            manifestName, commitOptions,
            [&]() -> std::optional<std::pair<ndn::Name, std::vector<std::uint8_t>>> {
                if (requireActive)
                    requireActive();
                if (next == 0) {
                    ++next;
                    return std::make_pair(manifestName, manifestWireBytes);
                }
                if (next <= segmentCount) {
                    const auto segment = next++ - 1;
                    return std::make_pair(
                        ndn::Name(encryptedDataName).appendSegment(segment),
                        originalPackets->read(segment));
                }
                const auto auxiliary = next - segmentCount - 1;
                if (auxiliary >= auxiliaryPackets.size())
                    return std::nullopt;
                ++next;
                return auxiliaryPackets[auxiliary];
            }, requireActive);
        if (!source || !source->isDurable() || source->size() != encodedBytes ||
            source->auxiliaryDataPacketNames() != commitOptions.auxiliaryPacketNames ||
            source->readOriginalDataPacket(manifestName) != manifestWireBytes) {
            throw std::runtime_error("DURABLE_ORIGINAL_PACKET_COMMIT_INVALID");
        }
        for (std::uint64_t segment = 0; segment < segmentCount; ++segment) {
            if (requireActive)
                requireActive();
            auto segmentName = encryptedDataName;
            segmentName.appendSegment(segment);
            const auto expectedWire = originalPackets->read(segment);
            const auto storedWire = source->readOriginalDataPacket(segmentName);
            if (storedWire != expectedWire)
                throw std::runtime_error("DURABLE_SEGMENT_READBACK_MISMATCH");
            const ndn::Data packet(ndn::Block(ndn::span<const std::uint8_t>(
                storedWire.data(), storedWire.size())));
            const auto expectedFinalBlock = ndn::name::Component::fromSegment(segmentCount - 1);
            if (packet.getName() != segmentName || !packet.getFinalBlock() ||
                *packet.getFinalBlock() != expectedFinalBlock)
                throw std::runtime_error("DURABLE_SEGMENT_GEOMETRY_INVALID");
        }
        for (const auto& packet : auxiliaryPackets) {
            if (requireActive)
                requireActive();
            if (source->readOriginalDataPacket(packet.first) != packet.second)
                throw std::runtime_error("DURABLE_AUXILIARY_PACKET_READBACK_MISMATCH");
        }
        source->requireCurrentDurableStorageOwner(manifestName,
            commitOptions.storageActor->grant.ownerIdentity);
        if (requireActive)
            requireActive();

        // Keep the small producer-signed manifest and wrapped-key packets in
        // the Provider IMS. The potentially large body remains in Repo's
        // original-packet store and is fetched by its original segment names.
        onProviderFace(producer->m_face.getIoContext(), requireActive, [&] {
            producer->insertDataIntoIMS(*manifestPacket, freshness);
            for (const auto& packet : wrappedContentData)
                producer->insertDataIntoIMS(*packet, freshness);
            for (const auto& packet : wrappedCkData)
                producer->insertDataIntoIMS(*packet, freshness);
        });
        source->requireCurrentDurableStorageOwner(manifestName,
            commitOptions.storageActor->grant.ownerIdentity);
        result.durableStorageSource = std::move(source);
        result.success = true;
    }
    catch (const std::exception& error) {
        try { releaseWrappedKeyReference(); }
        catch (...) {}
        result.errorMessage = error.what();
    }
    catch (...) {
        try { releaseWrappedKeyReference(); }
        catch (...) {}
        result.errorMessage = "unknown Provider durable publication failure";
    }
    return result;
}

} // namespace ndn_service_framework
