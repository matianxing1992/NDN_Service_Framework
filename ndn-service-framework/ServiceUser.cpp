#include "ServiceUser.hpp"
#include "detail/TargetedRequestRouting.hpp"
#include "detail/ProtectedTargetedRequest.hpp"
#include "detail/OriginalPacketFile.hpp"
#include "ProducerDataNaming.hpp"
#include "ProtectedBlobManifest.hpp"
#include <future>

#include <boost/asio/post.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <tuple>
#include <optional>
#include <random>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <system_error>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validator-null.hpp>
#include <ndn-cxx/security/transform/public-key.hpp>
#include <ndn-cxx/util/scope.hpp>
#include <ndn-cxx/util/sha256.hpp>

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.ServiceUser);

    namespace
    {
        bool
        isSpec192SelectedRequestTraceEnabled()
        {
            const char* setting = std::getenv("NDNSF_SPEC192_SELECTED_REQUEST_TRACE");
            return setting != nullptr && setting[0] == '1' && setting[1] == '\0';
        }

        bool
        isSpec192StatusService(const ndn::Name& serviceName)
        {
            // This classifier only gates optional traces; avoid URI creation
            // on normal ACK and response processing when tracing is disabled.
            if (!isSpec192SelectedRequestTraceEnabled()) return false;
            const auto uri = serviceName.toUri();
            constexpr char suffix[] = "/STATUS";
            return uri.size() >= sizeof(suffix) - 1 &&
                   uri.compare(uri.size() - (sizeof(suffix) - 1),
                               sizeof(suffix) - 1, suffix) == 0;
        }

        void
        traceSpec192StatusResponse(const char* stage)
        {
            if (!isSpec192SelectedRequestTraceEnabled()) return;
            std::cerr << "Spec192 user STATUS response: stage="
                      << stage << std::endl;
        }

        void
        traceSpec192StatusAck(const ndn::Name& serviceName, const char* stage)
        {
            if (!isSpec192StatusService(serviceName)) return;
            std::cerr << "[DEBUG-spec192-ack-user] " << stage << std::endl;
        }

        void
        traceSpec192SelectedRequestAckStage(const char* stage)
        {
            if (!isSpec192SelectedRequestTraceEnabled()) return;
            std::cerr << "Spec192 user STATUS ACK: stage="
                      << stage << std::endl;
        }
    }

    struct ServiceUser::LargeDataKeyReleaseState
    {
        std::mutex mutex;
        HybridMessageCrypto* crypto = nullptr;
        struct Lease
        {
            std::shared_ptr<LargeDataKeyReleaseState> state;
            ndn::Name service;
            std::string keyId;
            Lease(std::shared_ptr<LargeDataKeyReleaseState> owner,
                  ndn::Name scope, std::string id)
                : state(std::move(owner)), service(std::move(scope)), keyId(std::move(id)) {}
            ~Lease() noexcept
            {
                try {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->crypto) state->crypto->eraseWrappedSendKey(service, keyId);
                }
                catch (...) {}
            }
        };
    };

    struct ServiceUser::LargeDataFilePublication
    {
        ndn::Name baseName;
        std::filesystem::path filePath;
        std::uint64_t fileSize = 0;
        std::uint64_t segmentCount = 0;
        std::size_t maxSegmentBytes = 7000;
        ndn::time::milliseconds freshness{ndn::DEFAULT_FRESHNESS_PERIOD};
        std::ifstream stream;
        std::mutex streamMutex;
        std::size_t windowCapacity = 8;
        std::map<std::uint64_t, std::shared_ptr<const ndn::Data>> window;
        std::deque<std::uint64_t> windowOrder;
        std::uint64_t segmentReadCount = 0;
        std::uint64_t retransmissionHitCount = 0;
        std::size_t peakWindowSegments = 0;
        std::uintmax_t reservedBytes = 0;
        std::shared_ptr<const EncryptedLargeDataRangeSource> rangeSource;
        std::shared_ptr<const detail::OriginalPacketFile> originalPackets;
        std::shared_ptr<const ndn::Data> manifestPacket;
        bool originalPacketSource = false;
        std::weak_ptr<void> servingLease;
        std::shared_ptr<void> keyLease;
        std::string serviceName;
        std::string publicationIdentity;
        bool controllerVersioned = false;
        ControllerVersion controllerVersion;
        std::vector<std::string> protectedImsNames;

        ~LargeDataFilePublication()
        {
            stream.close();
            std::error_code error;
            if (!filePath.empty()) {
                std::filesystem::remove(filePath, error);
            }
        }

        std::vector<std::uint8_t> readSegment(std::uint64_t segment) {
            if (segment >= segmentCount) {
                throw std::out_of_range("large-data file segment is out of range");
            }
            const auto offset = segment * maxSegmentBytes;
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(
                maxSegmentBytes, fileSize - std::min(offset, fileSize)));
            std::vector<std::uint8_t> content(length);
            if (length == 0) {
                return content;
            }
            std::lock_guard<std::mutex> lock(streamMutex);
            ++segmentReadCount;
            if (rangeSource) {
                auto bytes = rangeSource->read(offset, length);
                if (bytes.size() != length)
                    throw std::runtime_error("short read from encrypted range store");
                return bytes;
            }
            stream.clear();
            stream.seekg(static_cast<std::streamoff>(offset));
            stream.read(reinterpret_cast<char*>(content.data()),
                        static_cast<std::streamsize>(length));
            if (static_cast<std::size_t>(stream.gcount()) != length) {
                throw std::runtime_error("short read from large-data file publication");
            }
            return content;
        }

        std::shared_ptr<const ndn::Data> findCached(std::uint64_t segment)
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            const auto it = window.find(segment);
            if (it == window.end()) {
                return nullptr;
            }
            ++retransmissionHitCount;
            return it->second;
        }

        void cache(std::uint64_t segment, std::shared_ptr<const ndn::Data> data)
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            if (windowCapacity == 0) {
                return;
            }
            if (window.find(segment) == window.end()) {
                windowOrder.push_back(segment);
            }
            window[segment] = std::move(data);
            while (windowOrder.size() > windowCapacity) {
                window.erase(windowOrder.front());
                windowOrder.pop_front();
            }
            peakWindowSegments = std::max(peakWindowSegments, window.size());
        }
    };

    namespace
    {
        void
        configureSvsProtocol(ndn::svs::SVSPubSubOptions& options)
        {
            std::string version = std::getenv("NDNSF_SVS_PROTOCOL_VERSION") != nullptr
                ? std::getenv("NDNSF_SVS_PROTOCOL_VERSION") : "v3";
            std::transform(version.begin(), version.end(), version.begin(),
                           [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (version == "v2" || version == "2") {
                options.syncProtocol.version = ndn::svs::SvsProtocolVersion::V2;
            }
            else if (version == "v3" || version == "3") {
                options.syncProtocol.version = ndn::svs::SvsProtocolVersion::V3;
            }
            else {
                throw std::invalid_argument("NDNSF_SVS_PROTOCOL_VERSION must be v2 or v3");
            }
            configureSvsPubSubOptionsFromEnvironment(options);
            if (const char* raw = std::getenv("NDNSF_SVS_MAX_SUPPRESSION_MS")) {
                try {
                    options.syncProtocol.suppressionPeriod =
                        ndn::time::milliseconds(std::max(0, std::stoi(raw)));
                }
                catch (const std::exception&) {
                    throw std::invalid_argument("NDNSF_SVS_MAX_SUPPRESSION_MS must be an integer");
                }
            }
        }

        std::string
        formatAttributesForLog(const std::vector<std::string>& attributes)
        {
            std::ostringstream os;
            for (size_t i = 0; i < attributes.size(); ++i) {
                if (i > 0) {
                    os << ",";
                }
                os << attributes[i];
            }
            return os.str();
        }

        ndn::Block
        makeNacInlineContentBlock(const ndn::Buffer& payload)
        {
            try {
                ndn::Block block(payload);
                if (block.type() == ndn::tlv::Content) {
                    return block;
                }
            }
            catch (const std::exception&) {
            }
            auto value = std::make_shared<ndn::Buffer>(payload);
            return ndn::Block(ndn::tlv::Content, value);
        }

        ndn::Block
        makeNacInlineContentBlock(ndn::span<const uint8_t> payload)
        {
            ndn::Buffer buffer(payload.size());
            if (!payload.empty()) {
                std::copy(payload.begin(), payload.end(), buffer.begin());
            }
            return makeNacInlineContentBlock(buffer);
        }

        std::map<std::string, std::string>
        parseSemicolonFields(const ndn::Buffer& payload)
        {
            std::map<std::string, std::string> fields;
            const std::string text(reinterpret_cast<const char*>(payload.data()),
                                   payload.size());
            size_t pos = 0;
            while (pos < text.size()) {
                const auto eq = text.find('=', pos);
                if (eq == std::string::npos) {
                    break;
                }
                const auto end = text.find(';', eq + 1);
                fields[text.substr(pos, eq - pos)] =
                    text.substr(eq + 1,
                                (end == std::string::npos ? text.size() : end) - eq - 1);
                if (end == std::string::npos) {
                    break;
                }
                pos = end + 1;
            }
            return fields;
        }

        ndn::Buffer
        textToBuffer(const std::string& text)
        {
            return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()),
                               text.size());
        }

        std::string
        bufferToText(const ndn::Buffer& payload)
        {
            return std::string(reinterpret_cast<const char*>(payload.data()),
                               payload.size());
        }

        ndn::Buffer
        mergeSelectionAssignmentPayloads(const ndn::Buffer& base,
                                         const ndn::Buffer& extra)
        {
            if (base.empty()) {
                return extra;
            }
            if (extra.empty()) {
                return base;
            }
            std::string merged = bufferToText(base);
            if (!merged.empty() && merged.back() != ';') {
                merged.push_back(';');
            }
            merged += bufferToText(extra);
            return textToBuffer(merged);
        }

        ndn::Buffer
        genericAdmissionLeaseSelectionPayloadFromAck(const RequestAckMessage& ack)
        {
            const auto fields = parseSemicolonFields(ack.getPayload());
            const auto leaseIt = fields.find("leaseId");
            if (leaseIt == fields.end() || leaseIt->second.empty()) {
                return ndn::Buffer();
            }
            std::string payload = "leaseId=" + leaseIt->second + ";";
            const auto proofIt = fields.find("resourceBindingProof");
            if (proofIt != fields.end() && !proofIt->second.empty()) {
                payload += "resourceBindingProof=" + proofIt->second + ";";
            }
            return textToBuffer(payload);
        }

        ndn::security::Certificate
        getExistingSigningCertificateOrFallback(ndn::KeyChain& keyChain,
                                                const ndn::security::Certificate& encryptionCert)
        {
            if (const char* value = std::getenv("NDNSF_DISABLE_SPLIT_SIGNING")) {
                std::string text(value);
                std::transform(text.begin(), text.end(), text.begin(),
                               [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!(text.empty() || text == "0" || text == "false" ||
                      text == "no" || text == "off")) {
                    return encryptionCert;
                }
            }
            const auto identityName = encryptionCert.getIdentity();
            try {
                auto identity = keyChain.getPib().getIdentity(identityName);
                for (const auto& key : identity.getKeys()) {
                    if (key.getKeyType() == ndn::KeyType::EC) {
                        try {
                            return key.getDefaultCertificate();
                        }
                        catch (const std::exception&) {
                            continue;
                        }
                    }
                }
            }
            catch (const std::exception&) {
                return encryptionCert;
            }
            return encryptionCert;
        }

        ndn::KeyType
        getCertificateKeyType(const ndn::security::Certificate& cert)
        {
            ndn::security::transform::PublicKey publicKey;
            publicKey.loadPkcs8(cert.getPublicKey());
            return publicKey.getKeyType();
        }

        bool
        isRsaCertificate(const ndn::security::Certificate& cert)
        {
            return getCertificateKeyType(cert) == ndn::KeyType::RSA;
        }

        ndn::security::Certificate
        getExistingEncryptionCertificateOrThrow(const ndn::security::Certificate& identityHintCert)
        {
            if (isRsaCertificate(identityHintCert)) {
                return identityHintCert;
            }

            ndn::KeyChain keyChain;
            const auto identityName = identityHintCert.getIdentity();
            try {
                auto identity = keyChain.getPib().getIdentity(identityName);
                for (const auto& key : identity.getKeys()) {
                    if (key.getKeyType() == ndn::KeyType::RSA) {
                        return key.getDefaultCertificate();
                    }
                }
            }
            catch (const std::exception&) {
            }

            throw std::invalid_argument("ServiceUser requires an RSA encryption certificate for NAC-ABE");
        }

        ndn::security::Certificate
        getExistingSigningCertificateOrFallback(const ndn::security::Certificate& encryptionCert)
        {
            ndn::KeyChain keyChain;
            return getExistingSigningCertificateOrFallback(keyChain, encryptionCert);
        }

        void
        ensureSameIdentity(const ndn::security::Certificate& encryptionCert,
                           const ndn::security::Certificate& signingCert,
                           const char* role)
        {
            if (encryptionCert.getIdentity() != signingCert.getIdentity()) {
                throw std::invalid_argument(std::string(role) +
                                            " encryptionCert and signingCert must share identity");
            }
        }

        double
        percentileLatency(std::vector<double> values, double percentileRank)
        {
            if (values.empty()) {
                return 0.0;
            }
            std::sort(values.begin(), values.end());
            const auto index = static_cast<size_t>(
                std::ceil((percentileRank / 100.0) * values.size()));
            return values[std::min(values.size() - 1, index == 0 ? 0 : index - 1)];
        }

        class FileLock
        {
        public:
            explicit FileLock(const char* path)
            {
                m_fd = open(path, O_CREAT | O_RDWR, 0666);
                if (m_fd < 0 || flock(m_fd, LOCK_EX) != 0) {
                    throw std::runtime_error("Failed to acquire file lock");
                }
            }

            ~FileLock()
            {
                if (m_fd >= 0) {
                    flock(m_fd, LOCK_UN);
                    close(m_fd);
                }
            }

        private:
            int m_fd = -1;
        };

        std::string
        userScopedLockPath(const std::string& base)
        {
            // Keep per-node SVS registration independent when several
            // MiniNDN applications share one UID or rootless namespace.
            if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
                const auto slash = base.find_last_of('/');
                const auto name = slash == std::string::npos ? base : base.substr(slash + 1);
                return std::string(home) + "/." + name + "-" + std::to_string(getuid()) + ".lock";
            }
            return base + "-" + std::to_string(getuid()) + ".lock";
        }

        bool
        isTruthyEnv(const char* name)
        {
            const char* value = std::getenv(name);
            if (value == nullptr) {
                return false;
            }
            std::string text(value);
            std::transform(text.begin(), text.end(), text.begin(),
                           [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return !(text.empty() || text == "0" || text == "false" ||
                     text == "no" || text == "off");
        }

        bool
        useFileBackedLargeData()
        {
            return isTruthyEnv("NDNSF_REQUEST_LARGE_FILE_BACKED");
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

        struct DurableLargeDataKeyReference
        {
            std::string publicationIdentity;
            std::string serviceName;
            std::string encryptedDataName;
            std::string contentDigest;
            std::uint64_t plaintextSize = 0;
            std::uint64_t policyEpoch = 0;
            std::uint64_t controllerGenerationTimestamp = 0;
            std::string protectionEpoch;
            std::string keyReferenceId;
            std::string keyReferenceVersion;
            std::string ciphertextManifestDigest;
            std::string servingLocator;
            std::string manifestName;
            std::string packetStorageFormat;
            bool needsLocationMigration = false;
        };

        // This store contains only Core-owned, non-secret identity metadata.
        // Repo holds original signed packets; no content key, private key, grant, or
        // request/KV state is written here.
        constexpr char DURABLE_KEY_REFERENCE_MAGIC[] =
            "NDNSF-DURABLE-LARGE-KEYREF-V3";
        constexpr char DURABLE_KEY_REFERENCE_V2_MAGIC[] =
            "NDNSF-DURABLE-LARGE-KEYREF-V2";
        constexpr char DURABLE_KEY_REFERENCE_LEGACY_MAGIC[] =
            "NDNSF-DURABLE-LARGE-KEYREF-V1";
        constexpr std::size_t DURABLE_KEY_REFERENCE_MAX_RECORDS = 4096;
        constexpr std::uint64_t DURABLE_KEY_REFERENCE_MAX_FIELD = 1U << 20;
        std::mutex durableKeyReferenceStoreMutex;

        std::string
        durableIdentityPathComponent(const ndn::Name& identity,
            ndn::name::UriFormat format = ndn::name::UriFormat::ENV_OR_ALTERNATE)
        {
            static constexpr char hex[] = "0123456789abcdef";
            std::string result;
            const auto uri = identity.toUri(format);
            result.reserve(uri.size() * 3);
            for (const auto ch : uri) {
                const auto value = static_cast<unsigned char>(ch);
                if (std::isalnum(value) || ch == '-' || ch == '_' || ch == '.') {
                    result.push_back(ch);
                }
                else {
                    result.push_back('_');
                    result.push_back(hex[value >> 4]);
                    result.push_back(hex[value & 0x0f]);
                }
            }
            return result.empty() ? "root" : result;
        }

        std::filesystem::path
        durableKeyReferenceStorePath(const ndn::Name& identity)
        {
            std::filesystem::path directory;
            if (const char* raw = std::getenv("NDNSF_DURABLE_KEY_REFERENCE_DIR");
                raw != nullptr && *raw != '\0') {
                directory = raw;
            }
            else if (const char* raw = std::getenv("NDNSF_RUNTIME_STATE_DIR");
                     raw != nullptr && *raw != '\0') {
                directory = raw;
            }
            else if (const char* raw = std::getenv("XDG_STATE_HOME");
                     raw != nullptr && *raw != '\0') {
                directory = std::filesystem::path(raw) / "ndnsf" /
                            "durable-key-references";
            }
            else {
                const char* home = std::getenv("HOME");
                directory = std::filesystem::path(home != nullptr ? home : "/tmp") /
                            ".local" / "state" / "ndnsf" /
                            "durable-key-references";
            }
            const auto& wire = identity.wireEncode();
            const auto digest = computeNamingDigest("ndnsf-naming/durable-owner",
                {{1, 1, std::vector<std::uint8_t>(wire.begin(), wire.end())}});
            return directory / ("user-" + digest + ".v3.dkr");
        }

        std::filesystem::path
        durableKeyReferenceLegacyStorePath(const ndn::Name& identity)
        {
            const auto v2 = durableKeyReferenceStorePath(identity);
            return v2.parent_path() /
                ("user-" + durableIdentityPathComponent(identity) + ".dkr");
        }

        bool
        writeDurableKeyReferenceString(std::ofstream& stream, const std::string& value)
        {
            if (value.size() > DURABLE_KEY_REFERENCE_MAX_FIELD) {
                return false;
            }
            const auto size = static_cast<std::uint64_t>(value.size());
            stream.write(reinterpret_cast<const char*>(&size), sizeof(size));
            if (size != 0) {
                stream.write(value.data(), static_cast<std::streamsize>(size));
            }
            return static_cast<bool>(stream);
        }

        bool
        readDurableKeyReferenceString(std::ifstream& stream, std::string& value)
        {
            std::uint64_t size = 0;
            stream.read(reinterpret_cast<char*>(&size), sizeof(size));
            if (!stream || size > DURABLE_KEY_REFERENCE_MAX_FIELD) {
                return false;
            }
            value.assign(static_cast<std::size_t>(size), '\0');
            if (size != 0) {
                stream.read(value.data(), static_cast<std::streamsize>(size));
            }
            return static_cast<bool>(stream);
        }

        bool
        writeDurableKeyReference(std::ofstream& stream,
                                 const DurableLargeDataKeyReference& record)
        {
            return writeDurableKeyReferenceString(stream, record.publicationIdentity) &&
                   writeDurableKeyReferenceString(stream, ndn::Name(record.serviceName).toUri(ndn::name::UriFormat::CANONICAL)) &&
                   writeDurableKeyReferenceString(stream, ndn::Name(record.encryptedDataName).toUri(ndn::name::UriFormat::CANONICAL)) &&
                   writeDurableKeyReferenceString(stream, record.contentDigest) &&
                   (stream.write(reinterpret_cast<const char*>(&record.plaintextSize),
                                 sizeof(record.plaintextSize)), static_cast<bool>(stream)) &&
                   (stream.write(reinterpret_cast<const char*>(&record.policyEpoch),
                                 sizeof(record.policyEpoch)), static_cast<bool>(stream)) &&
                   (stream.write(reinterpret_cast<const char*>(
                                     &record.controllerGenerationTimestamp),
                                 sizeof(record.controllerGenerationTimestamp)),
                    static_cast<bool>(stream)) &&
                   writeDurableKeyReferenceString(stream, record.protectionEpoch) &&
                   writeDurableKeyReferenceString(stream, record.keyReferenceId) &&
                   writeDurableKeyReferenceString(stream, record.keyReferenceVersion) &&
                   writeDurableKeyReferenceString(stream, record.ciphertextManifestDigest) &&
                   writeDurableKeyReferenceString(stream, ndn::Name(record.servingLocator).toUri(ndn::name::UriFormat::CANONICAL)) &&
                   writeDurableKeyReferenceString(stream, record.manifestName.empty() ? std::string{} :
                       ndn::Name(record.manifestName).toUri(ndn::name::UriFormat::CANONICAL)) &&
                   writeDurableKeyReferenceString(stream, record.packetStorageFormat);
        }

        bool
        readDurableKeyReference(std::ifstream& stream,
                                DurableLargeDataKeyReference& record)
        {
            return readDurableKeyReferenceString(stream, record.publicationIdentity) &&
                   readDurableKeyReferenceString(stream, record.serviceName) &&
                   readDurableKeyReferenceString(stream, record.encryptedDataName) &&
                   readDurableKeyReferenceString(stream, record.contentDigest) &&
                   (stream.read(reinterpret_cast<char*>(&record.plaintextSize),
                                sizeof(record.plaintextSize)), static_cast<bool>(stream)) &&
                   (stream.read(reinterpret_cast<char*>(&record.policyEpoch),
                                sizeof(record.policyEpoch)), static_cast<bool>(stream)) &&
                   (stream.read(reinterpret_cast<char*>(
                                    &record.controllerGenerationTimestamp),
                                sizeof(record.controllerGenerationTimestamp)),
                    static_cast<bool>(stream)) &&
                   readDurableKeyReferenceString(stream, record.protectionEpoch) &&
                   readDurableKeyReferenceString(stream, record.keyReferenceId) &&
                   readDurableKeyReferenceString(stream, record.keyReferenceVersion) &&
                   readDurableKeyReferenceString(stream, record.ciphertextManifestDigest) &&
                   readDurableKeyReferenceString(stream, record.servingLocator);
        }

        bool
        readLegacyDurableKeyReference(std::ifstream& stream,
                                      DurableLargeDataKeyReference& record)
        {
            const bool result =
                readDurableKeyReferenceString(stream, record.publicationIdentity) &&
                readDurableKeyReferenceString(stream, record.serviceName) &&
                readDurableKeyReferenceString(stream, record.encryptedDataName) &&
                readDurableKeyReferenceString(stream, record.contentDigest) &&
                (stream.read(reinterpret_cast<char*>(&record.plaintextSize),
                             sizeof(record.plaintextSize)), static_cast<bool>(stream)) &&
                (stream.read(reinterpret_cast<char*>(&record.policyEpoch),
                             sizeof(record.policyEpoch)), static_cast<bool>(stream)) &&
                readDurableKeyReferenceString(stream, record.protectionEpoch) &&
                readDurableKeyReferenceString(stream, record.keyReferenceId) &&
                readDurableKeyReferenceString(stream, record.keyReferenceVersion) &&
                readDurableKeyReferenceString(stream, record.ciphertextManifestDigest) &&
                readDurableKeyReferenceString(stream, record.servingLocator);
            // V1 did not bind a full ControllerVersion.  Treat its epoch as
            // unversioned compatibility metadata; only LocalMock may use it
            // for a one-time V2 migration.
            record.controllerGenerationTimestamp = 0;
            record.policyEpoch = 0;
            record.packetStorageFormat = "legacy-reference-v1";
            return result;
        }

        bool
        isLowerHex(const std::string& value)
        {
            return std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            });
        }

        bool
        isSha256Digest(const std::string& value)
        {
            return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
                   isLowerHex(value.substr(7));
        }

        bool
        isValidDurableKeyReference(const DurableLargeDataKeyReference& record,
                                   bool allowLegacyMetadata = false)
        {
            try {
                const bool legacy = allowLegacyMetadata && record.manifestName.empty() &&
                    (record.packetStorageFormat == "legacy-reference-v2" ||
                     record.packetStorageFormat == "legacy-reference-v1");
                if (!legacy && (record.packetStorageFormat != "original-packet-set" ||
                    ndn::Name(record.manifestName).empty() ||
                    !ProtectedBlobManifest::inferProducer(ndn::Name(record.manifestName))))
                    return false;
                if (ndn::Name(record.serviceName).empty() ||
                    ndn::Name(record.encryptedDataName).empty() ||
                    ndn::Name(record.servingLocator) != ndn::Name(record.encryptedDataName))
                    return false;
            }
            catch (const std::exception&) { return false; }
            return isSha256Digest(record.publicationIdentity) &&
                   !record.serviceName.empty() && record.serviceName.front() == '/' &&
                   !record.encryptedDataName.empty() && record.encryptedDataName.front() == '/' &&
                   isSha256Digest(record.contentDigest) && record.plaintextSize != 0 &&
                   ((record.controllerGenerationTimestamp == 0 && record.policyEpoch == 0) ||
                    (record.controllerGenerationTimestamp != 0 && record.policyEpoch != 0)) &&
                   record.protectionEpoch.size() == 16 && isLowerHex(record.protectionEpoch) &&
                   isSha256Digest(record.keyReferenceId) &&
                   record.keyReferenceVersion == "v1" &&
                   isSha256Digest(record.ciphertextManifestDigest);
        }

        // Called only while durableKeyReferenceStoreMutex is held.  This
        // remembers syntax validation, never authorization or file contents:
        // every table is still read and its envelope checked on every access.
        bool
        isValidReadDurableKeyReference(const DurableLargeDataKeyReference& record)
        {
            static std::map<std::string, DurableLargeDataKeyReference> validated;
            static std::size_t retainedBytes = 0;
            constexpr std::size_t maxRetainedBytes = 8 * 1024 * 1024;
            const auto fields = [] (const auto& item) {
                return std::tie(item.publicationIdentity, item.serviceName,
                    item.encryptedDataName, item.contentDigest, item.plaintextSize,
                    item.policyEpoch, item.controllerGenerationTimestamp,
                    item.protectionEpoch, item.keyReferenceId, item.keyReferenceVersion,
                    item.ciphertextManifestDigest, item.servingLocator,
                    item.manifestName, item.packetStorageFormat,
                    item.needsLocationMigration);
            };
            const auto bytes = [] (const auto& item) {
                return item.publicationIdentity.size() * 2 + item.serviceName.size() +
                    item.encryptedDataName.size() + item.contentDigest.size() +
                    item.protectionEpoch.size() + item.keyReferenceId.size() +
                    item.keyReferenceVersion.size() + item.ciphertextManifestDigest.size() +
                    item.servingLocator.size() + item.manifestName.size() +
                    item.packetStorageFormat.size();
            };
            auto found = validated.find(record.publicationIdentity);
            if (found != validated.end() && fields(found->second) == fields(record))
                return true;
            if (!isValidDurableKeyReference(record, true))
                return false;
            const auto size = bytes(record);
            if (size > maxRetainedBytes)
                return true; // Oversized valid records are checked each time.
            if (found != validated.end()) {
                retainedBytes -= bytes(found->second);
                validated.erase(found);
            }
            if (validated.size() >= DURABLE_KEY_REFERENCE_MAX_RECORDS ||
                size > maxRetainedBytes - retainedBytes) {
                validated.clear();
                retainedBytes = 0;
            }
            validated.emplace(record.publicationIdentity, record);
            retainedBytes += size;
            return true;
        }

        std::vector<DurableLargeDataKeyReference>
        readDurableKeyReferenceRecords(const std::filesystem::path& path, bool& valid)
        {
            valid = true;
            if (!std::filesystem::exists(path)) {
                return {};
            }
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                valid = false;
                return {};
            }
            std::string magic(sizeof(DURABLE_KEY_REFERENCE_MAGIC) - 1, '\0');
            stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
            const bool v2 = magic == DURABLE_KEY_REFERENCE_V2_MAGIC;
            if (!stream || (magic != DURABLE_KEY_REFERENCE_MAGIC && !v2)) {
                valid = false;
                return {};
            }
            std::uint64_t count = 0;
            stream.read(reinterpret_cast<char*>(&count), sizeof(count));
            if (!stream || count > DURABLE_KEY_REFERENCE_MAX_RECORDS) {
                valid = false;
                return {};
            }
            std::vector<DurableLargeDataKeyReference> records;
            records.reserve(static_cast<std::size_t>(count));
            std::set<std::string> identities;
            for (std::uint64_t i = 0; i < count; ++i) {
                DurableLargeDataKeyReference record;
                if (!readDurableKeyReference(stream, record)) {
                    valid = false;
                    return {};
                }
                if (v2) record.packetStorageFormat = "legacy-reference-v2";
                else if (!readDurableKeyReferenceString(stream, record.manifestName) ||
                         !readDurableKeyReferenceString(stream, record.packetStorageFormat)) {
                    valid = false;
                    return {};
                }
                if (record.packetStorageFormat == "legacy-reference-v1" ||
                    !isValidReadDurableKeyReference(record) ||
                    !identities.insert(record.publicationIdentity).second) {
                    valid = false;
                    return {};
                }
                records.push_back(std::move(record));
            }
            if (stream.peek() != std::char_traits<char>::eof()) {
                valid = false;
                return {};
            }
            return records;
        }

        std::optional<DurableLargeDataKeyReference>
        loadDurableKeyReference(const ndn::Name& identity,
                                const std::string& publicationIdentity)
        {
            try {
                std::lock_guard<std::mutex> guard(durableKeyReferenceStoreMutex);
                const auto currentPath = durableKeyReferenceStorePath(identity);
                const std::vector<std::filesystem::path> paths{
                    currentPath,
                    currentPath.parent_path() / ("user-" + durableIdentityPathComponent(
                        identity, ndn::name::UriFormat::CANONICAL) + ".v2.dkr"),
                    currentPath.parent_path() / ("user-" + durableIdentityPathComponent(
                        identity, ndn::name::UriFormat::ALTERNATE) + ".v2.dkr")};
                for (const auto& path : paths) {
                    std::error_code error;
                    if (!std::filesystem::exists(path, error)) {
                        if (error && path == currentPath) return std::nullopt;
                        continue;
                    }
                    FileLock lock((path.string() + ".lock").c_str());
                    bool valid = false;
                    const auto records = readDurableKeyReferenceRecords(path, valid);
                    if (!valid) {
                        if (path == currentPath) return std::nullopt;
                        continue;
                    }
                    for (auto record : records) {
                        if (record.publicationIdentity == publicationIdentity) {
                            record.needsLocationMigration = path != currentPath;
                            return record;
                        }
                    }
                }
            }
            catch (const std::exception&) {
                // A protected durable lookup must fail closed.  In
                // particular, a stale lock or unreadable state file must not
                // escape through the accepted request/status path.
            }
            return std::nullopt;
        }

        std::optional<DurableLargeDataKeyReference>
        loadLegacyDurableKeyReference(const ndn::Name& identity,
                                      const std::string& publicationIdentity)
        {
            try {
                std::lock_guard<std::mutex> guard(durableKeyReferenceStoreMutex);
                const auto path = durableKeyReferenceLegacyStorePath(identity);
                if (!std::filesystem::exists(path)) {
                    return std::nullopt;
                }
                FileLock lock((path.string() + ".lock").c_str());
                std::ifstream stream(path, std::ios::binary);
                if (!stream) {
                    return std::nullopt;
                }
                std::string magic(sizeof(DURABLE_KEY_REFERENCE_LEGACY_MAGIC) - 1, '\0');
                stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
                if (!stream || magic != DURABLE_KEY_REFERENCE_LEGACY_MAGIC) {
                    return std::nullopt;
                }
                std::uint64_t count = 0;
                stream.read(reinterpret_cast<char*>(&count), sizeof(count));
                if (!stream || count > DURABLE_KEY_REFERENCE_MAX_RECORDS) {
                    return std::nullopt;
                }
                std::set<std::string> identities;
                std::optional<DurableLargeDataKeyReference> selected;
                for (std::uint64_t i = 0; i < count; ++i) {
                    DurableLargeDataKeyReference record;
                    if (!readLegacyDurableKeyReference(stream, record) ||
                        !isValidDurableKeyReference(record, true) ||
                        !identities.insert(record.publicationIdentity).second) {
                        return std::nullopt;
                    }
                    if (record.publicationIdentity == publicationIdentity) {
                        selected = std::move(record);
                    }
                }
                if (stream.peek() != std::char_traits<char>::eof()) {
                    return std::nullopt;
                }
                return selected;
            }
            catch (const std::exception&) {
                // Legacy state is advisory only.  Invalid or inaccessible
                // V1 state must never authorize a protected hit.
            }
            return std::nullopt;
        }

        bool
        persistDurableKeyReference(const ndn::Name& identity,
                                   const DurableLargeDataKeyReference& record,
                                   bool replaceCorruptStore = false)
        {
            try {
                if (!isValidDurableKeyReference(record)) {
                    return false;
                }
                std::lock_guard<std::mutex> guard(durableKeyReferenceStoreMutex);
                const auto path = durableKeyReferenceStorePath(identity);
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                if (error) {
                    return false;
                }
                ::chmod(path.parent_path().c_str(), 0700);
                const auto lockPath = path.string() + ".lock";
                FileLock lock(lockPath.c_str());
                bool valid = false;
                auto records = readDurableKeyReferenceRecords(path, valid);
                if (!valid && replaceCorruptStore) {
                    const auto quarantine = path.string() + ".corrupt." +
                        std::to_string(::getpid());
                    std::filesystem::rename(path, quarantine, error);
                    if (error) {
                        return false;
                    }
                    records.clear();
                    valid = true;
                }
                if (!valid || records.size() >= DURABLE_KEY_REFERENCE_MAX_RECORDS) {
                    auto existing = std::find_if(records.begin(), records.end(),
                        [&record] (const auto& candidate) {
                            return candidate.publicationIdentity == record.publicationIdentity;
                        });
                    if (!valid || existing == records.end()) {
                        return false;
                    }
                }
                auto existing = std::find_if(records.begin(), records.end(),
                    [&record] (const auto& candidate) {
                        return candidate.publicationIdentity == record.publicationIdentity;
                });
                if (existing != records.end()) {
                    if (ndn::Name(existing->serviceName) != ndn::Name(record.serviceName)) {
                        return false;
                    }
                    const bool existingVersioned =
                        existing->controllerGenerationTimestamp != 0 ||
                        existing->policyEpoch != 0;
                    const bool recordVersioned =
                        record.controllerGenerationTimestamp != 0 ||
                        record.policyEpoch != 0;
                    if (existingVersioned && !recordVersioned) {
                        return false;
                    }
                    if (existingVersioned && recordVersioned) {
                        const ControllerVersion oldVersion{
                            existing->controllerGenerationTimestamp,
                            existing->policyEpoch};
                        const ControllerVersion newVersion{
                            record.controllerGenerationTimestamp,
                            record.policyEpoch};
                        if (oldVersion.compare(newVersion) > 0) {
                            return false;
                        }
                    }
                }
                if (existing == records.end()) {
                    records.push_back(record);
                }
                else {
                    *existing = record;
                }

                const auto temporary = path.string() + ".tmp." + std::to_string(::getpid());
                {
                    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                    if (!stream) {
                        return false;
                    }
                    ::chmod(temporary.c_str(), 0600);
                    stream.write(DURABLE_KEY_REFERENCE_MAGIC,
                                 sizeof(DURABLE_KEY_REFERENCE_MAGIC) - 1);
                    const auto count = static_cast<std::uint64_t>(records.size());
                    stream.write(reinterpret_cast<const char*>(&count), sizeof(count));
                    for (const auto& item : records) {
                        if (!writeDurableKeyReference(stream, item)) {
                            stream.close();
                            std::filesystem::remove(temporary, error);
                            return false;
                        }
                    }
                    stream.flush();
                    if (!stream) {
                        stream.close();
                        std::filesystem::remove(temporary, error);
                        return false;
                    }
                }
                std::filesystem::rename(temporary, path, error);
                if (error) {
                    std::filesystem::remove(temporary, error);
                    return false;
                }
                return true;
            }
            catch (const std::exception&) {
                return false;
            }
        }

        void
        pruneDurableKeyReferences(const ndn::Name& identity,
                                  const ndn::Name& serviceName,
                                  const ControllerVersion& version) noexcept
        {
            try {
                std::lock_guard<std::mutex> guard(durableKeyReferenceStoreMutex);
                const auto path = durableKeyReferenceStorePath(identity);
                if (!std::filesystem::exists(path)) {
                    return;
                }
                const auto lockPath = path.string() + ".lock";
                FileLock lock(lockPath.c_str());
                bool valid = false;
                auto records = readDurableKeyReferenceRecords(path, valid);
                if (!valid) {
                    return;
                }
                const auto oldSize = records.size();
                records.erase(std::remove_if(records.begin(), records.end(),
                    [&] (const auto& record) {
                        if (ndn::Name(record.serviceName) != serviceName) {
                            return false;
                        }
                        if (record.controllerGenerationTimestamp == 0 &&
                            record.policyEpoch == 0) {
                            return true;
                        }
                        const ControllerVersion candidate{
                            record.controllerGenerationTimestamp,
                            record.policyEpoch};
                        return candidate.compare(version) < 0;
                    }), records.end());
                if (records.size() == oldSize) {
                    return;
                }

                const auto temporary = path.string() + ".tmp." + std::to_string(::getpid());
                std::error_code error;
                {
                    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                    if (!stream) {
                        return;
                    }
                    ::chmod(temporary.c_str(), 0600);
                    stream.write(DURABLE_KEY_REFERENCE_MAGIC,
                                 sizeof(DURABLE_KEY_REFERENCE_MAGIC) - 1);
                    const auto count = static_cast<std::uint64_t>(records.size());
                    stream.write(reinterpret_cast<const char*>(&count), sizeof(count));
                    for (const auto& record : records) {
                        if (!writeDurableKeyReference(stream, record)) {
                            stream.close();
                            std::filesystem::remove(temporary, error);
                            return;
                        }
                    }
                    stream.flush();
                    if (!stream) {
                        stream.close();
                        std::filesystem::remove(temporary, error);
                        return;
                    }
                }
                std::filesystem::rename(temporary, path, error);
                if (error) {
                    std::filesystem::remove(temporary, error);
                }
            }
            catch (const std::exception&) {
                // Pruning is best effort.  The strict version check remains
                // the authoritative fail-closed guard for a stale reference.
            }
        }

        std::uintmax_t
        largeDataFileMinimumFreeBytes()
        {
            constexpr std::uintmax_t defaultMinimum = 256ULL * 1024ULL * 1024ULL;
            const char* raw = std::getenv("NDNSF_REQUEST_LARGE_DATA_MIN_FREE_BYTES");
            if (raw == nullptr || *raw == '\0') {
                return defaultMinimum;
            }
            try {
                std::string value(raw);
                size_t consumed = 0;
                const auto parsed = std::stoull(value, &consumed, 10);
                if (consumed != value.size()) {
                    throw std::invalid_argument("trailing characters");
                }
                return static_cast<std::uintmax_t>(parsed);
            }
            catch (const std::exception&) {
                throw std::invalid_argument(
                    "NDNSF_REQUEST_LARGE_DATA_MIN_FREE_BYTES must be an integer");
            }
        }

        size_t
        largeDataWindowSegments()
        {
            constexpr size_t defaultWindow = 8;
            constexpr size_t maximumWindow = 1024;
            const char* raw = std::getenv("NDNSF_REQUEST_LARGE_WINDOW_SEGMENTS");
            if (raw == nullptr || *raw == '\0') {
                return defaultWindow;
            }
            try {
                std::string value(raw);
                size_t consumed = 0;
                const auto parsed = std::stoull(value, &consumed, 10);
                if (consumed != value.size() || parsed == 0 || parsed > maximumWindow) {
                    throw std::invalid_argument("out of range");
                }
                return static_cast<size_t>(parsed);
            }
            catch (const std::exception&) {
                throw std::invalid_argument(
                    "NDNSF_REQUEST_LARGE_WINDOW_SEGMENTS must be an integer in [1,1024]");
            }
        }

        ndn::time::milliseconds
        largeDataFileRetention()
        {
            // Wire FreshnessPeriod controls network cacheability.  A
            // file-backed publication is also a request-scoped serving
            // owner, so its local file must outlive the delayed ACK,
            // Selection and Provider fetch path even when the wire freshness
            // is zero.  Keep the default bounded and make longer deployments
            // explicit rather than silently retaining files forever.
            constexpr std::uint64_t defaultRetentionMs = 5ULL * 60ULL * 1000ULL;
            constexpr std::uint64_t maximumRetentionMs = 24ULL * 60ULL * 60ULL * 1000ULL;
            const char* raw = std::getenv("NDNSF_REQUEST_LARGE_DATA_RETENTION_MS");
            if (raw == nullptr || *raw == '\0') {
                return ndn::time::milliseconds(defaultRetentionMs);
            }
            try {
                std::string value(raw);
                size_t consumed = 0;
                const auto parsed = std::stoull(value, &consumed, 10);
                if (consumed != value.size() || parsed == 0 || parsed > maximumRetentionMs) {
                    throw std::invalid_argument("out of range");
                }
                return ndn::time::milliseconds(parsed);
            }
            catch (const std::exception&) {
                throw std::invalid_argument(
                    "NDNSF_REQUEST_LARGE_DATA_RETENTION_MS must be an integer in [1,86400000]");
            }
        }

        int
        intEnvOrDefault(const char* name, int fallback)
        {
            const char* value = std::getenv(name);
            if (value == nullptr || *value == '\0') {
                return fallback;
            }
            try {
                return std::stoi(value);
            }
            catch (const std::exception&) {
                return fallback;
            }
        }

        /**
         * Return the explicitly requested IMS capacity for large encrypted
         * publications.  The default remains the historical bounded cache;
         * a caller handling a genuinely large object must opt in and choose a
         * finite value that its process can afford.  Silently falling back on
         * malformed values would turn a capacity diagnosis into a late,
         * partially published failure, so reject invalid overrides at
         * construction time.
         */
        size_t
        largeDataImsLimit()
        {
            constexpr size_t defaultLimit = 50000;
            constexpr size_t maximumLimit = 1000000;
            const char* raw = std::getenv("NDNSF_REQUEST_LARGE_IMS_LIMIT");
            if (raw == nullptr || *raw == '\0') {
                return defaultLimit;
            }
            try {
                std::string value(raw);
                size_t consumed = 0;
                const auto parsed = std::stoull(value, &consumed, 10);
                if (consumed != value.size() || parsed == 0 ||
                    parsed > maximumLimit) {
                    throw std::invalid_argument("out of range");
                }
                return static_cast<size_t>(parsed);
            }
            catch (const std::exception&) {
                throw std::invalid_argument(
                    "NDNSF_REQUEST_LARGE_IMS_LIMIT must be an integer in [1,1000000]");
            }
        }

        constexpr size_t TARGETED_TOKEN_BATCH_MIN = 1;
        constexpr size_t TARGETED_TOKEN_BATCH_DEFAULT = 256;
        constexpr size_t TARGETED_TOKEN_BATCH_ADAPTIVE_MIN = 8;
        constexpr size_t TARGETED_TOKEN_BATCH_MAX = 256;
        constexpr size_t TARGETED_TOKEN_POOL_MAX = 256;

        size_t
        clampTargetedTokenBatch(size_t value)
        {
            return std::clamp(value,
                              TARGETED_TOKEN_BATCH_MIN,
                              TARGETED_TOKEN_BATCH_MAX);
        }

        size_t
        parseTargetedTokenBatch(const std::string& value,
                                size_t fallback)
        {
            try {
                size_t parsed = 0;
                const auto result = std::stoul(value, &parsed);
                if (parsed != value.size()) {
                    return fallback;
                }
                return clampTargetedTokenBatch(static_cast<size_t>(result));
            }
            catch (const std::exception&) {
                return fallback;
            }
        }

        bool
        targetedTokenBatchAdaptiveEnabled()
        {
            // Adaptive refill sizing is deliberately opt-in.  The stable
            // default is a fixed, explicitly configured batch size so that
            // request latency and wire size do not depend on early demand
            // observations.
            return isTruthyEnv("NDNSF_TARGETED_TOKEN_ADAPTIVE");
        }

        size_t
        configuredTargetedTokenBatch()
        {
            return clampTargetedTokenBatch(static_cast<size_t>(std::max(
                static_cast<int>(TARGETED_TOKEN_BATCH_MIN),
                intEnvOrDefault("NDNSF_TARGETED_TOKEN_BATCH_SIZE",
                                static_cast<int>(TARGETED_TOKEN_BATCH_DEFAULT)))));
        }

        bool
        envIsSet(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr && *value != '\0';
        }

        double
        doubleEnvOrDefault(const char* name, double fallback)
        {
            const char* value = std::getenv(name);
            if (value == nullptr || *value == '\0') {
                return fallback;
            }
            try {
                return std::stod(value);
            }
            catch (const std::exception&) {
                return fallback;
            }
        }

        int
        adaptiveSvsPublicationFetchWindow(int fallback)
        {
            const double expectedRps =
                doubleEnvOrDefault("NDNSF_SVS_EXPECTED_RPS", 0.0);
            if (expectedRps <= 0.0 ||
                (envIsSet("NDNSF_SVS_ADAPTIVE_FETCH_WINDOW") &&
                 !isTruthyEnv("NDNSF_SVS_ADAPTIVE_FETCH_WINDOW"))) {
                return fallback;
            }

            const int minWindow =
                std::max(1, intEnvOrDefault("NDNSF_SVS_ADAPTIVE_FETCH_MIN_WINDOW", 32));
            const int maxWindow =
                std::max(minWindow, intEnvOrDefault("NDNSF_SVS_ADAPTIVE_FETCH_MAX_WINDOW", 128));
            const int scaledWindow =
                static_cast<int>(std::ceil(expectedRps * 0.64));
            return std::max(minWindow, std::min(maxWindow, scaledWindow));
        }

        int
        permissionFetchMaxAttempts()
        {
            const int defaultAttempts =
                std::max(1, intEnvOrDefault("NDNSF_PERMISSION_FETCH_RETRIES", 19) + 1);
            return std::max(1, intEnvOrDefault("NDNSF_PERMISSION_FETCH_MAX_ATTEMPTS",
                                               defaultAttempts));
        }

        int
        permissionFetchLifetimeMs()
        {
            return std::max(500, intEnvOrDefault("NDNSF_PERMISSION_FETCH_LIFETIME_MS", 4000));
        }

        uint64_t
        policyRevalidationPeriodMs()
        {
            // Deterministic scheduled revalidation for the Spec179 MiniNDN
            // gate.  Default 0 keeps the production near-expiry schedule
            // unchanged; a positive value forces the scheduled-refresh path
            // to re-fetch on a fixed period while the status is still far
            // from expiry, so a controller-side revocation/authorization
            // change is discovered inside a bounded campaign window.
            return static_cast<uint64_t>(
                std::max(0, intEnvOrDefault("NDNSF_POLICY_REVALIDATION_PERIOD_MS", 0)));
        }

        int
        permissionFetchRetryBackoffMs(int attempt)
        {
            const int baseMs =
                std::max(0, intEnvOrDefault("NDNSF_PERMISSION_FETCH_RETRY_BACKOFF_MS", 250));
            return baseMs * std::max(1, attempt);
        }

        ndn::Name
        makePermissionFullServiceName(const ndn::Name& providerName,
                                      const ndn::Name& serviceName)
        {
            if (providerName.isPrefixOf(serviceName)) {
                return serviceName;
            }
            ndn::Name fullName(providerName);
            fullName.append(serviceName);
            return fullName;
        }

        size_t
        defaultNdnsfWorkerThreads()
        {
            if (std::getenv("NDNSF_HANDLER_THREADS") == nullptr) {
                return 2;
            }
            return static_cast<size_t>(
                std::max(0, intEnvOrDefault("NDNSF_HANDLER_THREADS", 0)));
        }

        size_t
        defaultAckProcessingThreads()
        {
            if (std::getenv("NDNSF_ACK_THREADS") == nullptr) {
                return 2;
            }
            return static_cast<size_t>(
                std::max(0, intEnvOrDefault("NDNSF_ACK_THREADS", 0)));
        }

        bool
        useAsyncSvsPublish()
        {
            // Fire-and-forget async publication cannot report a background
            // prepare failure to the caller.  Runtime control messages require
            // commit-before-return reliability, so async is explicit opt-in.
            return std::getenv("NDNSF_SVS_ASYNC_PUBLISH") != nullptr &&
                   isTruthyEnv("NDNSF_SVS_ASYNC_PUBLISH");
        }

        bool
        containsRole(const std::vector<SelectedParticipant>& participants,
                     const CollaborationRole& role)
        {
            return std::any_of(participants.begin(), participants.end(),
                               [&role](const SelectedParticipant& participant) {
                                   return participant.role == role;
                               });
        }

        bool
        validateCollaborationSelection(
            const CollaborationPlan& plan,
            const std::vector<SelectedParticipant>& participants,
            std::string& reason)
        {
            for (const auto& role : plan.roles) {
                const auto count = static_cast<size_t>(
                    std::count_if(participants.begin(), participants.end(),
                                  [&role](const SelectedParticipant& participant) {
                                      return participant.role == role.role;
                                  }));
                if (count < role.minProviders) {
                    reason = "missing required collaboration role " + role.role;
                    return false;
                }
                if (role.maxProviders > 0 && count > role.maxProviders) {
                    reason = "too many providers selected for collaboration role " +
                             role.role;
                    return false;
                }
            }

            for (const auto& dependency : plan.dependencies) {
                if (!dependency.required) {
                    continue;
                }
                for (const auto& producer : dependency.producers) {
                    if (!containsRole(participants, producer)) {
                        reason = "missing dependency producer role " + producer +
                                 " for scope " + dependency.keyScope;
                        return false;
                    }
                }
                for (const auto& consumer : dependency.consumers) {
                    if (!containsRole(participants, consumer)) {
                        reason = "missing dependency consumer role " + consumer +
                                 " for scope " + dependency.keyScope;
                        return false;
                    }
                }
            }

            return true;
        }

        ndn::svs::SeqNo
        publishSvs(const std::shared_ptr<ndn::svs::SVSPubSub>& svs,
                   const ndn::Name& name,
                   const ndn::Block& content)
        {
            if (svs == nullptr) {
                return 0;
            }
            if (useAsyncSvsPublish()) {
                return svs->publishAsync(name, content);
            }
            return svs->publish(name, content);
        }

        std::vector<ndn::Data>
        copyOriginalSvsPublication(ndn::svs::SVSPubSub& publisher,
                                   const ndn::Name& outerName, const ndn::Name& logicalName)
        {
            auto& store = publisher.getSVSync().getDataStore();
            ndn::Interest discovery(outerName);
            discovery.setCanBePrefix(true);
            const auto first = store.find(discovery);
            if (!first) throw std::runtime_error("accepted Selection original packet missing");
            std::vector<ndn::Data> originals;
            size_t wireBytes = 0;
            const auto retain = [&](const ndn::Data& outer) {
                ndn::Data original(outer.getContent().blockFromValue());
                const auto& name = original.getName();
                if (name != logicalName && !(logicalName.isPrefixOf(name) && name.size() == logicalName.size() + 2 &&
                    name[-2].isVersion() && name[-1].isSegment()))
                    throw std::runtime_error("accepted Selection original packet name mismatch");
                const auto bytes = original.wireEncode().size();
                if (bytes > 16 * 1024 * 1024 - wireBytes)
                    throw std::runtime_error("Selection original cache exceeds 16MiB invocation bound");
                wireBytes += bytes;
                originals.push_back(std::move(original));
            };
            if (!first->getFinalBlock()) retain(*first);
            else {
                const auto& name = first->getName();
                if (!first->getFinalBlock()->isSegment() || name.size() != outerName.size() + 2 ||
                    !outerName.isPrefixOf(name) || !name[-2].isVersion() || !name[-1].isSegment())
                    throw std::runtime_error("accepted Selection outer segment geometry invalid");
                const auto base = name.getPrefix(-1);
                const auto final = first->getFinalBlock()->toSegment();
                if (final >= 16 * 1024 * 1024)
                    throw std::runtime_error("Selection original cache segment count exceeds invocation bound");
                for (uint64_t segment = 0; segment <= final; ++segment) {
                    ndn::Interest exact(ndn::Name(base).appendSegment(segment));
                    const auto outer = store.find(exact);
                    if (!outer || outer->getName() != exact.getName())
                        throw std::runtime_error("accepted Selection original segment missing");
                    retain(*outer);
                }
            }
            return originals;
        }

        bool
        decodeEncryptedPermissionResponseFromDataContent(
            const ndn::Data& data,
            EncryptedPermissionResponse& response)
        {
            const auto& content = data.getContent();
            if (content.type() == tlv::EncryptedPermissionResponseType) {
                return response.WireDecode(content);
            }

            auto [ok, block] = ndn::Block::fromBuffer(
                ndn::span<const uint8_t>(content.value(), content.value_size()));
            if (!ok) {
                return false;
            }
            return response.WireDecode(block);
        }

        bool
        payloadEquals(const RequestAckMessage& lhs, const RequestAckMessage& rhs)
        {
            const auto lhsPayload = lhs.getPayload();
            const auto rhsPayload = rhs.getPayload();
            return lhsPayload.size() == rhsPayload.size() &&
                   std::equal(lhsPayload.begin(),
                              lhsPayload.end(),
                              rhsPayload.begin());
        }

        bool
        ackEquals(const RequestAckMessage& lhs, const RequestAckMessage& rhs)
        {
            return lhs.getStatus() == rhs.getStatus() &&
                   lhs.getMessage() == rhs.getMessage() &&
                   lhs.getUserToken() == rhs.getUserToken() &&
                   lhs.getProviderToken() == rhs.getProviderToken() &&
                   payloadEquals(lhs, rhs);
        }

        uint64_t
        nowMilliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

#if defined(HAVE_TESTS)
        struct UnitTestClock
        {
            std::mutex mutex;
            std::function<uint64_t()> callback;
        };

        UnitTestClock&
        unitTestClock()
        {
            static UnitTestClock clock;
            return clock;
        }
#endif

        uint64_t
        nowMicroseconds()
        {
#if defined(HAVE_TESTS)
            auto& clock = unitTestClock();
            std::function<uint64_t()> callback;
            {
                std::lock_guard<std::mutex> lock(clock.mutex);
                callback = clock.callback;
            }
            if (callback) {
                return callback();
            }
#endif
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::string
        currentThreadIdForTrace()
        {
            std::ostringstream os;
            os << std::this_thread::get_id();
            return os.str();
        }

        bool
        envFlagEnabled(const char* name)
        {
            const char* value = std::getenv(name);
            if (value == nullptr) {
                return false;
            }
            const std::string text(value);
            return !(text.empty() || text == "0" || text == "false" ||
                     text == "FALSE" || text == "no" || text == "NO");
        }

        bool
        cryptoDiagEnabled()
        {
            return envFlagEnabled("NDNSF_CRYPTO_DIAG");
        }

        bool
        plaintextAckDiagEnabled()
        {
            return cryptoDiagEnabled() &&
                   envFlagEnabled("NDNSF_DIAG_PLAINTEXT_ACK");
        }

        bool
        plaintextResponseDiagEnabled()
        {
            return cryptoDiagEnabled() &&
                   envFlagEnabled("NDNSF_DIAG_PLAINTEXT_RESPONSE");
        }

        std::string
        cryptoStageForName(const ndn::Name& name, const ndn::Name& actor)
        {
            auto type = hybridMessageTypeForName(name, actor);
            std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return std::tolower(c); });
            return type;
        }

        void
        logCryptoDiag(const std::string& role,
                      const std::string& stage,
                      const std::string& op,
                      const std::string& mode,
                      const std::string& status,
                      uint64_t startUs,
                      uint64_t endUs,
                      const ndn::Name& name,
                      size_t bytes,
                      const std::string& error = "")
        {
            if (!cryptoDiagEnabled()) {
                return;
            }
            NDN_LOG_DEBUG("[NDNSF_CRYPTO_DIAG]"
                      << " role=" << role
                      << " stage=" << stage
                      << " op=" << op
                      << " mode=" << mode
                      << " status=" << status
                      << " start_us=" << startUs
                      << " end_us=" << endUs
                      << " duration_us=" << (endUs >= startUs ? endUs - startUs : 0)
                      << " name=" << name.toUri()
                      << " bytes=" << bytes);
            if (!error.empty()) {
                NDN_LOG_INFO(" error=" << error);
            }
            NDN_LOG_INFO('\n');
        }

        std::string
        makeOneTimeToken()
        {
            static std::random_device randomDevice;
            static constexpr char alphabet[] =
                "0123456789"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz";
            static constexpr size_t tokenLength = 16;

            std::string token;
            token.reserve(tokenLength);
            for (size_t i = 0; i < tokenLength; ++i) {
                token.push_back(alphabet[randomDevice() % (sizeof(alphabet) - 1)]);
            }
            return token;
        }

        std::string
        sha256DigestString(ndn::span<const uint8_t> payload,
                           const std::function<void()>& requireActive = {})
        {
            ndn::util::Sha256 digest;
            constexpr size_t chunkSize = 1024 * 1024;
            for (size_t offset = 0; offset < payload.size();) {
                if (requireActive) requireActive();
                const auto chunk = std::min(chunkSize, payload.size() - offset);
                digest << std::string(reinterpret_cast<const char*>(payload.data() + offset),
                                      chunk);
                offset += chunk;
            }
            auto hex = digest.toString();
            std::transform(hex.begin(), hex.end(), hex.begin(),
                           [] (unsigned char value) {
                               return static_cast<char>(std::tolower(value));
                           });
            return "sha256:" + hex;
        }

        std::string
        sha256DigestString(const ndn::Buffer& payload)
        {
            return sha256DigestString(
                ndn::span<const uint8_t>(payload.data(), payload.size()));
        }

        std::vector<uint8_t>
        encodeVarNumber(uint64_t value)
        {
            if (value < 253) {
                return {static_cast<uint8_t>(value)};
            }
            if (value <= 0xffff) {
                return {0xfd, static_cast<uint8_t>(value >> 8),
                        static_cast<uint8_t>(value)};
            }
            if (value <= 0xffffffffULL) {
                return {0xfe, static_cast<uint8_t>(value >> 24),
                        static_cast<uint8_t>(value >> 16),
                        static_cast<uint8_t>(value >> 8),
                        static_cast<uint8_t>(value)};
            }
            return {0xff, static_cast<uint8_t>(value >> 56),
                    static_cast<uint8_t>(value >> 48),
                    static_cast<uint8_t>(value >> 40),
                    static_cast<uint8_t>(value >> 32),
                    static_cast<uint8_t>(value >> 24),
                    static_cast<uint8_t>(value >> 16),
                    static_cast<uint8_t>(value >> 8),
                    static_cast<uint8_t>(value)};
        }

        void
        writeAll(int fd, const uint8_t* data, size_t size)
        {
            size_t written = 0;
            while (written < size) {
                const auto count = ::write(fd, data + written, size - written);
                if (count <= 0) {
                    throw std::system_error(errno, std::generic_category(),
                                            "short write to large-data wire file");
                }
                written += static_cast<size_t>(count);
            }
        }

        std::filesystem::path
        streamEncryptLargeDataEnvelope(const ndn::Buffer& key,
                                       ndn::span<const uint8_t> plaintext,
                                       ndn::span<const uint8_t> associatedData,
                                       HybridMessageEnvelope& envelope,
                                       const ndn::Name& baseName,
                                       size_t& wireBytes,
                                       const std::function<void()>& requireActive = {})
        {
            if (key.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                throw std::runtime_error("invalid AES-GCM key size");
            }
            constexpr size_t nonceSize = HybridMessageCrypto::NONCE_SIZE;
            constexpr size_t tagSize = HybridMessageCrypto::TAG_SIZE;
            ndn::Buffer nonce = envelope.getNonce();
            if (nonce.empty()) {
                nonce.resize(nonceSize);
                if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
                    throw std::runtime_error("RAND_bytes failed for large-data envelope");
            }
            if (nonce.size() != nonceSize)
                throw std::runtime_error("invalid nonce size for large-data envelope");
            envelope.setNonce(nonce);
            envelope.setCipherText(ndn::Buffer{});
            envelope.setAuthTag(ndn::Buffer(tagSize));
            auto skeleton = envelope.WireEncode();
            skeleton.parse();

            size_t innerLength = 0;
            for (const auto& element : skeleton.elements()) {
                if (element.type() == tlv::CipherTextType) {
                    innerLength += encodeVarNumber(element.type()).size() +
                                   encodeVarNumber(plaintext.size()).size() +
                                   plaintext.size();
                }
                else {
                    innerLength += element.size();
                }
            }

            const auto directory = largeDataFileDirectory();
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) {
                throw std::system_error(error, "cannot create large-data directory");
            }
            const auto available = std::filesystem::space(directory, error).available;
            if (error) {
                throw std::system_error(error, "cannot inspect large-data filesystem");
            }
            const auto minimumFree = largeDataFileMinimumFreeBytes();
            if (available < static_cast<std::uintmax_t>(plaintext.size()) + minimumFree) {
                throw std::runtime_error(
                    "large-data file publication has insufficient free disk space");
            }
            static std::atomic<uint64_t> sequence{0};
            const auto uri = baseName.toUri();
            const auto digest = sha256DigestString(ndn::Buffer(
                reinterpret_cast<const uint8_t*>(uri.data()), uri.size()));
            const auto stem = digest.size() > 7 ? digest.substr(7) : digest;
            const auto finalPath = directory / ("ndnsf-large-data-" + stem + "-" +
                                                std::to_string(getpid()) + "-" +
                                                std::to_string(sequence.fetch_add(1)) + ".wire");
            auto stagingPath = finalPath;
            stagingPath += ".staging";
            const int fd = ::open(stagingPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0) {
                throw std::system_error(errno, std::generic_category(),
                                         "cannot create large-data staging file");
            }
            EVP_CIPHER_CTX* cipher = nullptr;
            bool closed = false;
            auto cleanup = [&] {
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
                auto emit = [&] (const std::vector<uint8_t>& bytes) {
                    writeAll(fd, bytes.data(), bytes.size());
                };
                const auto topType = encodeVarNumber(skeleton.type());
                const auto topLength = encodeVarNumber(innerLength);
                emit(topType);
                emit(topLength);

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
                    EVP_EncryptUpdate(cipher, nullptr, &produced,
                                      associatedData.data(),
                                      static_cast<int>(associatedData.size())) != 1) {
                    throw std::runtime_error("AES-GCM streaming AAD failed");
                }
                std::vector<uint8_t> output(1024 * 1024);
                ndn::Buffer tag(tagSize);
                bool ciphertextEmitted = false;
                for (const auto& element : skeleton.elements()) {
                    if (element.type() != tlv::CipherTextType) {
                        if (element.type() == tlv::AuthTagType) {
                            if (!ciphertextEmitted) {
                                throw std::runtime_error("large-data envelope missing ciphertext");
                            }
                            if (EVP_EncryptFinal_ex(cipher, output.data(), &produced) != 1 ||
                                EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG,
                                                    static_cast<int>(tag.size()), tag.data()) != 1) {
                                throw std::runtime_error("AES-GCM streaming finalization failed");
                            }
                            const auto type = encodeVarNumber(element.type());
                            const auto length = encodeVarNumber(tag.size());
                            emit(type);
                            emit(length);
                            emit(std::vector<uint8_t>(tag.begin(), tag.end()));
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
                    size_t offset = 0;
                    while (offset < plaintext.size()) {
                        if (requireActive) requireActive();
                        const auto chunk = std::min(output.size(), plaintext.size() - offset);
                        if (EVP_EncryptUpdate(cipher, output.data(), &produced,
                                              plaintext.data() + offset,
                                              static_cast<int>(chunk)) != 1) {
                            throw std::runtime_error("AES-GCM streaming encryption failed");
                        }
                        writeAll(fd, output.data(), static_cast<size_t>(produced));
                        offset += chunk;
                    }
                    ciphertextEmitted = true;
                }
                if (!ciphertextEmitted) {
                    throw std::runtime_error("large-data envelope has no ciphertext element");
                }
                if (requireActive) requireActive();
                if (::fsync(fd) != 0) {
                    throw std::system_error(errno, std::generic_category(),
                                            "cannot fsync large-data wire file");
                }
                if (::close(fd) != 0) {
                    closed = true;
                    throw std::system_error(errno, std::generic_category(),
                                            "cannot close large-data wire file");
                }
                closed = true;
                EVP_CIPHER_CTX_free(cipher);
                cipher = nullptr;
                std::filesystem::rename(stagingPath, finalPath, error);
                if (error) {
                    throw std::system_error(error, "cannot commit large-data wire file");
                }
                wireBytes = static_cast<size_t>(std::filesystem::file_size(finalPath));
                return finalPath;
            }
            catch (...) {
                cleanup();
                throw;
            }
        }

        ndn::Name
        makeUserAuthorizationAttribute(const ndn::Name& serviceName)
        {
            ndn::Name attribute("/PERMISSION");
            attribute.append(serviceName);
            return attribute;
        }

        EncryptionCertificateAdvertisement
        makeEncryptionCertificateAdvertisement(
            const ndn::security::Certificate& certificate)
        {
            EncryptionCertificateAdvertisement advertisement;
            advertisement.certificateName = certificate.getName();
            const auto wire = certificate.wireEncode();
            advertisement.certificateDigest = sha256DigestString(
                ndn::Buffer(wire.data(), wire.data() + wire.size()));
            const auto period = certificate.getValidityPeriod().getPeriod();
            const auto toMilliseconds = [] (const auto& point) -> uint64_t {
                return static_cast<uint64_t>(boost::chrono::duration_cast<
                    boost::chrono::milliseconds>(point.time_since_epoch()).count());
            };
            advertisement.validFromMs = toMilliseconds(period.first);
            advertisement.validUntilMs = toMilliseconds(period.second);
            advertisement.supportedEnvelopeAlgorithms = {"RSA-OAEP-SHA256"};
            return advertisement;
        }

        void
        logValidatedPublicationAudit(
            const char* role,
            const char* messageType,
            const ndn::svs::SVSPubSub::SubscriptionData& subscription,
            const ndn::Name& requestId,
            const ndn::Name& serviceName,
            const ndn::Name& requesterName,
            const ndn::Name& providerName)
        {
            if (!ndn_cxx_getLogger().isLevelEnabled(ndn::util::LogLevel::INFO)) {
                return;
            }
            std::string packetName = "-";
            std::string signerKeyLocator = "-";
            std::string wireDigest = sha256DigestString(
                ndn::Buffer(subscription.data.begin(), subscription.data.end()));
            if (subscription.packet) {
                packetName = subscription.packet->getName().toUri();
                const auto& signatureInfo = subscription.packet->getSignatureInfo();
                if (signatureInfo.hasKeyLocator() &&
                    signatureInfo.getKeyLocator().getType() == ndn::tlv::Name) {
                    signerKeyLocator =
                        signatureInfo.getKeyLocator().getName().toUri();
                }
                const auto wire = subscription.packet->wireEncode();
                wireDigest = sha256DigestString(ndn::Buffer(
                    wire.data(), wire.data() + wire.size()));
            }
            NDN_LOG_INFO("NDNSF_PUBLICATION_AUDIT role=" << role
                         << " type=" << messageType
                         << " validated=true"
                         << " packetPresent=" << (subscription.packet ? "true" : "false")
                         << " packetName=" << packetName
                         << " producerPrefix=" << subscription.producerPrefix.toUri()
                         << " seqNo=" << subscription.seqNo
                         << " signerKeyLocator=" << signerKeyLocator
                         << " wireDigest=" << wireDigest
                         << " requestId=" << requestId.toUri()
                         << " serviceName=" << serviceName.toUri()
                         << " requesterName=" << requesterName.toUri()
                         << " providerName=" << providerName.toUri());
        }

        ndn_service_framework::AckAuthenticationEvidence
        makeAckAuthenticationEvidence(
            const ndn::svs::SVSPubSub::SubscriptionData& subscription)
        {
            ndn_service_framework::AckAuthenticationEvidence evidence;
            // Direct unit helpers may not have an SVS packet.  Do not
            // manufacture provenance from the ACK name or decrypted payload;
            // production verification must fail closed when this is empty.
            if (!subscription.packet) {
                return evidence;
            }

            // OnRequestAck is reached from the SVS subscription after the
            // configured Trust Schema validator has accepted the Data. Keep
            // that fact as explicit provenance; Python must not infer it from
            // a reconstructed Provider name or key map.
            evidence.trustSchemaValidated = true;

            const auto wire = subscription.packet->wireEncode();
            evidence.wireDigest = sha256DigestString(ndn::Buffer(
                wire.data(), wire.data() + wire.size()));

            const auto& signatureInfo = subscription.packet->getSignatureInfo();
            if (!signatureInfo.hasKeyLocator() ||
                signatureInfo.getKeyLocator().getType() != ndn::tlv::Name) {
                return evidence;
            }

            const auto& keyLocatorName = signatureInfo.getKeyLocator().getName();
            evidence.signerKeyLocator = keyLocatorName.toUri();
            try {
                if (ndn::security::Certificate::isValidName(keyLocatorName)) {
                    evidence.signerIdentity =
                        ndn::security::extractIdentityFromCertName(keyLocatorName).toUri();
                }
                else if (ndn::security::isValidKeyName(keyLocatorName)) {
                    evidence.signerIdentity =
                        ndn::security::extractIdentityFromKeyName(keyLocatorName).toUri();
                }
            }
            catch (const std::exception&) {
                // Preserve the wire digest for diagnostics, but leave the
                // identity empty so a production verifier rejects the ACK.
                evidence.signerIdentity.clear();
            }
            return evidence;
        }

        ndn::Buffer
        userCollaborationAssociatedData(
            const ndn::Name& dataName,
            const ndn::Name& requestId,
            const CollaborationDataMessage& message,
            const std::string& keyId,
            const std::string& epochId)
        {
            const auto wireKeyId = hybridCompactKeyId(keyId);
            const std::string text =
                dataName.toUri(ndn::name::UriFormat::CANONICAL) + "|COLLAB|" + requestId.toUri(ndn::name::UriFormat::CANONICAL) + "|" +
                message.getKeyScope() + "|" + message.getTopic().toUri(ndn::name::UriFormat::CANONICAL) + "|" +
                message.getProducerRole() + "|" +
                std::to_string(message.getSequence()) + "|" + wireKeyId + "|" +
                epochId;
            return ndn::Buffer(
                reinterpret_cast<const uint8_t*>(text.data()), text.size());
        }

        void
        appendDigestField(std::string& output, const std::string& value)
        {
            output += std::to_string(value.size());
            output.push_back(':');
            output.append(value);
        }

        void
        appendDigestBuffer(std::string& output, const ndn::Buffer& value)
        {
            output += std::to_string(value.size());
            output.push_back(':');
            output.append(reinterpret_cast<const char*>(value.data()), value.size());
        }

        std::string
        deferredAckClosureDigest(
            const ndn::Name& requestId,
            uint64_t requestDeadlineUs,
            std::vector<AckCandidate> candidates)
        {
            std::sort(candidates.begin(), candidates.end(),
                      [](const AckCandidate& lhs, const AckCandidate& rhs) {
                          const auto left = std::make_tuple(
                              lhs.providerName.toUri(), lhs.serviceName.toUri(),
                              lhs.requestId.toUri());
                          const auto right = std::make_tuple(
                              rhs.providerName.toUri(), rhs.serviceName.toUri(),
                              rhs.requestId.toUri());
                          return left < right;
                      });
            std::string canonical;
            appendDigestField(canonical, "ndnsf-collaboration-ack-closed-v1");
            appendDigestField(canonical, requestId.toUri());
            appendDigestField(canonical, std::to_string(requestDeadlineUs));
            appendDigestField(canonical, std::to_string(candidates.size()));
            for (const auto& candidate : candidates) {
                appendDigestField(canonical, candidate.providerName.toUri());
                appendDigestField(canonical, candidate.serviceName.toUri());
                appendDigestField(canonical, candidate.requestId.toUri());
                const auto wire = candidate.ack.WireEncode();
                appendDigestBuffer(canonical, ndn::Buffer(
                    wire.data(), wire.data() + wire.size()));
            }
            return sha256DigestString(ndn::Buffer(
                reinterpret_cast<const uint8_t*>(canonical.data()),
                canonical.size()));
        }

        std::string
        deferredPlanCommitDigest(
            const std::string& ackClosedDigest,
            const CollaborationPlan& plan,
            std::vector<SelectedParticipant> participants)
        {
            std::sort(participants.begin(), participants.end(),
                      [](const SelectedParticipant& lhs,
                         const SelectedParticipant& rhs) {
                          return std::make_tuple(
                              lhs.role, lhs.provider.toUri(), lhs.service.toUri()) <
                                 std::make_tuple(
                              rhs.role, rhs.provider.toUri(), rhs.service.toUri());
                      });
            std::string canonical;
            appendDigestField(canonical, "ndnsf-collaboration-plan-commit-v1");
            appendDigestField(canonical, ackClosedDigest);
            appendDigestField(canonical, std::to_string(plan.roles.size()));
            for (const auto& role : plan.roles) {
                appendDigestField(canonical, role.role);
                appendDigestField(canonical, role.service.toUri());
                appendDigestField(canonical, role.requiredArtifact.toUri());
                appendDigestField(canonical,
                                  role.allowDynamicProvisioning ? "1" : "0");
                appendDigestField(
                    canonical, std::to_string(role.provisioningTimeoutMs));
                appendDigestBuffer(canonical, role.appRequirement);
                appendDigestBuffer(canonical, role.assignmentPayload);
                appendDigestField(canonical, std::to_string(role.minProviders));
                appendDigestField(canonical, std::to_string(role.maxProviders));
                appendDigestField(canonical,
                                  role.terminalResponseOwner ? "1" : "0");
            }
            appendDigestField(canonical, std::to_string(plan.keyScopes.size()));
            for (const auto& scope : plan.keyScopes) {
                appendDigestField(canonical, scope.name);
                appendDigestField(canonical, std::to_string(scope.roles.size()));
                for (const auto& role : scope.roles) {
                    appendDigestField(canonical, role);
                }
            }
            appendDigestField(
                canonical, std::to_string(plan.dependencies.size()));
            for (const auto& dependency : plan.dependencies) {
                appendDigestField(canonical, dependency.keyScope);
                appendDigestField(canonical, dependency.topicPrefix.toUri());
                appendDigestField(canonical, dependency.required ? "1" : "0");
                appendDigestField(
                    canonical, std::to_string(dependency.producers.size()));
                for (const auto& role : dependency.producers) {
                    appendDigestField(canonical, role);
                }
                appendDigestField(
                    canonical, std::to_string(dependency.consumers.size()));
                for (const auto& role : dependency.consumers) {
                    appendDigestField(canonical, role);
                }
            }
            appendDigestBuffer(canonical, plan.sharedAssignmentMetadata);
            appendDigestField(canonical, std::to_string(participants.size()));
            for (const auto& participant : participants) {
                appendDigestField(canonical, participant.role);
                appendDigestField(canonical, participant.service.toUri());
                appendDigestField(canonical, participant.provider.toUri());
                appendDigestField(canonical, participant.assignedArtifact.toUri());
                appendDigestField(
                    canonical, participant.requiresProvisioning ? "1" : "0");
                appendDigestField(
                    canonical, std::to_string(participant.provisioningTimeoutMs));
                appendDigestBuffer(canonical, participant.assignmentPayload);
                const auto ackWire = participant.ack.ack.WireEncode();
                appendDigestBuffer(canonical, ndn::Buffer(
                    ackWire.data(), ackWire.data() + ackWire.size()));
            }
            return sha256DigestString(ndn::Buffer(
                reinterpret_cast<const uint8_t*>(canonical.data()),
                canonical.size()));
        }

        std::string
        quoteNamingJson(const std::string& value)
        {
            static constexpr char hex[] = "0123456789abcdef";
            std::string out = "\"";
            for (unsigned char c : value) {
                if (c == '\"' || c == '\\') { out += '\\'; out += c; }
                else if (c < 0x20) {
                    out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
                }
                else out += c;
            }
            return out + "\"";
        }

        struct ProtectedBlobIdentity {
            ndn::Name dataPrefix;
            ndn::Buffer nonce;
            std::string representationSpec;
            std::string protectionSpec;
        };

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
            // Both JSON objects use sorted keys and canonical scalar spelling.
            // They contain descriptors only, never secret key material.
            const auto representation = std::string("{\"aad\":\"canonical-name-and-service-uri\",\"encoding\":\"ndnsf-hybrid-envelope\",\"freshnessMs\":") +
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

        bool
        isProtectedBlobDataPrefix(const ndn::Name& name, const ndn::Name& producer,
                                  const std::string& contentDigest,
                                  const PublishedContentName& contentName)
        {
            if (contentDigest.size() != 71 || contentDigest.compare(0, 7, "sha256:") != 0)
                return false;
            const auto prefix = contentName.contentPrefix(producer, contentDigest.substr(7));
            return prefix.isPrefixOf(name) && name.size() == prefix.size() + 3 &&
                name[-3] == ndn::name::Component("PROTECTED") &&
                name[-2].type() == 8 && name[-2].value_size() == 32 &&
                name[-1] == ndn::name::Component("DATA");
        }

        std::optional<ndn::Name>
        extractPermissionControllerIdentity(const ndn::Interest& interest, const ndn::Name& expected,
          const std::map<ndn::Name, ndn::Interest>* preparedQueries = nullptr)
        {
            auto producer = expected;
            if (producer.empty()) {
                if (!preparedQueries || interest.getName().size() <= 6 ||
                    !interest.getName()[-1].isParametersSha256Digest()) return std::nullopt;
                auto owner = interest.getName().getPrefix(-2);
                owner.append("private-owner");
                const auto prepared = preparedQueries->find(owner);
                if (prepared == preparedQueries->end() ||
                    prepared->second.getName() != interest.getName()) return std::nullopt;
                // Resolve only from our own prepared POLICY-STATUS query,
                // before permission fetching has configured a Controller.
                producer = prepared->second.getName().getPrefix(-6);
                if (!parseControlRpcName(prepared->second, producer, "POLICY-STATUS"))
                    return std::nullopt;
            }
            const auto parsed = parseProducerDataName(interest.getName(), producer);
            if (!parsed || parsed->category != "RPC" || parsed->suffix.size() != 4 ||
                parsed->suffix[0].type() != 8) return std::nullopt;
            const auto& c = parsed->suffix[0];
            const std::string operation(reinterpret_cast<const char*>(c.value()), c.value_size());
            return parseControlRpcName(interest, producer, operation) ? std::optional<ndn::Name>(producer) : std::nullopt;
        }

        bool
        isSignedByIdentity(const ndn::Data& data, const ndn::Name& expectedIdentity)
        {
            if (!data.getSignatureInfo().hasKeyLocator() ||
                data.getSignatureInfo().getKeyLocator().getType() != ndn::tlv::Name) {
                return false;
            }

            const auto signerIdentity = ndn::security::extractIdentityFromCertName(
                data.getSignatureInfo().getKeyLocator().getName());
            return signerIdentity == expectedIdentity;
        }

        int
        statusHexValue(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return -1;
        }

        ndn::Buffer
        decodeStatusHex(const std::string& text)
        {
            if (text.size() % 2 != 0 || text.size() > 8192) return {};
            ndn::Buffer result(text.size() / 2);
            for (size_t i = 0; i < result.size(); ++i) {
                const int hi = statusHexValue(text[i * 2]);
                const int lo = statusHexValue(text[i * 2 + 1]);
                if (hi < 0 || lo < 0) return {};
                result[i] = static_cast<uint8_t>((hi << 4) | lo);
            }
            return result;
        }
    }

    namespace
    {
        class FirstRespondingPolicy final : public AckSelectionPolicy
        {
        public:
            std::vector<ProviderId>
            select(const std::vector<AckCandidate>& candidates) const override
            {
                for (const auto& candidate : candidates) {
                    if (candidate.ack.getStatus()) {
                        return {candidate.providerName};
                    }
                }
                return {};
            }
        };

        class RandomSelectionPolicy final : public AckSelectionPolicy
        {
        public:
            std::vector<ProviderId>
            select(const std::vector<AckCandidate>& candidates) const override
            {
                std::vector<ProviderId> validProviders;
                for (const auto& candidate : candidates) {
                    if (candidate.ack.getStatus()) {
                        validProviders.push_back(candidate.providerName);
                    }
                }
                if (validProviders.empty()) {
                    return {};
                }

                static thread_local std::mt19937 generator(std::random_device{}());
                std::uniform_int_distribution<size_t> distribution(0, validProviders.size() - 1);
                return {validProviders[distribution(generator)]};
            }
        };

        class AllSelectedPolicy final : public AckSelectionPolicy
        {
        public:
            std::vector<ProviderId>
            select(const std::vector<AckCandidate>& candidates) const override
            {
                std::vector<ProviderId> selected;
                for (const auto& candidate : candidates) {
                    if (candidate.ack.getStatus()) {
                        selected.push_back(candidate.providerName);
                    }
                }
                return selected;
            }

            size_t
            requestStrategy() const override
            {
            return ndn_service_framework::tlv::AllSelected;
            }
        };
    }

    namespace strategy
    {
        const std::shared_ptr<const AckSelectionPolicy> FirstResponding =
            std::make_shared<FirstRespondingPolicy>();
        const std::shared_ptr<const AckSelectionPolicy> RandomSelection =
            std::make_shared<RandomSelectionPolicy>();
        const std::shared_ptr<const AckSelectionPolicy> AllSelected =
            std::make_shared<AllSelectedPolicy>();
    }

    ServiceUser::ServiceUser(ndn::Face &face,
                             ndn::Name group_prefix,
                             ndn::security::Certificate identityCert,
                             ndn::security::Certificate attrAuthorityCertificate,
                             std::string trustSchemaPath)
        : ServiceUser(ExternalKeyChainTag{}, face,
                      std::move(group_prefix),
                      getExistingEncryptionCertificateOrThrow(identityCert),
                      getExistingSigningCertificateOrFallback(identityCert),
                      std::move(attrAuthorityCertificate),
                      std::move(trustSchemaPath), nullptr)
    {
    }

    ServiceUser::ServiceUser(ndn::Face &face,
                             ndn::Name group_prefix,
                             ndn::security::Certificate encryptionCert,
                             ndn::security::Certificate signingCert,
                             ndn::security::Certificate attrAuthorityCertificate,
                             std::string trustSchemaPath)
        : ServiceUser(ExternalKeyChainTag{}, face,
                      std::move(group_prefix), std::move(encryptionCert),
                      std::move(signingCert), std::move(attrAuthorityCertificate),
                      std::move(trustSchemaPath), nullptr)
    {
    }

    ServiceUser::ServiceUser(ndn::Face &face,
                             ndn::Name group_prefix,
                             ndn::security::Certificate encryptionCert,
                             ndn::security::Certificate signingCert,
                             ndn::security::Certificate attrAuthorityCertificate,
                             std::string trustSchemaPath,
                             ndn::KeyChain& signingKeyChain)
        : ServiceUser(ExternalKeyChainTag{}, face,
                      std::move(group_prefix), std::move(encryptionCert),
                      std::move(signingCert), std::move(attrAuthorityCertificate),
                      std::move(trustSchemaPath), &signingKeyChain)
    {
    }

    ServiceUser::ServiceUser(ExternalKeyChainTag,
                             ndn::Face &face,
                             ndn::Name group_prefix,
                             ndn::security::Certificate encryptionCert,
                             ndn::security::Certificate signingCert,
                             ndn::security::Certificate attrAuthorityCertificate,
                             std::string trustSchemaPath,
                             ndn::KeyChain* externalKeyChain) :
        m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(encryptionCert.getIdentity()),
        m_testSigningKeyChain(externalKeyChain),
        validator(std::make_shared<MessageValidator>(
          trustSchemaPath, group_prefix, &face)),
        identityCert(encryptionCert),
        signingCert(signingCert),
        attrAuthorityCertificate(attrAuthorityCertificate),
        // nac_validator(std::move(ndn::security::ValidatorNull())),
        nacConsumer(m_face, externalKeyChain ? *externalKeyChain : m_keyChain,
                    nac_validator, encryptionCert, attrAuthorityCertificate),
        nacProducer(m_face, externalKeyChain ? *externalKeyChain : m_keyChain,
                    nac_validator, encryptionCert, attrAuthorityCertificate),
        m_IMS(largeDataImsLimit())
    {
        ensureSameIdentity(encryptionCert, signingCert, "ServiceUser");
        if (!isRsaCertificate(encryptionCert)) {
            throw std::invalid_argument("ServiceUser encryptionCert must be RSA for NAC-ABE");
        }
        NDN_LOG_WARN("NDNSF_CERT_SELECTION role=user identity="
                     << identity.toUri()
                     << " encryptionCert=" << encryptionCert.getName()
                     << " signingCert=" << signingCert.getName()
                     << " splitSigning="
                     << (encryptionCert.getName() == signingCert.getName() ? "false" : "true"));
        m_handlerPool.setThreadCount(defaultNdnsfWorkerThreads());
        NDN_LOG_INFO("NDNSF_HANDLER_THREADS role=user workers="
                     << m_handlerPool.getThreadCount());
        m_ackProcessingPool.setThreadCount(defaultAckProcessingThreads());
        NDN_LOG_INFO("NDNSF_ACK_THREADS role=user workers="
                     << m_ackProcessingPool.getThreadCount());
        if (isTruthyEnv("NDNSF_ENABLE_NDNSD") &&
            std::getenv("NDNSF_DISABLE_NDNSD") == nullptr) {
            m_ServiceDiscovery.enable(group_prefix,
                                      identity,
                                      face,
                                      externalKeyChain ? *externalKeyChain : m_keyChain,
                                      std::bind(&ServiceUser::processNDNSDServiceInfoCallback, this, _1));
        }

        loadTrustSchema(nac_validator, trustSchemaPath);

        // Register the identity prefix as a route so other processes can
        // fetch this User's signed APP Data (the spec181 grant Data is
        // fetched by Providers on other MiniNDN nodes; without the route
        // their Interests die with NoRoute before reaching the IMS filter).
        // Bounded retry mirrors registerInterestFilterWithRetry: NFD's
        // offline command authenticator can transiently miss the freshly
        // created signer certificate while the node PIB is briefly locked.
        registerIdentityPrefixWithRetry();

        // Serve NDNSF and ck messages using IMS.  Registration uses a short
        // bounded retry because NFD's offline command authenticator can
        // transiently miss the freshly created signer certificate while the
        // node PIB is briefly locked by a concurrent keychain operation.
        for (const auto& servicePrefix :
             {ndn::Name(identity.toUri()).append("NDNSF"),
              // spec181: the KEY-GRANT namespace uses the Spec170 spelling
              // NDNSF-DI (single component); serve it from the same IMS.
              ndn::Name(identity.toUri()).append("NDNSF-DI"),
              ndn::Name(identity.toUri()).append("CK")}) {
            registerInterestFilterWithRetry(
                m_face,
                servicePrefix,
                std::bind(&ServiceUser::onInterest, this, _1, _2),
                std::function<void(const ndn::Name&)>(),
                std::bind(&ServiceUser::onPrefixRegisterFailure, this, _1, _2),
                6,
                std::chrono::milliseconds(250),
                [&] {
                    auto holder = std::make_shared<ndn::ScopedRegisteredPrefixHandle>();
                    m_serviceRegistrations.push_back(holder);
                    return holder;
                }());
        }

        m_signingInfo = ndn::security::signingByCertificate(signingCert);

        // Runtime may bind a caller-owned KeyChain before init().  Use that
        // same owner for every SVS signer; otherwise the certificate supplied
        // to ServiceUser can be present only in an unrelated PIB.
        auto& signingKeyChain = m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain;
        ndn::svs::SecurityOptions secOpts(signingKeyChain);
        secOpts.interestSigner = std::make_shared<CommandInterestSigner>(signingKeyChain);
        secOpts.interestSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.interestSigner->signingInfo.setSigningKeyName(signingCert.getKeyName());
        secOpts.dataSigner->signingInfo.setSigningCertName(signingCert.getName());
        secOpts.dataSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.pubSigner->signingInfo.setSigningCertName(signingCert.getName());
        secOpts.pubSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.validator = validator;
        secOpts.encapsulatedDataValidator = validator->makeEncapsulatedValidator();

        // Do not fetch publications older than 10 seconds
        ndn::svs::SVSPubSubOptions opts;
        configureSvsProtocol(opts);
        NDN_LOG_INFO("NDNSF_SVS_OPTIONS role=user"
                     << " maxApplicationParametersSize=" << opts.maxApplicationParametersSize
                     << " maxPiggyDataSize=" << opts.maxPiggyDataSize);
        #ifdef USE_TIMESTAMP
        opts.useTimestamp = true;
        // opts.maxPubAge = ndn::time::seconds(0);
        #else
        opts.useTimestamp = false;
        #endif
        opts.publicationFetchRetries =
            std::max(0, intEnvOrDefault("NDNSF_SVS_PUBLICATION_FETCH_RETRIES",
                                        opts.publicationFetchRetries));
        opts.publicationFetchInnerRetries =
            std::max(0, intEnvOrDefault("NDNSF_SVS_PUBLICATION_FETCH_INNER_RETRIES",
                                        opts.publicationFetchInnerRetries));
        opts.publicationFetchInterestLifetime =
            ndn::time::milliseconds(std::max(100, intEnvOrDefault(
                "NDNSF_SVS_PUBLICATION_FETCH_LIFETIME_MS",
                static_cast<int>(opts.publicationFetchInterestLifetime.count()))));
        opts.publicationFetchFailureBackoff =
            ndn::time::milliseconds(std::max(0, intEnvOrDefault(
                "NDNSF_SVS_PUBLICATION_FETCH_BACKOFF_MS",
                static_cast<int>(opts.publicationFetchFailureBackoff.count()))));
        opts.publicationFetchMaxBackoff =
            ndn::time::milliseconds(std::max(0, intEnvOrDefault(
                "NDNSF_SVS_PUBLICATION_FETCH_MAX_BACKOFF_MS",
                static_cast<int>(opts.publicationFetchMaxBackoff.count()))));
        const int adaptiveFetchWindow =
            adaptiveSvsPublicationFetchWindow(static_cast<int>(opts.publicationFetchWindow));
        opts.publicationFetchWindow =
            static_cast<uint16_t>(std::max(1, intEnvOrDefault(
                "NDNSF_SVS_PUBLICATION_FETCH_WINDOW",
                adaptiveFetchWindow)));
        NDN_LOG_WARN("NDNSF_SVS_PUBLICATION_FETCH_CONFIG role=user retries="
                     << opts.publicationFetchRetries
                     << " innerRetries=" << opts.publicationFetchInnerRetries
                     << " lifetimeMs=" << opts.publicationFetchInterestLifetime.count()
                     << " backoffMs=" << opts.publicationFetchFailureBackoff.count()
                     << " maxBackoffMs=" << opts.publicationFetchMaxBackoff.count()
                     << " window=" << opts.publicationFetchWindow
                     << " expectedRps="
                     << doubleEnvOrDefault("NDNSF_SVS_EXPECTED_RPS", 0.0)
                     << " explicitWindow="
                     << (envIsSet("NDNSF_SVS_PUBLICATION_FETCH_WINDOW") ? "true" : "false"));

        ndn::Name node_id(identity);
        node_id.append("user");
        int session_id = m_configManager.loadAndIncrement(group_prefix.toUri(), node_id.toUri());
        node_id.append(std::to_string(session_id));
        {
            const auto svsLockPath = userScopedLockPath("/tmp/ndnsf-svs-registration");
            FileLock svsRegistrationLock(svsLockPath.c_str());
            m_svsps = std::make_shared<ndn::svs::SVSPubSub>(
                ndn::Name(group_prefix),
                node_id,
                m_face,
                std::bind(&ServiceUser::onMissingData, this, _1),
                opts,
                secOpts);
            const auto& syncProfile = m_svsps->getSyncProtocolOptions();
            NDN_LOG_INFO("NDNSF_SVS_PROTOCOL role=user version="
                         << static_cast<int>(syncProfile.version)
                         << " lifetimeMs=" << syncProfile.syncInterestLifetime.count()
                         << " suppressionMs=" << syncProfile.suppressionPeriod.count()
                         << " periodicMs=" << syncProfile.periodicTimeout.count());
            if (std::getenv("NDNSF_SVS_PERIODIC_SYNC_MS") != nullptr) {
                const int periodicSyncMs =
                    std::max(1, intEnvOrDefault("NDNSF_SVS_PERIODIC_SYNC_MS", 30000));
                m_svsps->getSVSync().getCore().setPeriodicSyncTime(
                    ndn::time::milliseconds(periodicSyncMs));
                NDN_LOG_INFO("NDNSF_SVS_PERIODIC_SYNC_MS role=user value="
                             << periodicSyncMs);
            }
            NDN_LOG_INFO("NDNSF_SVS_ASYNC_PUBLISH role=user "
                         << (useAsyncSvsPublish() ? "enabled" : "disabled"));
            // Parallel Sync receive processing is an experimental optimization.
            // Replaying a fixed mobility trace showed that worker results can
            // delay a reconnected Provider's state beyond the ACK window.  Keep
            // the serial correctness path as the default until that state
            // transition is repaired in NDN-SVS.
            const bool enableParallelSync =
                std::getenv("NDNSF_SVS_PARALLEL_SYNC") != nullptr &&
                isTruthyEnv("NDNSF_SVS_PARALLEL_SYNC");
            if (enableParallelSync) {
                const int workers = std::max(1, intEnvOrDefault("NDNSF_SVS_PARALLEL_WORKERS", 4));
                const int queue = std::max(1, intEnvOrDefault("NDNSF_SVS_PARALLEL_QUEUE", 256));
                m_svsps->getSVSync().getCore().setParallelSyncProcessing(
                    true, static_cast<size_t>(workers), static_cast<size_t>(queue));
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_SYNC enabled role=user workers="
                             << workers << " queue=" << queue);
            }
            else {
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_SYNC disabled role=user"
                             " reason=explicit-opt-in-required");
            }
            // Parallel Sync production is an experimental optimization.  A
            // mobility regression showed that it can delay a reconnected
            // producer's state until another peer gossips that state.  Keep
            // the serial correctness path as the default and require an
            // explicit opt-in while the production state machine is repaired.
            const bool enableParallelProduction =
                std::getenv("NDNSF_SVS_PARALLEL_PRODUCTION") != nullptr &&
                isTruthyEnv("NDNSF_SVS_PARALLEL_PRODUCTION");
            if (enableParallelProduction) {
                const int workers = std::max(
                    1, intEnvOrDefault("NDNSF_SVS_PARALLEL_PRODUCTION",
                                       intEnvOrDefault("NDNSF_SVS_PARALLEL_WORKERS", 4)));
                const int queue = std::max(1, intEnvOrDefault("NDNSF_SVS_PARALLEL_QUEUE", 256));
                // Keep Sync Interest signing on the Face/io_context thread by
                // default. Worker signing can assign monotonically increasing
                // timestamps in an order different from expressInterest(),
                // which lets remote validators observe reordered timestamps.
                const bool signInWorker =
                    std::getenv("NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING") != nullptr &&
                    isTruthyEnv("NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING");
                const bool extraBlockInWorker =
                    std::getenv("NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK") == nullptr ||
                    isTruthyEnv("NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK");
                m_svsps->getSVSync().getCore().setParallelSyncProduction(
                    true, static_cast<size_t>(workers), static_cast<size_t>(queue),
                    signInWorker, extraBlockInWorker);
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_PRODUCTION enabled role=user workers="
                             << workers << " queue=" << queue
                             << " signInWorker=" << signInWorker
                             << " extraBlockInWorker=" << extraBlockInWorker);
            }
            else {
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_PRODUCTION disabled role=user"
                             " reason=explicit-opt-in-required");
            }
            if (isTruthyEnv("NDNSF_SVS_SYNC_BATCHING")) {
                const int windowMs = std::max(0, intEnvOrDefault("NDNSF_SVS_SYNC_BATCH_MS", 5));
                m_svsps->getSVSync().getCore().setSyncInterestBatching(
                    true, ndn::time::milliseconds(windowMs));
                NDN_LOG_INFO("NDNSF_SVS_SYNC_BATCHING enabled role=user windowMs="
                             << windowMs);
            }
        }

        // Construction must not wait for authorization: an unprovisioned
        // identity needs a live application to request/renew permissions.
        // The Consumer fetch and permission/status-driven refresh complete
        // asynchronously; protected paths still require installed authority.
        nacConsumer.obtainDecryptionKey();
        if (!nacConsumer.readyForDecryption())
            NDN_LOG_INFO("NDNSF_NAC_BOOTSTRAP_PENDING role=user");

        // Opt-in durable runtime status (NDNSF_PERSIST_RUNTIME_STATE, FR-039):
        // re-verify and seed statuses accepted by an earlier process of this
        // identity, then confirm each against its Controller online.
        if (RuntimeStatusStore::enabled()) {
            m_runtimeStatusStore = std::make_unique<RuntimeStatusStore>(
                RuntimeStatusStore::defaultStorePath("user", identity));
        }
        restorePersistedRuntimeStatuses();

    }

    ServiceUser::ServiceUser(LocalMockTag,
                             ndn::Face& face,
                             ndn::Name group_prefix,
                             ndn::security::Certificate identityCert,
                             ndn::security::Certificate attrAuthorityCertificate,
                             std::string trustSchemaPath)
        : ServiceUser(LocalMockTag{},
                      face,
                      std::move(group_prefix),
                      getExistingEncryptionCertificateOrThrow(identityCert),
                      getExistingSigningCertificateOrFallback(identityCert),
                      std::move(attrAuthorityCertificate),
                      std::move(trustSchemaPath))
    {
    }

    ServiceUser::ServiceUser(LocalMockTag,
                             ndn::Face& face,
                             ndn::Name group_prefix,
                             ndn::security::Certificate encryptionCert,
                             ndn::security::Certificate signingCert,
                             ndn::security::Certificate attrAuthorityCertificate,
                             std::string trustSchemaPath)
        : m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(encryptionCert.getIdentity()),
        m_keyChain(),
        m_svsps(nullptr),
        validator(std::make_shared<MessageValidator>(trustSchemaPath, group_prefix)),
        nac_validator(m_face),
        identityCert(encryptionCert),
        signingCert(signingCert),
        attrAuthorityCertificate(attrAuthorityCertificate),
        nacConsumer(m_face, m_keyChain, nac_validator, encryptionCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, encryptionCert, attrAuthorityCertificate),
        m_IMS(largeDataImsLimit()),
        m_configManager("/tmp/ndnsf-service-user-local-mock.conf")
    {
        m_isLocalMock = true;
        ensureSameIdentity(encryptionCert, signingCert, "ServiceUser");
        if (!isRsaCertificate(encryptionCert)) {
            throw std::invalid_argument("ServiceUser encryptionCert must be RSA for NAC-ABE");
        }
        // LocalMockTag still owns real NAC-ABE Consumer/Producer instances.
        // Load the same trust schema as the production constructor before the
        // fixture pumps their constructor-time public-parameter Interests.
        loadTrustSchema(nac_validator, trustSchemaPath);
        m_signingInfo = ndn::security::signingByCertificate(signingCert);
        // Opt-in durable runtime status (NDNSF_PERSIST_RUNTIME_STATE, FR-039):
        // re-verify and seed statuses accepted by an earlier process of this
        // identity, then confirm each against its Controller online.  The
        // LocalMock trust schema resolves synchronously.
        if (RuntimeStatusStore::enabled()) {
            m_runtimeStatusStore = std::make_unique<RuntimeStatusStore>(
                RuntimeStatusStore::defaultStorePath("user", identity));
        }
        restorePersistedRuntimeStatuses();
    }

    void ServiceUser::init()
    {
        registerNDNSFMessages();
    }

    void
    ServiceUser::attachLocalMockPubSubForTest(
        std::shared_ptr<ndn::svs::SVSPubSub> pubSub)
    {
        if (pubSub == nullptr) {
            throw std::invalid_argument(
                "ServiceUser LocalMock PubSub cannot be null");
        }
        if (m_svsps != nullptr) {
            throw std::logic_error(
                "ServiceUser PubSub is already initialized");
        }
        m_svsps = std::move(pubSub);

        // The LocalMock constructor intentionally skips the production
        // constructor's Face registrations.  Install the same IMS content
        // filters here so post-Selection SegmentFetcher Interests can fetch
        // assignment artifacts from this test user.
        const ndn::Name ndnsfFilter = ndn::Name(identity.toUri()).append("NDNSF");
        const ndn::Name diFilter = ndn::Name(identity.toUri()).append("NDNSF-DI");
        const ndn::Name ckFilter = ndn::Name(identity.toUri()).append("CK");
        auto registerContentFilter = [this](const ndn::Name& prefix) {
            auto holder = std::make_shared<ndn::ScopedInterestFilterHandle>();
            m_testInterestFilters.push_back(holder);
            *holder = m_face.setInterestFilter(
                ndn::InterestFilter(prefix),
                std::bind(&ServiceUser::onInterest, this, _1, _2));
        };
        registerContentFilter(ndnsfFilter);
        registerContentFilter(diFilter);
        registerContentFilter(ckFilter);
    }

    void
    ServiceUser::useSigningKeyChain(ndn::KeyChain& keyChain)
    {
        const auto identity = keyChain.getPib().getIdentity(signingCert.getIdentity());
        const auto key = identity.getKey(signingCert.getKeyName());
        (void)key.getCertificate(signingCert.getName());
        m_testSigningKeyChain = &keyChain;
        m_testNacConsumer = std::make_unique<ndn::nacabe::Consumer>(
            m_face, keyChain, nac_validator, identityCert,
            attrAuthorityCertificate);
        m_testNacConsumer->obtainDecryptionKey();
        m_testNacProducer = std::make_unique<ndn::nacabe::CacheProducer>(
            m_face, keyChain, nac_validator, identityCert,
            attrAuthorityCertificate);
        m_testNacProducer->refreshPublicParameters();
    }

    void
    ServiceUser::useSigningKeyChainForTest(ndn::KeyChain& keyChain)
    {
        useSigningKeyChain(keyChain);
    }

    void
    ServiceUser::useSigningKeyChainForSigningOnlyForTest(ndn::KeyChain& keyChain)
    {
        const auto identity = keyChain.getPib().getIdentity(signingCert.getIdentity());
        const auto key = identity.getKey(signingCert.getKeyName());
        (void)key.getCertificate(signingCert.getName());
        m_testSigningKeyChain = &keyChain;
    }

    bool
    ServiceUser::isNacConsumerReadyForTest()
    {
        return activeNacConsumer().readyForDecryption();
    }

    bool
    ServiceUser::isNacProducerReadyForTest()
    {
        const auto& producer = activeNacProducer();
        return !producer.getPublicParamsDataName().empty() &&
               !producer.getPublicParamsDigest().empty();
    }

    void
    ServiceUser::refreshNacProducerForTest()
    {
        activeNacProducer().refreshPublicParameters();
    }

    void
    ServiceUser::cacheHybridReceiveKeyForTest(const std::string& keyId,
                                               const std::string& epochId,
                                               const ndn::Buffer& key)
    {
        m_hybridMessageCrypto.cacheReceiveKey(keyId, epochId, key);
    }

    HybridMessageKey
    ServiceUser::prepareHybridSendKeyForTest(const ndn::Name& serviceName,
                                              const std::string& messageType)
    {
        if (messageType != "REQUEST" && messageType != "SELECTION" &&
            messageType != "REQUEST-LARGE") {
            throw std::invalid_argument(
                "LocalMock user outbound Hybrid key must be REQUEST, "
                "REQUEST-LARGE, or SELECTION");
        }
        const auto accessAttribute = makeAbeServiceAttribute(serviceName, "SERVICE");
        auto key = m_hybridMessageCrypto.getOrCreateSendKey(
            serviceName, identity, accessAttribute, messageType, m_hybridCryptoCounters);
        // The LocalMock hook represents a key that Core/NAC has already
        // wrapped.  Keep the service-scoped wrapped bytes and reference
        // index in sync with production retain/release bookkeeping; the
        // placeholder is never sent because shouldAttachWrappedKey() is false.
        m_hybridMessageCrypto.cacheWrappedSendKey(
            serviceName, key.keyId, ndn::Buffer{0x00});
        return key;
    }

    void
    ServiceUser::cacheDataForTest(
        const ndn::Data& data,
        ndn::time::milliseconds freshness)
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        m_IMS.insert(data, freshness);
    }

    void ServiceUser::registerIdentityPrefixWithRetry(size_t attempts)
    {
        if (!m_identityRegistration) {
            m_identityRegistration =
                std::make_shared<ndn::ScopedRegisteredPrefixHandle>();
        }
        const std::weak_ptr<ndn::ScopedRegisteredPrefixHandle> owner =
            m_identityRegistration;
        *m_identityRegistration = m_face.registerPrefix(
            identity,
            [](const ndn::Name&) {},
            [this, owner, attempts](const ndn::Name& prefix,
                                    const std::string& reason) {
                const auto registration = owner.lock();
                if (!registration) {
                    return;
                }
                onPrefixRegisterFailure(prefix, reason);
                if (attempts >= 7) {
                    return;
                }
                m_scheduler.schedule(ndn::time::milliseconds(250),
                    [this, owner, attempts] {
                        const auto registration = owner.lock();
                        if (registration) {
                            registerIdentityPrefixWithRetry(attempts + 1);
                        }
                    });
            });
    }

    ServiceUser::~ServiceUser()
    {
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            m_policyStatusFetchInFlight.clear();
        }
        if (m_securityProfiles) m_securityProfiles->close();
        if (m_largeDataKeyReleaseState) {
            std::lock_guard<std::mutex> lock(m_largeDataKeyReleaseState->mutex);
            m_largeDataKeyReleaseState->crypto = nullptr;
        }
        // Invalidate pending failure/timer callbacks before tearing down state.
        // The scoped handle also unregisters a successfully installed route.
        m_identityRegistration.reset();
        if (m_svsps != nullptr) {
            const auto stats = m_svsps->getSVSync().getCore().getSyncProcessingStats();
            NDN_LOG_INFO("NDNSF_SVS_SYNC_STATS role=user"
                         << " submitted=" << stats.syncJobsSubmitted
                         << " completed=" << stats.syncJobsCompleted
                         << " dropped=" << stats.syncJobsDropped
                         << " stale=" << stats.syncJobsStale
                         << " queueDepth=" << stats.syncWorkerQueueDepth
                         << " workerMs=" << stats.syncWorkerProcessingMs
                         << " publishMs=" << stats.syncMainThreadPublishMs
	                         << " serialMs=" << stats.syncInterestSerialHandlerMs
	                         << " parallelTotalMs=" << stats.syncInterestParallelTotalMs
	                         << " mainBlockingMs=" << stats.syncInterestMainThreadBlockingMs
	                         << " productionSubmitted=" << stats.syncProductionJobsSubmitted
	                         << " productionCompleted=" << stats.syncProductionJobsCompleted
	                         << " productionDropped=" << stats.syncProductionJobsDropped
	                         << " productionStale=" << stats.syncProductionJobsStale
	                         << " productionQueueDepth=" << stats.syncProductionWorkerQueueDepth);
            const auto rejection = m_svsps->getSVSync().getCore().getSyncRejectionStats();
            // The system NDN-SVS ABI used by this build exposes the core
            // rejection counters but not the newer fetch/piggyback counters.
            // Keep the portable diagnostics here; provider-specific counters
            // must be added only when the linked SVS ABI exposes them.
            NDN_LOG_INFO("NDNSF_SVS_DELIVERY_STATS role=user"
                         << " malformed=" << rejection.malformedEnvelope
                         << " signaturePolicy=" << rejection.signaturePolicy
                         << " vectorDecode=" << rejection.vectorDecode
                         << " fetchStatsAbi=unavailable");
	            m_svsps.reset();
        }
        m_cryptoProduceQueue.shutdown();
        m_ackProcessingPool.shutdown();
        m_handlerPool.shutdown();
    }

    void ServiceUser::setRequestPublisher(RequestPublisher publisher)
    {
        m_requestPublisher = std::move(publisher);
    }

    void ServiceUser::setLocalPublicationHandler(LocalPublicationHandler handler)
    {
        m_localPublicationHandler = std::move(handler);
    }

    ndn::Buffer ServiceUser::makeGenericAdmissionLeaseSelectionPayload(
        const std::string& leaseId,
        const ndn::Buffer& resourceBindingProof)
    {
        std::string payload = "leaseId=" + leaseId + ";";
        if (!resourceBindingProof.empty()) {
            payload += "resourceBindingProof=" + bufferToText(resourceBindingProof) + ";";
        }
        return textToBuffer(payload);
    }

    bool ServiceUser::setSelectionAssignmentPayloadForRequest(
        const ndn::Name& requestId,
        const ndn::Name& providerName,
        const ndn::Buffer& assignmentPayload)
    {
        auto pendingIt = m_pendingCalls.find(requestId);
        if (pendingIt == m_pendingCalls.end()) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_ASSIGNMENT_PAYLOAD_REJECTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " reason=pending_missing");
            return false;
        }
        pendingIt->second.selectionAssignmentPayloads[providerName.toUri()] =
            assignmentPayload;
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_ASSIGNMENT_PAYLOAD_SET timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " payloadBytes=" << assignmentPayload.size());
        return true;
    }

    void ServiceUser::setRequestLifecycleCallback(RequestLifecycleCallback callback)
    {
        m_requestLifecycleCallback = std::move(callback);
    }

    void ServiceUser::setAdmissionControlWarningHandler(AdmissionControlWarningHandler handler)
    {
        m_admissionControlWarningHandler = std::move(handler);
    }

    void ServiceUser::setAdmissionControlRejectHandler(AdmissionControlRejectHandler handler)
    {
        m_admissionControlRejectHandler = std::move(handler);
    }

    void ServiceUser::setHandlerThreads(size_t n)
    {
        m_handlerPool.setThreadCount(n);
        NDN_LOG_WARN("NDNSF user worker threads: " << n);
    }

    size_t ServiceUser::getHandlerThreads() const
    {
        return m_handlerPool.getThreadCount();
    }

    size_t ServiceUser::getHandlerQueueDepth() const
    {
        return m_handlerPool.getQueueSize();
    }

    void ServiceUser::setAckProcessingThreads(size_t n)
    {
        m_ackProcessingPool.setThreadCount(n);
        NDN_LOG_WARN("NDNSF user ACK processing threads: " << n);
    }

    size_t ServiceUser::getAckProcessingThreads() const
    {
        return m_ackProcessingPool.getThreadCount();
    }

    size_t ServiceUser::getAckProcessingQueueDepth() const
    {
        return m_ackProcessingPool.getQueueSize();
    }

    const char* ServiceUser::requestLifecycleStateToString(RequestLifecycleState state)
    {
        switch (state) {
        case RequestLifecycleState::QUEUED_LOCAL: return "QUEUED_LOCAL";
        case RequestLifecycleState::ADMISSION_DELAYED: return "ADMISSION_DELAYED";
        case RequestLifecycleState::ADMITTED: return "ADMITTED";
        case RequestLifecycleState::REQUEST_PUBLISHED: return "REQUEST_PUBLISHED";
        case RequestLifecycleState::ACK_MATCHED: return "ACK_MATCHED";
        case RequestLifecycleState::PROVIDER_SELECTED: return "PROVIDER_SELECTED";
        case RequestLifecycleState::SELECTION_PUBLISHED: return "SELECTION_PUBLISHED";
        case RequestLifecycleState::RESPONSE_OBSERVED: return "RESPONSE_OBSERVED";
        case RequestLifecycleState::RESPONSE_DECRYPTED: return "RESPONSE_DECRYPTED";
        case RequestLifecycleState::CALLBACK_FIRED: return "CALLBACK_FIRED";
        case RequestLifecycleState::COMPLETED: return "COMPLETED";
        case RequestLifecycleState::ADMISSION_REJECTED: return "ADMISSION_REJECTED";
        case RequestLifecycleState::TIMED_OUT: return "TIMED_OUT";
        case RequestLifecycleState::CANCELLED_OR_DROPPED: return "CANCELLED_OR_DROPPED";
        }
        return "UNKNOWN";
    }

    std::optional<ServiceUser::RequestLifecycleStatus>
    ServiceUser::getRequestStatus(const ndn::Name& requestId) const
    {
        auto it = m_requestLifecycleStatuses.find(requestId);
        if (it == m_requestLifecycleStatuses.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<ServiceUser::RequestLifecycleStatus>
    ServiceUser::getActiveRequestStatuses() const
    {
        std::vector<RequestLifecycleStatus> statuses;
        for (const auto& item : m_pendingCalls) {
            auto status = getRequestStatus(item.first);
            if (status) {
                statuses.push_back(*status);
            }
        }
        return statuses;
    }

    size_t ServiceUser::getPendingCallCount() const
    {
        return m_pendingCalls.size();
    }

    std::shared_ptr<StreamInvocationLifecycle>
    ServiceUser::attachStreamLifecycle(const ndn::Name& requestId,
                                       bool deferredCollaboration)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end()) {
            throw std::invalid_argument(
                "cannot attach streamed lifecycle to an unknown request");
        }

        auto& call = pending->second;
        if (deferredCollaboration &&
            !(call.isCollaboration && call.collaborationDeferred)) {
            throw std::invalid_argument(
                "deferred streamed lifecycle requires an existing deferred collaboration");
        }
        if (!call.streamLifecycle) {
            call.streamLifecycle =
                std::make_shared<StreamInvocationLifecycle>(deferredCollaboration);
        }
        else if (call.streamLifecycle->isDeferredCollaboration() !=
                 deferredCollaboration) {
            throw std::logic_error(
                "stream lifecycle attachment mode cannot change");
        }
        return call.streamLifecycle;
    }

    std::shared_ptr<StreamInvocationLifecycle>
    ServiceUser::getStreamLifecycle(const ndn::Name& requestId) const
    {
        const auto pending = m_pendingCalls.find(requestId);
        return pending == m_pendingCalls.end() ? nullptr :
                                                    pending->second.streamLifecycle;
    }

    ServiceUser::RuntimeDiagnostics ServiceUser::consumeRuntimeDiagnostics()
    {
        RuntimeDiagnostics diagnostics = std::move(m_runtimeDiagnostics);
        m_runtimeDiagnostics = RuntimeDiagnostics();
        return diagnostics;
    }

    SelectionExecutionStatus
    ServiceUser::parseSelectionExecutionStatusPayload(
        const ndn::Data& data,
        const ndn::Name& providerName,
        const std::string& selectionDigest)
    {
        SelectionExecutionStatus status;
        status.providerName = providerName;
        status.selectionDigest = selectionDigest;

        const auto content = data.getContent();
        const std::string text(reinterpret_cast<const char*>(content.value()),
                               content.value_size());
        std::istringstream input(text);
        std::string line;
        std::map<size_t, CollaborationMemberStatus> members;
        size_t declaredMemberCount = 0;
        while (std::getline(input, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const auto key = line.substr(0, eq);
            const auto value = line.substr(eq + 1);
            try {
                if (key == "member_count") {
                    declaredMemberCount = std::stoull(value);
                    if (declaredMemberCount > 64) {
                        throw std::invalid_argument("member status bound exceeded");
                    }
                    continue;
                }
                if (key == "decision_receipt_hex") {
                    status.decisionReceipt = decodeStatusHex(value);
                    continue;
                }
                if (key.rfind("member.", 0) == 0) {
                    const auto indexEnd = key.find('.', 7);
                    if (indexEnd == std::string::npos) continue;
                    const size_t index = std::stoull(key.substr(7, indexEnd - 7));
                    if (index >= 64) {
                        throw std::invalid_argument("member status index exceeded");
                    }
                    const auto field = key.substr(indexEnd + 1);
                    auto& member = members[index];
                    if (field == "provider" && !value.empty()) member.providerName = ndn::Name(value);
                    else if (field == "service" && !value.empty()) member.serviceName = ndn::Name(value);
                    else if (field == "request_id" && !value.empty()) member.requestId = ndn::Name(value);
                    else if (field == "selection_digest") member.selectionDigest = value;
                    else if (field == "role") member.role = value;
                    else if (field == "operation_id") member.operationId = value;
                    else if (field == "operation") member.operation = value;
                    else if (field == "state") member.state = value;
                    else if (field == "reason_code") member.reasonCode = value;
                    else if (field == "message") member.message = value;
                    else if (field == "attempt") member.attempt = std::stoull(value);
                    else if (field == "epoch") member.epoch = std::stoull(value);
                    else if (field == "sequence") member.sequence = std::stoull(value);
                    else if (field == "progress_known") member.progressKnown = value == "1";
                    else if (field == "progress") member.progress = std::stod(value);
                    else if (field == "created_at_ms") member.createdAtMs = std::stoull(value);
                    else if (field == "updated_at_ms") member.updatedAtMs = std::stoull(value);
                    else if (field == "expires_at_ms") member.expiresAtMs = std::stoull(value);
                    else if (field == "details_schema") member.detailsSchema = value;
                    else if (field == "details_hex") member.detailsPayload = decodeStatusHex(value);
                    continue;
                }
                if (key == "state") {
                    status.state = selectionExecutionStateFromString(value);
                }
                else if (key == "provider" && !value.empty()) {
                    status.providerName = ndn::Name(value);
                }
                else if (key == "service" && !value.empty()) {
                    status.serviceName = ndn::Name(value);
                }
                else if (key == "request_id" && !value.empty()) {
                    status.requestId = ndn::Name(value);
                }
                else if (key == "selection_digest") {
                    status.selectionDigest = value;
                }
                else if (key == "message") {
                    status.message = value;
                }
                else if (key == "response_name" && !value.empty()) {
                    status.responseName = ndn::Name(value);
                }
                else if (key == "received_at_us") {
                    status.receivedAtUs = std::stoull(value);
                }
                else if (key == "queued_at_us") {
                    status.queuedAtUs = std::stoull(value);
                }
                else if (key == "running_at_us") {
                    status.runningAtUs = std::stoull(value);
                }
                else if (key == "completed_at_us") {
                    status.completedAtUs = std::stoull(value);
                }
                else if (key == "updated_at_us") {
                    status.updatedAtUs = std::stoull(value);
                }
            }
            catch (const std::exception&) {
            }
        }
        if (declaredMemberCount == members.size()) {
            for (auto& item : members) {
                auto& member = item.second;
                if (member.providerName.equals(status.providerName) &&
                    member.serviceName.equals(status.serviceName) &&
                    member.requestId.equals(status.requestId) &&
                    member.selectionDigest == status.selectionDigest &&
                    !member.role.empty() && !member.operationId.empty() &&
                    !member.operation.empty() && member.attempt > 0 &&
                    member.epoch > 0 && member.sequence > 0 &&
                    member.progress >= 0.0 && member.progress <= 1.0 &&
                    member.detailsPayload.size() <= 4096) {
                    status.memberStatuses.push_back(std::move(member));
                }
            }
        }
        return status;
    }

    void ServiceUser::QuerySelectionStatus(
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const std::string& selectionDigest,
        SelectionStatusHandler onStatus,
        TimeoutHandler onTimeout,
        int timeoutMs)
    {
        const auto parameters = encodeSelectionStatusQueryParameters(serviceName, selectionDigest);
        ndn::Interest interest(makeControlRpcName(providerName, "SELECTION-STATUS",
                                                 parameters, makeControlQueryNonce()));
        interest.setApplicationParameters(parameters);
        const auto expectedDataName = interest.getName();
        interest.setCanBePrefix(false);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::milliseconds(
            std::max(1, timeoutMs)));

        const bool traceStatus = std::getenv("NDNSF_SELECTION_STATUS_TRACE") != nullptr;
        if (traceStatus) {
            NDN_LOG_INFO("NDNSF_SELECTION_STATUS_QUERY event=expressed"
                         << " provider=" << providerName.toUri()
                         << " service=" << serviceName.toUri()
                         << " selectionDigest=" << selectionDigest
                         << " timeoutMs=" << timeoutMs);
        }

        const auto timeoutHandler = std::move(onTimeout);
        m_face.expressInterest(
            interest,
            [this, providerName, serviceName, selectionDigest, expectedDataName,
             onStatus = std::move(onStatus), timeoutHandler, traceStatus](
                const ndn::Interest&, const ndn::Data& data) {
                if (traceStatus) {
                    NDN_LOG_INFO("NDNSF_SELECTION_STATUS_QUERY event=data-received"
                                 << " provider=" << providerName.toUri()
                                 << " service=" << serviceName.toUri()
                                 << " selectionDigest=" << selectionDigest
                                 << " name=" << data.getName().toUri());
                }
                if (data.getName() != expectedDataName) {
                    if (timeoutHandler) timeoutHandler(expectedDataName);
                    return;
                }
                validator->validate(
                    data,
                    [this, providerName, serviceName, selectionDigest, onStatus, timeoutHandler, traceStatus](
                        const ndn::Data& validatedData) {
                        if (!validator->isProducerSignerAuthorized(validatedData, providerName)) {
                            if (traceStatus) {
                                NDN_LOG_WARN("NDNSF_SELECTION_STATUS_QUERY event=signer-mismatch"
                                             << " provider=" << providerName.toUri()
                                             << " service=" << serviceName.toUri()
                                             << " selectionDigest=" << selectionDigest);
                            }
                            if (timeoutHandler) timeoutHandler(validatedData.getName());
                            return;
                        }
                        auto status = parseSelectionExecutionStatusPayload(
                            validatedData, providerName, selectionDigest);
                        if (!status.providerName.equals(providerName) ||
                            !status.serviceName.equals(serviceName) ||
                            status.selectionDigest != selectionDigest) {
                            if (traceStatus) {
                                NDN_LOG_WARN("NDNSF_SELECTION_STATUS_QUERY event=payload-mismatch"
                                             << " provider=" << providerName.toUri()
                                             << " service=" << serviceName.toUri()
                                             << " selectionDigest=" << selectionDigest
                                             << " payloadProvider=" << status.providerName.toUri()
                                             << " payloadService=" << status.serviceName.toUri()
                                             << " payloadDigest=" << status.selectionDigest);
                            }
                            if (timeoutHandler) timeoutHandler(validatedData.getName());
                            return;
                        }
                        if (traceStatus) {
                            NDN_LOG_INFO("NDNSF_SELECTION_STATUS_QUERY event=accepted"
                                         << " provider=" << providerName.toUri()
                                         << " service=" << serviceName.toUri()
                                         << " selectionDigest=" << selectionDigest
                                         << " state="
                                         << selectionExecutionStateToString(status.state)
                                         << " members=" << status.memberStatuses.size());
                        }
                        if (onStatus) onStatus(status);
                    },
                    [timeoutHandler, providerName, serviceName, selectionDigest, traceStatus](
                        const ndn::Data& badData,
                                     const ndn::security::ValidationError&) {
                        if (traceStatus) {
                            NDN_LOG_WARN("NDNSF_SELECTION_STATUS_QUERY event=validation-failed"
                                         << " provider=" << providerName.toUri()
                                         << " service=" << serviceName.toUri()
                                         << " selectionDigest=" << selectionDigest
                                         << " name=" << badData.getName().toUri());
                        }
                        if (timeoutHandler) timeoutHandler(badData.getName());
                    });
            },
            [timeoutHandler, providerName, serviceName, selectionDigest, traceStatus](
                const ndn::Interest& interest, const ndn::lp::Nack&) {
                if (traceStatus) {
                    NDN_LOG_WARN("NDNSF_SELECTION_STATUS_QUERY event=nack"
                                 << " provider=" << providerName.toUri()
                                 << " service=" << serviceName.toUri()
                                 << " selectionDigest=" << selectionDigest
                                 << " name=" << interest.getName().toUri());
                }
                if (timeoutHandler) {
                    timeoutHandler(interest.getName());
                }
            },
            [timeoutHandler, providerName, serviceName, selectionDigest, traceStatus](
                const ndn::Interest& interest) {
                if (traceStatus) {
                    NDN_LOG_WARN("NDNSF_SELECTION_STATUS_QUERY event=timeout"
                                 << " provider=" << providerName.toUri()
                                 << " service=" << serviceName.toUri()
                                 << " selectionDigest=" << selectionDigest
                                 << " name=" << interest.getName().toUri());
                }
                if (timeoutHandler) {
                    timeoutHandler(interest.getName());
                }
            });
    }

    std::vector<SelectionExecutionStatus>
    ServiceUser::GetCollaborationStatusSnapshot(const ndn::Name& requestId) const
    {
        std::vector<SelectionExecutionStatus> output;
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end() || !pending->second.isCollaboration) {
            return output;
        }
        const auto& call = pending->second;
        output.reserve(call.selectionStatusesByProvider.size() +
                       call.collaborationCommittedParticipants.size());
        for (const auto& item : call.selectionStatusesByProvider) {
            output.push_back(item.second);
        }
        const auto addUnknown = [&output, &call, &requestId](const ndn::Name& provider) {
            const auto uri = provider.toUri();
            if (call.selectionStatusesByProvider.count(uri) != 0 ||
                std::any_of(output.begin(), output.end(),
                            [&provider](const SelectionExecutionStatus& status) {
                                return status.providerName.equals(provider);
                            })) {
                return;
            }
            SelectionExecutionStatus unknown;
            unknown.providerName = provider;
            unknown.serviceName = call.serviceName;
            unknown.requestId = requestId;
            const auto digest = call.selectionDigestsByProvider.find(uri);
            if (digest != call.selectionDigestsByProvider.end()) {
                unknown.selectionDigest = digest->second;
            }
            output.push_back(std::move(unknown));
        };
        for (const auto& participant : call.collaborationCommittedParticipants) {
            addUnknown(participant.provider);
        }
        for (const auto& provider : call.expectedResponseProviders) {
            addUnknown(provider);
        }
        for (const auto& item : call.selectionDigestsByProvider) {
            addUnknown(ndn::Name(item.first));
        }
        std::sort(output.begin(), output.end(),
                  [](const SelectionExecutionStatus& left,
                     const SelectionExecutionStatus& right) {
                      return left.providerName.toUri() < right.providerName.toUri();
                  });
        return output;
    }

    void ServiceUser::scheduleSelectionStatusQuery(
        const ndn::Name& requestId,
        const ndn::Name& providerName,
        const std::string& selectionDigest)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end() ||
            !pending->second.trackSelectionStatus ||
            !pending->second.selectionStatusOptions.enabled ||
            pending->second.hasResponse ||
            pending->second.timedOut) {
            return;
        }

        const ndn::Name serviceName = pending->second.serviceName;
        const auto options = pending->second.selectionStatusOptions;
        const std::string providerUri = providerName.toUri();

        QuerySelectionStatus(
            providerName,
            serviceName,
            selectionDigest,
            [this, requestId, providerName, providerUri, serviceName, selectionDigest](
                const SelectionExecutionStatus& status) {
                auto call = m_pendingCalls.find(requestId);
                if (call == m_pendingCalls.end()) {
                    return;
                }
                // QuerySelectionStatus authenticates the Provider and binds
                // service/selectionDigest, but the status payload also carries
                // a request id.  Reject a cross-request snapshot before it can
                // affect stream liveness or the diagnostic map.
                if (!status.providerName.equals(providerName) ||
                    !status.serviceName.equals(serviceName) ||
                    !status.requestId.equals(requestId) ||
                    status.selectionDigest != selectionDigest) {
                    return;
                }
                const auto currentDigest =
                    call->second.selectionDigestsByProvider.find(providerUri);
                if (currentDigest == call->second.selectionDigestsByProvider.end() ||
                    currentDigest->second != selectionDigest) {
                    return;
                }
                call->second.selectionStatusesByProvider[providerUri] = status;
                auto consumer = m_streamConsumers.find(requestId);
                if (consumer != m_streamConsumers.end()) {
                    consumer->second->observeAuthenticatedProgress(status);
                }
            },
            [this, requestId, providerName, serviceName, providerUri, selectionDigest](
                const ndn::Name&) {
                auto call = m_pendingCalls.find(requestId);
                if (call == m_pendingCalls.end()) {
                    return;
                }
                auto& status =
                    call->second.selectionStatusesByProvider[providerUri];
                if (status.state == SelectionExecutionState::Unknown ||
                    status.selectionDigest.empty()) {
                    status.providerName = providerName;
                    status.serviceName = serviceName;
                    status.requestId = requestId;
                    status.selectionDigest = selectionDigest;
                    status.state = SelectionExecutionState::Unknown;
                }
                status.message = "selection status query timed out";
                status.updatedAtUs = nowMicroseconds();
            },
            options.queryTimeoutMs);

        m_scheduler.schedule(ndn::time::milliseconds(options.queryIntervalMs),
            [this, requestId, providerName, selectionDigest] {
                scheduleSelectionStatusQuery(requestId,
                                             providerName,
                                             selectionDigest);
            });
    }

    void ServiceUser::scheduleInitialSelectionStatusQuery(
        const ndn::Name& requestId,
        const ndn::Name& providerName,
        const std::string& selectionDigest)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end() ||
            !pending->second.trackSelectionStatus ||
            !pending->second.selectionStatusOptions.enabled ||
            pending->second.hasResponse || pending->second.timedOut) {
            return;
        }
        const auto intervalMs = std::max(
            1, pending->second.selectionStatusOptions.queryIntervalMs);
        // Selection is published before the Provider has necessarily
        // decrypted it and created the digest-bound status record.  Delay the
        // first query by the same bounded interval used by the retry loop so
        // the initial Interest cannot race status registration.  Subsequent
        // queries are scheduled by scheduleSelectionStatusQuery().
        m_scheduler.schedule(ndn::time::milliseconds(intervalMs),
            [this, requestId, providerName, selectionDigest] {
                scheduleSelectionStatusQuery(requestId,
                                             providerName,
                                             selectionDigest);
            });
    }

    void ServiceUser::querySelectionStatusForTimeoutDiagnostics(
        const ndn::Name& requestId,
        const PendingCall& pendingCall)
    {
        if (pendingCall.selectionDigestsByProvider.empty()) {
            NDN_LOG_INFO("[NDNSF_SELECTION_STATUS_TIMEOUT_DIAG] event=no_selection_query"
                         << " requestId=" << requestId.toUri()
                         << " serviceName=" << pendingCall.serviceName.toUri()
                         << " reason=no_selection_published");
            return;
        }

        const int queryTimeoutMs =
            std::max(1, pendingCall.selectionStatusOptions.queryTimeoutMs);
        for (const auto& item : pendingCall.selectionDigestsByProvider) {
            const ndn::Name providerName(item.first);
            const std::string selectionDigest = item.second;
            const ndn::Name serviceName = pendingCall.serviceName;
            NDN_LOG_INFO("[NDNSF_SELECTION_STATUS_TIMEOUT_DIAG] event=query_expressed"
                         << " requestId=" << requestId.toUri()
                         << " providerName=" << providerName.toUri()
                         << " serviceName=" << serviceName.toUri()
                         << " selectionDigest=" << selectionDigest
                         << " queryTimeoutMs=" << queryTimeoutMs);
            QuerySelectionStatus(
                providerName,
                serviceName,
                selectionDigest,
                [requestId] (const SelectionExecutionStatus& status) {
                    NDN_LOG_INFO("[NDNSF_SELECTION_STATUS_TIMEOUT_DIAG] event=query_result"
                                 << " requestId=" << requestId.toUri()
                                 << " providerName=" << status.providerName.toUri()
                                 << " serviceName=" << status.serviceName.toUri()
                                 << " selectionDigest=" << status.selectionDigest
                                 << " state="
                                 << selectionExecutionStateToString(status.state)
                                 << " message=\"" << status.message << "\""
                                 << " responseName=" << status.responseName.toUri());
                },
                [requestId, providerName, serviceName, selectionDigest](
                    const ndn::Name&) {
                    NDN_LOG_INFO("[NDNSF_SELECTION_STATUS_TIMEOUT_DIAG] event=query_no_reply"
                                 << " requestId=" << requestId.toUri()
                                 << " providerName=" << providerName.toUri()
                                 << " serviceName=" << serviceName.toUri()
                                 << " selectionDigest=" << selectionDigest
                                 << " reason=status_not_queryable_or_unreachable");
                },
                queryTimeoutMs);
        }
    }

    void ServiceUser::setAdaptiveAdmissionControl(const AdaptiveAdmissionOptions& options)
    {
        m_adaptiveAdmissionOptions = options;
        m_adaptiveAdmissionOptions.minWindow = std::max<size_t>(1, m_adaptiveAdmissionOptions.minWindow);
        m_adaptiveAdmissionOptions.maxWindow =
            std::max(m_adaptiveAdmissionOptions.minWindow, m_adaptiveAdmissionOptions.maxWindow);
        m_adaptiveAdmissionOptions.hardInflightLimit =
            std::max(m_adaptiveAdmissionOptions.minWindow,
                     m_adaptiveAdmissionOptions.hardInflightLimit);
        m_adaptiveAdmissionOptions.maxWindow =
            std::min(m_adaptiveAdmissionOptions.maxWindow,
                     m_adaptiveAdmissionOptions.hardInflightLimit);
        m_adaptiveAdmissionOptions.initialWindow =
            std::max(m_adaptiveAdmissionOptions.minWindow,
                     std::min(m_adaptiveAdmissionOptions.initialWindow,
                              m_adaptiveAdmissionOptions.maxWindow));
        m_adaptiveAdmissionOptions.aiStep = std::max<size_t>(1, m_adaptiveAdmissionOptions.aiStep);
        if (m_adaptiveAdmissionOptions.mdFactor <= 0.0 ||
            m_adaptiveAdmissionOptions.mdFactor >= 1.0) {
            m_adaptiveAdmissionOptions.mdFactor = 0.85;
        }
        if (m_adaptiveAdmissionOptions.severeMdFactor <= 0.0 ||
            m_adaptiveAdmissionOptions.severeMdFactor >= m_adaptiveAdmissionOptions.mdFactor) {
            m_adaptiveAdmissionOptions.severeMdFactor =
                std::min(0.5, m_adaptiveAdmissionOptions.mdFactor * 0.7);
        }
        m_adaptiveAdmissionOptions.controlIntervalMs =
            std::max(1, m_adaptiveAdmissionOptions.controlIntervalMs);
        m_adaptiveAdmissionOptions.targetLatencyMs =
            std::max(1, m_adaptiveAdmissionOptions.targetLatencyMs);
        m_adaptiveAdmissionOptions.hardTargetLatencyMs =
            std::max(m_adaptiveAdmissionOptions.targetLatencyMs,
                     m_adaptiveAdmissionOptions.hardTargetLatencyMs);
        if (m_adaptiveAdmissionOptions.minRecommendedRateRps <= 0.0) {
            m_adaptiveAdmissionOptions.minRecommendedRateRps = 1.0;
        }
        if (m_adaptiveAdmissionOptions.initialRecommendedRateRps <= 0.0) {
            const double initialLatencyBudgetMs =
                static_cast<double>(m_adaptiveAdmissionOptions.hardTargetLatencyMs);
            m_adaptiveAdmissionOptions.initialRecommendedRateRps = std::max(
                m_adaptiveAdmissionOptions.minRecommendedRateRps,
                1000.0 * static_cast<double>(m_adaptiveAdmissionOptions.initialWindow) /
                    std::max(1.0, initialLatencyBudgetMs));
        }
        if (m_adaptiveAdmissionOptions.maxRecommendedRateRps > 0.0 &&
            m_adaptiveAdmissionOptions.maxRecommendedRateRps <
                m_adaptiveAdmissionOptions.minRecommendedRateRps) {
            m_adaptiveAdmissionOptions.maxRecommendedRateRps =
                m_adaptiveAdmissionOptions.minRecommendedRateRps;
        }
        if (m_adaptiveAdmissionOptions.maxRecommendedRateRps > 0.0) {
            m_adaptiveAdmissionOptions.initialRecommendedRateRps = std::min(
                m_adaptiveAdmissionOptions.initialRecommendedRateRps,
                m_adaptiveAdmissionOptions.maxRecommendedRateRps);
        }
        if (m_adaptiveAdmissionOptions.softQueueLimit > 0 &&
            m_adaptiveAdmissionOptions.hardQueueLimit > 0 &&
            m_adaptiveAdmissionOptions.softQueueLimit >
                m_adaptiveAdmissionOptions.hardQueueLimit) {
            m_adaptiveAdmissionOptions.softQueueLimit =
                m_adaptiveAdmissionOptions.hardQueueLimit;
        }
        m_adaptiveAdmissionWindow = m_adaptiveAdmissionOptions.enabled ?
            m_adaptiveAdmissionOptions.initialWindow :
            m_adaptiveAdmissionOptions.maxWindow;
        m_adaptiveAdmissionSlowStartThreshold =
            m_adaptiveAdmissionOptions.maxWindow;
        m_adaptiveAdmissionBaselineLatencyMs = 0.0;
        m_adaptiveAdmissionPreviousQueueDelayMs = 0.0;
        m_adaptiveAdmissionPreviousAverageLatencyMs = 0.0;
        m_adaptiveAdmissionPreviousP95LatencyMs = 0.0;
        m_adaptiveAdmissionCompletionRateEmaRps = 0.0;
        m_adaptiveAdmissionRecommendedRateRps =
            m_adaptiveAdmissionOptions.rateRecommendationEnabled ?
            m_adaptiveAdmissionOptions.initialRecommendedRateRps : 0.0;
        m_adaptiveAdmissionLatencyRisingIntervals = 0;
        m_adaptiveAdmissionAverageLatencyRisingIntervals = 0;
        m_adaptiveAdmissionRecoveryIntervals = 0;
        m_adaptiveAdmissionSuccessfulControlIntervals = 0;
        m_adaptiveAdmissionQueueDelayOverTargetIntervals = 0;
        NDN_LOG_WARN("Adaptive admission control: "
                     << (m_adaptiveAdmissionOptions.enabled ? "enabled" : "disabled")
                     << " window=" << m_adaptiveAdmissionWindow
                     << " min=" << m_adaptiveAdmissionOptions.minWindow
                     << " max=" << m_adaptiveAdmissionOptions.maxWindow
                     << " hardInflight=" << m_adaptiveAdmissionOptions.hardInflightLimit
                     << " softQueueLimitCap=" << m_adaptiveAdmissionOptions.softQueueLimit
                     << " hardQueueLimitCap=" << m_adaptiveAdmissionOptions.hardQueueLimit
                     << " controlIntervalMs="
                     << m_adaptiveAdmissionOptions.controlIntervalMs
                     << " targetLatencyMs="
                     << m_adaptiveAdmissionOptions.targetLatencyMs
                     << " hardTargetLatencyMs="
                     << m_adaptiveAdmissionOptions.hardTargetLatencyMs);
        if (m_adaptiveAdmissionOptions.enabled) {
            scheduleAdaptiveAdmissionControl();
            drainAdaptiveAdmissionQueue();
        }
    }

    ServiceUser::AdaptiveAdmissionOptions ServiceUser::getAdaptiveAdmissionOptions() const
    {
        return m_adaptiveAdmissionOptions;
    }

    size_t ServiceUser::getAdaptiveAdmissionWindow() const
    {
        return m_adaptiveAdmissionWindow;
    }

    size_t ServiceUser::getAdaptiveAdmissionInflight() const
    {
        return m_adaptiveAdmissionInflight;
    }

    size_t ServiceUser::getAdaptiveAdmissionQueueDepth() const
    {
        return m_adaptiveAdmissionQueue.size();
    }

    double ServiceUser::getAdaptiveAdmissionRecommendedRateRps() const
    {
        return m_adaptiveAdmissionRecommendedRateRps;
    }

    void ServiceUser::recordAdaptiveAdmissionBackpressure()
    {
        if (m_adaptiveAdmissionOptions.enabled) {
            ++m_adaptiveAdmissionIntervalBackpressure;
        }
    }

    void ServiceUser::updateRequestLifecycleState(const ndn::Name& requestId,
                                                  RequestLifecycleState state,
                                                  const char* cleanupReason)
    {
        const auto nowUs = nowMicroseconds();
        auto& status = m_requestLifecycleStatuses[requestId];
        status.requestId = requestId;
        if (status.applicationTaskId.empty()) {
            status.applicationTaskId = requestId.toUri();
        }

        auto pendingIt = m_pendingCalls.find(requestId);
        if (pendingIt != m_pendingCalls.end()) {
            const auto& pending = pendingIt->second;
            status.serviceName = pending.serviceName;
            status.selectedProviderName = pending.selectedProvider;
            if (status.enqueueTimestampUs == 0) {
                status.enqueueTimestampUs = pending.createdAtUs;
            }
            if (status.publishTimestampUs == 0) {
                status.publishTimestampUs = pending.publishedAtUs;
            }
            if (status.ackMatchedTimestampUs == 0) {
                status.ackMatchedTimestampUs = pending.firstAckAtUs;
            }
            if (status.providerSelectionTimestampUs == 0) {
                status.providerSelectionTimestampUs = pending.ackSelectionCompletedAtUs;
            }
            if (status.selectionPublishTimestampUs == 0) {
                status.selectionPublishTimestampUs = pending.selectionPublishedAtUs;
            }
            if (status.responseObservedTimestampUs == 0) {
                status.responseObservedTimestampUs = pending.responseObservedAtUs;
            }
            if (status.responseDecryptedTimestampUs == 0) {
                status.responseDecryptedTimestampUs = pending.responseDecryptedAtUs;
            }
        }

        status.state = state;
        switch (state) {
        case RequestLifecycleState::QUEUED_LOCAL:
            if (status.enqueueTimestampUs == 0) {
                status.enqueueTimestampUs = nowUs;
            }
            break;
        case RequestLifecycleState::ADMISSION_DELAYED:
            status.delayedByAdmissionControl = true;
            break;
        case RequestLifecycleState::ADMITTED:
            if (status.admissionTimestampUs == 0) {
                status.admissionTimestampUs = nowUs;
            }
            break;
        case RequestLifecycleState::REQUEST_PUBLISHED:
            status.publishTimestampUs = status.publishTimestampUs == 0 ? nowUs : status.publishTimestampUs;
            break;
        case RequestLifecycleState::ACK_MATCHED:
            status.ackMatchedTimestampUs = status.ackMatchedTimestampUs == 0 ? nowUs : status.ackMatchedTimestampUs;
            break;
        case RequestLifecycleState::PROVIDER_SELECTED:
            status.providerSelectionTimestampUs =
                status.providerSelectionTimestampUs == 0 ? nowUs : status.providerSelectionTimestampUs;
            break;
        case RequestLifecycleState::SELECTION_PUBLISHED:
            status.selectionPublishTimestampUs = nowUs;
            break;
        case RequestLifecycleState::RESPONSE_OBSERVED:
            status.responseObservedTimestampUs =
                status.responseObservedTimestampUs == 0 ? nowUs : status.responseObservedTimestampUs;
            break;
        case RequestLifecycleState::RESPONSE_DECRYPTED:
            status.responseDecryptedTimestampUs =
                status.responseDecryptedTimestampUs == 0 ? nowUs : status.responseDecryptedTimestampUs;
            break;
        case RequestLifecycleState::CALLBACK_FIRED:
            status.callbackTimestampUs = nowUs;
            break;
        case RequestLifecycleState::COMPLETED:
            status.completionTimestampUs = nowUs;
            break;
        case RequestLifecycleState::ADMISSION_REJECTED:
            status.completionTimestampUs = nowUs;
            break;
        case RequestLifecycleState::TIMED_OUT:
            status.timeoutTimestampUs = nowUs;
            break;
        case RequestLifecycleState::CANCELLED_OR_DROPPED:
            status.completionTimestampUs = nowUs;
            break;
        }
        if (cleanupReason != nullptr) {
            status.finalCleanupReason = cleanupReason;
        }
        if (status.enqueueTimestampUs != 0 && status.admissionTimestampUs != 0 &&
            status.admissionTimestampUs >= status.enqueueTimestampUs) {
            status.queuedDurationMs =
                static_cast<double>(status.admissionTimestampUs - status.enqueueTimestampUs) / 1000.0;
        }
        const auto terminalUs = status.completionTimestampUs != 0 ?
            status.completionTimestampUs : status.timeoutTimestampUs;
        if (status.publishTimestampUs != 0 && terminalUs != 0 &&
            terminalUs >= status.publishTimestampUs) {
            status.inflightDurationMs =
                static_cast<double>(terminalUs - status.publishTimestampUs) / 1000.0;
        }
        if (status.enqueueTimestampUs != 0 && terminalUs != 0 &&
            terminalUs >= status.enqueueTimestampUs) {
            status.endToEndLatencyMs =
                static_cast<double>(terminalUs - status.enqueueTimestampUs) / 1000.0;
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_LIFECYCLE_STATE timestamp_us="
                  << nowUs
                  << " requestId=" << requestId.toUri()
                  << " state=" << requestLifecycleStateToString(state)
                  << " serviceName=" << (status.serviceName.empty() ? "-" : status.serviceName.toUri())
                  << " selectedProvider="
                  << (status.selectedProviderName.empty() ? "-" : status.selectedProviderName.toUri())
                  << " delayedByAdmissionControl=" << status.delayedByAdmissionControl
                  << " cleanupReason="
                  << (status.finalCleanupReason.empty() ? "-" : status.finalCleanupReason));
        if (m_requestLifecycleCallback) {
            m_requestLifecycleCallback(status);
        }
        logControlTiming("user",
                         requestLifecycleStateToString(state),
                         requestId,
                         {{"serviceName", status.serviceName.empty() ? "-" : status.serviceName.toUri()},
                          {"selectedProvider", status.selectedProviderName.empty() ? "-" : status.selectedProviderName.toUri()},
                          {"queuedDurationMs", std::to_string(status.queuedDurationMs)},
                          {"inflightDurationMs", std::to_string(status.inflightDurationMs)},
                          {"endToEndLatencyMs", std::to_string(status.endToEndLatencyMs)},
                          {"cleanupReason", status.finalCleanupReason.empty() ? "-" : status.finalCleanupReason}});
    }

    void ServiceUser::setPendingCallTimeoutGrace(ndn::time::milliseconds grace)
    {
        m_pendingCallTimeoutGrace = std::max(ndn::time::milliseconds(0), grace);
    }

    void ServiceUser::setResponseRetryOptions(ResponseRetryOptions options)
    {
        options.attemptTimeoutMs = std::max(1, options.attemptTimeoutMs);
        options.maxAttempts = std::max<size_t>(1, options.maxAttempts);
        m_responseRetryOptions = options;
        NDN_LOG_INFO("NDNSF_RESPONSE_RETRY enabled=" << options.enabled
                     << " attemptTimeoutMs=" << options.attemptTimeoutMs
                     << " maxAttempts=" << options.maxAttempts);
    }

    ServiceUser::ResponseRetryOptions ServiceUser::getResponseRetryOptions() const
    {
        return m_responseRetryOptions;
    }

    void ServiceUser::setPerformanceMode(bool enabled)
    {
        m_performanceMode = enabled;
    }

    void ServiceUser::setUseTokens(bool enabled)
    {
        m_useTokens = enabled;
        NDN_LOG_WARN("UserToken/ProviderToken runtime mode: "
                     << (m_useTokens ? "enabled" : "disabled for controlled experiment"));
    }

    bool ServiceUser::getUseTokens() const
    {
        return m_useTokens;
    }

#if defined(HAVE_TESTS)
    void ServiceUser::setTestClockForUnitTests(std::function<uint64_t()> clock)
    {
        auto& holder = unitTestClock();
        std::lock_guard<std::mutex> lock(holder.mutex);
        holder.callback = std::move(clock);
    }
#endif

    void ServiceUser::setTimelineTrace(bool enabled)
    {
        m_timelineTrace = enabled;
        if (enabled) {
            setenv("NDNSF_TIMELINE_TRACE", "1", 1);
            setenv("NDNSF_PHASE_TIMING", "1", 1);
        }
    }

    HybridCryptoCounters& ServiceUser::getHybridCryptoCounters()
    {
        return m_hybridCryptoCounters;
    }

    ServiceUser::AckCandidatesHandler
    ServiceUser::makeAckSelectionHandler(AckSelectionStrategy strategy)
    {
        switch (strategy) {
        case AckSelectionStrategy::FirstRespondingSelection:
            return [] (const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
                return selectFirstRespondingAck(candidates);
            };
        case AckSelectionStrategy::RandomSelection:
            return [] (const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
                return selectRandomAck(candidates);
            };
        case AckSelectionStrategy::AllSelected:
            return [] (const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
                return selectAllResponderAcks(candidates);
            };
        case AckSelectionStrategy::CustomSelectionStrategy:
            return nullptr;
        }
        return nullptr;
    }

    std::vector<ndn_service_framework::AckSelectionCandidate>
    ServiceUser::selectFirstRespondingAck(
        const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates)
    {
        for (const auto& candidate : candidates) {
            if (candidate.ack.getStatus()) {
                return {candidate};
            }
        }
        return {};
    }

    std::vector<ndn_service_framework::AckSelectionCandidate>
    ServiceUser::selectRandomAck(
        const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates)
    {
        std::vector<ndn_service_framework::AckSelectionCandidate> validCandidates;
        for (const auto& candidate : candidates) {
            if (candidate.ack.getStatus()) {
                validCandidates.push_back(candidate);
            }
        }
        if (validCandidates.empty()) {
            return {};
        }

        static thread_local std::mt19937 generator(std::random_device{}());
        std::uniform_int_distribution<size_t> distribution(0, validCandidates.size() - 1);
        return {validCandidates[distribution(generator)]};
    }

    std::vector<ndn_service_framework::AckSelectionCandidate>
    ServiceUser::selectAllResponderAcks(
        const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates)
    {
        std::vector<ndn_service_framework::AckSelectionCandidate> selected;
        for (const auto& candidate : candidates) {
            if (candidate.ack.getStatus()) {
                selected.push_back(candidate);
            }
        }
        return selected;
    }

    void ServiceUser::cleanupPendingCallState(const ndn::Name& requestId)
    {
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=PENDING_CLEANUP timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " strategyState=" << (m_strategyMap.find(requestId) != m_strategyMap.end())
                  << " ackInfoState=" << (m_AckInfoMap.find(requestId) != m_AckInfoMap.end()));
        m_strategyMap.erase(requestId);
        m_AckInfoMap.erase(requestId);
        m_adaptiveAdmissionQueue.erase(
            std::remove(m_adaptiveAdmissionQueue.begin(),
                        m_adaptiveAdmissionQueue.end(),
                        requestId),
            m_adaptiveAdmissionQueue.end());
    }

    std::string ServiceUser::samplePendingCallKeys(size_t limit) const
    {
        std::ostringstream os;
        size_t count = 0;
        for (const auto& item : m_pendingCalls) {
            if (count > 0) {
                os << ",";
            }
            os << item.first.toUri();
            ++count;
            if (count >= limit) {
                break;
            }
        }
        if (m_pendingCalls.size() > limit) {
            os << ",...";
        }
        return os.str();
    }

    void ServiceUser::logRequestPendingCreated(const ndn::Name& requestId,
                                               const PendingCall& pendingCall)
    {
        PendingCallTraceRecord record;
        record.createdAtUs = pendingCall.createdAtUs;
        record.requestName = pendingCall.requestName;
        m_pendingCallTraceHistory[requestId] = record;
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_PENDING_CREATED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " callId=" << requestId.toUri()
                  << " requestName="
                  << (pendingCall.requestName.empty() ? "-" : pendingCall.requestName.toUri())
                  << " createdAtUs=" << pendingCall.createdAtUs
                  << " pendingCallsSize=" << m_pendingCalls.size()
                  << " threadId=" << currentThreadIdForTrace());
    }

    void ServiceUser::erasePendingCallWithTrace(
        const ndn::Name& requestId,
        std::map<ndn::Name, PendingCall>::iterator pendingCall,
        const char* reason)
    {
        if (pendingCall == m_pendingCalls.end()) {
            return;
        }
        const auto eraseAtUs = nowMicroseconds();
        auto& record = m_pendingCallTraceHistory[requestId];
        if (record.createdAtUs == 0) {
            record.createdAtUs = pendingCall->second.createdAtUs;
        }
        record.erasedAtUs = eraseAtUs;
        record.timedOut = pendingCall->second.timedOut ||
                          std::string(reason) == "timeout";
        record.completed = pendingCall->second.hasResponse ||
                           std::string(reason) == "completed";
        record.requestName = pendingCall->second.requestName;
        if (record.completed || std::string(reason) == "response_callback") {
            updateRequestLifecycleState(requestId, RequestLifecycleState::COMPLETED, reason);
        }
        else if (std::string(reason) == "admission_queue_full") {
            updateRequestLifecycleState(requestId, RequestLifecycleState::ADMISSION_REJECTED, reason);
        }
        else if (record.timedOut) {
            updateRequestLifecycleState(requestId, RequestLifecycleState::TIMED_OUT, reason);
        }
        else {
            updateRequestLifecycleState(requestId, RequestLifecycleState::CANCELLED_OR_DROPPED, reason);
        }

        const char* event = record.completed ?
            "REQUEST_PENDING_COMPLETED" :
            (record.timedOut ? "REQUEST_PENDING_TIMEOUT" : "REQUEST_PENDING_ERASED");
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=" << event
                  << " timestamp_us=" << eraseAtUs
                  << " requestId=" << requestId.toUri()
                  << " callId=" << requestId.toUri()
                  << " requestName="
                  << (pendingCall->second.requestName.empty() ?
                      "-" : pendingCall->second.requestName.toUri())
                  << " createdAtUs=" << pendingCall->second.createdAtUs
                  << " erasedAtUs=" << eraseAtUs
                  << " reason=" << reason
                  << " hasMatchedAck=" << record.matchedAck
                  << " pendingCallsSizeBefore=" << m_pendingCalls.size()
                  << " threadId=" << currentThreadIdForTrace());
        if (std::string(event) != "REQUEST_PENDING_ERASED") {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_PENDING_ERASED"
                      << " timestamp_us=" << eraseAtUs
                      << " requestId=" << requestId.toUri()
                      << " callId=" << requestId.toUri()
                      << " reason=" << reason
                      << " pendingCallsSizeBefore=" << m_pendingCalls.size()
                      << " threadId=" << currentThreadIdForTrace());
        }
        releaseAdaptiveAdmissionSlot(requestId, pendingCall->second, reason, eraseAtUs);
        pendingCall->second.requestTimeoutEvent.cancel();
        pendingCall->second.responseAttemptTimeoutEvent.cancel();
        if (pendingCall->second.requestScopedKeys) {
            const auto keyId = pendingCall->second.requestScopedKeys->keyId;
            pendingCall->second.requestScopedKeys->zeroize();
            m_requestScopedNonceRegistry.invalidate(keyId);
        }
        for (auto& [provider, state] :
             pendingCall->second.requestScopedProviderStates) {
            if (!state.keys.keyId.empty()) {
                const auto keyId = state.keys.keyId;
                state.keys.zeroize();
                m_requestScopedNonceRegistry.invalidate(keyId);
            }
        }
        m_pendingCalls.erase(pendingCall);
        cleanupPendingCallState(requestId);
    }

    bool ServiceUser::hasReachedLatePipelineStage(const PendingCall& pendingCall) const
    {
        return pendingCall.firstAckAtUs != 0 ||
               !pendingCall.requestAcks.empty() ||
               pendingCall.providerSelected ||
               !pendingCall.selectedProvider.empty() ||
               pendingCall.selectionScheduledAtUs != 0 ||
               pendingCall.selectionPublishedAtUs != 0 ||
               pendingCall.responseObservedAtUs != 0 ||
               pendingCall.responseDecryptedAtUs != 0 ||
               pendingCall.responseValidatedAtUs != 0 ||
               pendingCall.hasResponse;
    }

    void ServiceUser::finalizeTimedOutPendingCall(const ndn::Name& requestId)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return;
        }
        if (pendingCall->second.hasResponse) {
            erasePendingCallWithTrace(requestId, pendingCall, "timeout_after_response");
            return;
        }

        pendingCall->second.timedOut = true;
        querySelectionStatusForTimeoutDiagnostics(requestId, pendingCall->second);
        auto timeoutHandler = pendingCall->second.timeoutHandler;
        auto statusTimeoutHandler = pendingCall->second.statusTimeoutHandler;
        std::vector<SelectionExecutionStatus> selectionStatuses;
        if (pendingCall->second.trackSelectionStatus) {
            for (const auto& item : pendingCall->second.selectionStatusesByProvider) {
                selectionStatuses.push_back(item.second);
            }
            for (const auto& item : pendingCall->second.selectionDigestsByProvider) {
                const auto& providerUri = item.first;
                const bool alreadyKnown =
                    pendingCall->second.selectionStatusesByProvider.find(providerUri) !=
                    pendingCall->second.selectionStatusesByProvider.end();
                if (alreadyKnown) {
                    continue;
                }
                SelectionExecutionStatus unknown;
                unknown.providerName = ndn::Name(providerUri);
                unknown.serviceName = pendingCall->second.serviceName;
                unknown.requestId = requestId;
                unknown.selectionDigest = item.second;
                unknown.state = SelectionExecutionState::Unknown;
                unknown.message = "no status response received before timeout";
                unknown.updatedAtUs = nowMicroseconds();
                selectionStatuses.push_back(std::move(unknown));
            }
        }
        erasePendingCallWithTrace(requestId, pendingCall, "timeout");

        if (statusTimeoutHandler) {
            statusTimeoutHandler(requestId, selectionStatuses);
        }
        else if (timeoutHandler) {
            timeoutHandler(requestId);
        }
    }

    void ServiceUser::scheduleRequestTimeout(const ndn::Name& requestId, int timeoutMs)
    {
        if (timeoutMs <= 0) {
            return;
        }

        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return;
        }

        auto delay = ndn::time::microseconds(static_cast<int64_t>(timeoutMs) * 1000);
        if (pendingCall->second.requestDeadlineUs != 0) {
            const auto nowUs = nowMicroseconds();
            if (nowUs >= pendingCall->second.requestDeadlineUs) {
                delay = ndn::time::microseconds(0);
            }
            else {
                delay = ndn::time::microseconds(
                    pendingCall->second.requestDeadlineUs - nowUs);
            }
        }

        pendingCall->second.requestTimeoutEvent =
          m_scheduler.schedule(delay, [this, requestId]() {
            auto pendingCall = m_pendingCalls.find(requestId);
            if (pendingCall == m_pendingCalls.end()) {
                return;
            }
            if (pendingCall->second.hasResponse) {
                erasePendingCallWithTrace(requestId, pendingCall, "timeout_after_response");
                return;
            }

            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=TIMEOUT_FIRED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " pendingCall=present"
                      << " ackCount=" << pendingCall->second.requestAcks.size()
                      << " selectedProvider="
                      << (pendingCall->second.selectedProvider.empty() ?
                          "-" : pendingCall->second.selectedProvider.toUri())
                      << " providerTokenCount="
                      << pendingCall->second.providerTokens.size()
                      << " ackSelectionAtUs="
                      << pendingCall->second.ackSelectionAtUs
                      << " ackSelectionCompletedAtUs="
                      << pendingCall->second.ackSelectionCompletedAtUs
                      << " selectionScheduledAtUs="
                      << pendingCall->second.selectionScheduledAtUs
                      << " selectionPublishedAtUs="
                      << pendingCall->second.selectionPublishedAtUs
                      << " responseObservedAtUs="
                      << pendingCall->second.responseObservedAtUs
                      << " responseDecryptedAtUs="
                      << pendingCall->second.responseDecryptedAtUs
                      << " responseValidatedAtUs="
                      << pendingCall->second.responseValidatedAtUs
                      << " createdAtUs="
                      << pendingCall->second.createdAtUs
                      << " publishedAtUs="
                      << pendingCall->second.publishedAtUs);

            if (pendingCall->second.requestDeadlineUs == 0 &&
                hasReachedLatePipelineStage(pendingCall->second) &&
                m_pendingCallTimeoutGrace.count() > 0) {
                pendingCall->second.timeoutGraceActive = true;
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=TIMEOUT_GRACE_STARTED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << requestId.toUri()
                          << " graceMs=" << m_pendingCallTimeoutGrace.count());
                m_scheduler.schedule(m_pendingCallTimeoutGrace, [this, requestId]() {
                    finalizeTimedOutPendingCall(requestId);
                });
                return;
            }

            NDN_LOG_INFO("[ServiceUser] user timeout timestampMs="
                      << nowMilliseconds()
                      << " requestId=" << requestId.toUri());
            finalizeTimedOutPendingCall(requestId);
          });
    }

    void ServiceUser::scheduleResponseAttemptTimeout(
        const ndn::Name& requestId,
        const ndn::Name& providerName)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end() ||
            !pending->second.responseRetryEnabled ||
            pending->second.hasResponse ||
            pending->second.timedOut ||
            providerName.empty() ||
            containsName(pending->second.responseAttemptProviders, providerName)) {
            return;
        }

        addUniqueName(pending->second.responseAttemptProviders, providerName);
        pending->second.responseAttemptStartedAtUs = nowMicroseconds();
        pending->second.expectedResponseProviders.clear();
        addUniqueName(pending->second.expectedResponseProviders, providerName);

        const size_t attempt = pending->second.responseAttemptProviders.size();
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_ATTEMPT_STARTED timestamp_us="
                  << pending->second.responseAttemptStartedAtUs
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " attempt=" << attempt
                  << " maxAttempts=" << pending->second.responseMaxAttempts
                  << " attemptTimeoutMs="
                  << pending->second.responseAttemptTimeoutMs);

        if (attempt >= pending->second.responseMaxAttempts) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_RETRY_EXHAUSTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " reason=max_attempts_selected"
                      << " attempts=" << attempt);
            return;
        }

        const uint64_t deadlineUs = pending->second.requestDeadlineUs != 0 ?
            pending->second.requestDeadlineUs :
            (pending->second.publishedAtUs != 0 && pending->second.timeoutMs > 0 ?
                pending->second.publishedAtUs +
                    static_cast<uint64_t>(pending->second.timeoutMs) * 1000 : 0);
        const uint64_t nowUs = nowMicroseconds();
        const uint64_t attemptUs =
            static_cast<uint64_t>(std::max(1, pending->second.responseAttemptTimeoutMs)) * 1000;
        if (deadlineUs != 0 && (nowUs >= deadlineUs || deadlineUs - nowUs <= attemptUs)) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_RETRY_EXHAUSTED timestamp_us="
                      << nowUs
                      << " requestId=" << requestId.toUri()
                      << " reason=insufficient_global_deadline"
                      << " attempts=" << attempt);
            return;
        }

        pending->second.responseAttemptTimeoutEvent.cancel();
        pending->second.responseRetryTimerArmed = true;
        pending->second.responseAttemptTimeoutEvent = m_scheduler.schedule(
            ndn::time::milliseconds(pending->second.responseAttemptTimeoutMs),
            [this, requestId, providerName]() {
                auto call = m_pendingCalls.find(requestId);
                if (call == m_pendingCalls.end()) {
                    return;
                }
                call->second.responseRetryTimerArmed = false;
                if (call->second.hasResponse || call->second.timedOut ||
                    !call->second.selectedProvider.equals(providerName)) {
                    return;
                }
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_ATTEMPT_TIMEOUT timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << requestId.toUri()
                          << " providerName=" << providerName.toUri()
                          << " attempt="
                          << call->second.responseAttemptProviders.size());
                retryResponseWithNextProvider(requestId,
                                              "response_attempt_timeout");
            });
    }

    bool ServiceUser::retryResponseWithNextProvider(
        const ndn::Name& requestId,
        const char* trigger)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end() ||
            !pending->second.responseRetryEnabled ||
            pending->second.hasResponse ||
            pending->second.timedOut ||
            pending->second.responseAttemptProviders.size() >=
                pending->second.responseMaxAttempts) {
            return false;
        }

        const StoredAck* nextAck = nullptr;
        for (const auto& ack : pending->second.requestAcks) {
            if (!ack.message.getStatus() ||
                containsName(pending->second.responseAttemptProviders,
                             ack.providerName)) {
                continue;
            }
            if (m_useTokens &&
                pending->second.providerTokens.find(ack.providerName.toUri()) ==
                    pending->second.providerTokens.end()) {
                continue;
            }
            nextAck = &ack;
            break;
        }

        if (nextAck == nullptr) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_RETRY_WAITING_NO_CANDIDATE timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " trigger=" << trigger
                      << " attempts="
                      << pending->second.responseAttemptProviders.size());
            return false;
        }

        const ndn::Name providerName = nextAck->providerName;
        const ndn::Name serviceName = nextAck->serviceName;
        pending->second.selectedProvider = providerName;
        pending->second.providerSelected = true;
        pending->second.selectionScheduledAtUs = nowMicroseconds();
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_RESELECTION timestamp_us="
                  << pending->second.selectionScheduledAtUs
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " trigger=" << trigger
                  << " nextAttempt="
                  << (pending->second.responseAttemptProviders.size() + 1));
        PublishServiceSelectionMessageV2(providerName, serviceName, requestId);
        return true;
    }

    void ServiceUser::admitOrQueuePendingCall(const ndn::Name& requestId,
                                              bool scheduleAckTimeout,
                                              bool scheduleImmediateAckTimeout)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return;
        }
        pendingCall->second.scheduleAckTimeoutAfterPublish = scheduleAckTimeout;
        pendingCall->second.scheduleImmediateAckTimeoutAfterPublish = scheduleImmediateAckTimeout;

        if (!m_adaptiveAdmissionOptions.enabled) {
            publishAdmittedPendingCall(requestId);
            return;
        }

        const size_t activeLimit = getEffectiveAdaptiveAdmissionWindow();
        if (m_adaptiveAdmissionInflight >= activeLimit) {
            const auto queueLimits =
                getEffectiveAdaptiveAdmissionQueueLimits(m_adaptiveAdmissionWindow);
            const size_t effectiveSoftQueueLimit = queueLimits.first;
            const size_t effectiveHardQueueLimit = queueLimits.second;
            if (m_adaptiveAdmissionQueue.size() >= effectiveHardQueueLimit) {
                rejectPendingCallByAdmission(requestId,
                                             "admission_queue_full",
                                             effectiveSoftQueueLimit,
                                             effectiveHardQueueLimit);
                return;
            }
            updateRequestLifecycleState(requestId, RequestLifecycleState::ADMISSION_DELAYED);
            m_adaptiveAdmissionQueue.push_back(requestId);
            if (m_adaptiveAdmissionQueue.size() >= effectiveSoftQueueLimit) {
                notifyAdmissionControlWarning(requestId,
                                              m_adaptiveAdmissionQueue.size(),
                                              "admission_queue_soft_limit",
                                              effectiveSoftQueueLimit,
                                              effectiveHardQueueLimit);
            }
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ADMISSION_QUEUED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " inflight=" << m_adaptiveAdmissionInflight
                      << " window=" << m_adaptiveAdmissionWindow
                      << " effectiveSoftQueueLimit=" << effectiveSoftQueueLimit
                      << " effectiveHardQueueLimit=" << effectiveHardQueueLimit
                      << " hardInflight="
                      << m_adaptiveAdmissionOptions.hardInflightLimit
                      << " queueDepth=" << m_adaptiveAdmissionQueue.size());
            return;
        }

        publishAdmittedPendingCall(requestId);
    }

    std::pair<size_t, size_t>
    ServiceUser::getEffectiveAdaptiveAdmissionQueueLimits(size_t activeLimit) const
    {
        const size_t active = std::max<size_t>(1, activeLimit);
        const size_t scaledHard = active > (std::numeric_limits<size_t>::max() - 16) / 2 ?
            std::numeric_limits<size_t>::max() : 16 + active * 2;
        size_t dynamicHard = std::min<size_t>(256, std::max<size_t>(32, scaledHard));
        if (m_adaptiveAdmissionOptions.hardQueueLimit > 0) {
            dynamicHard = std::min(dynamicHard,
                                   m_adaptiveAdmissionOptions.hardQueueLimit);
        }
        dynamicHard = std::max<size_t>(1, dynamicHard);

        size_t dynamicSoft = std::max<size_t>(
            1, static_cast<size_t>(std::ceil(static_cast<double>(dynamicHard) * 0.5)));
        if (m_adaptiveAdmissionOptions.softQueueLimit > 0) {
            dynamicSoft = std::min(dynamicSoft,
                                   m_adaptiveAdmissionOptions.softQueueLimit);
        }
        return {std::min(dynamicSoft, dynamicHard), dynamicHard};
    }

    ServiceUser::AdmissionControlStatus
    ServiceUser::makeAdmissionControlStatus(const ndn::Name& requestId,
                                            size_t queueDepth,
                                            const char* reason,
                                            size_t softQueueLimit,
                                            size_t hardQueueLimit) const
    {
        AdmissionControlStatus status;
        status.requestId = requestId;
        status.queueDepth = queueDepth;
        if (softQueueLimit == 0 || hardQueueLimit == 0) {
            const auto effectiveLimits = getEffectiveAdaptiveAdmissionQueueLimits(
                m_adaptiveAdmissionWindow);
            if (softQueueLimit == 0) {
                softQueueLimit = effectiveLimits.first;
            }
            if (hardQueueLimit == 0) {
                hardQueueLimit = effectiveLimits.second;
            }
        }
        status.softQueueLimit = softQueueLimit;
        status.hardQueueLimit = hardQueueLimit;
        status.remainingHardSlots =
            queueDepth >= status.hardQueueLimit ? 0 : status.hardQueueLimit - queueDepth;
        status.reason = reason;
        return status;
    }

    void ServiceUser::notifyAdmissionControlWarning(const ndn::Name& requestId,
                                                    size_t queueDepth,
                                                    const char* reason,
                                                    size_t softQueueLimit,
                                                    size_t hardQueueLimit)
    {
        if (softQueueLimit == 0 || hardQueueLimit == 0) {
            const auto effectiveLimits = getEffectiveAdaptiveAdmissionQueueLimits(
                m_adaptiveAdmissionWindow);
            if (softQueueLimit == 0) {
                softQueueLimit = effectiveLimits.first;
            }
            if (hardQueueLimit == 0) {
                hardQueueLimit = effectiveLimits.second;
            }
        }
        const size_t effectiveSoftQueueLimit = softQueueLimit;
        const size_t effectiveHardQueueLimit = hardQueueLimit;
        if (queueDepth < effectiveSoftQueueLimit) {
            return;
        }

        auto status = makeAdmissionControlStatus(requestId, queueDepth, reason,
                                                effectiveSoftQueueLimit,
                                                effectiveHardQueueLimit);
        ++m_adaptiveAdmissionIntervalQueueWarnings;
        ++m_adaptiveAdmissionIntervalBackpressure;
        if (queueDepth >= std::max<size_t>(
                effectiveSoftQueueLimit,
                static_cast<size_t>(std::ceil(
                    static_cast<double>(effectiveHardQueueLimit) * 0.75)))) {
            m_adaptiveAdmissionIntervalSevere = true;
        }
        NDN_LOG_WARN("[NDNSF_ADMISSION_WARNING] requestId=" << requestId.toUri()
                     << " depth=" << queueDepth
                     << " softLimit=" << status.softQueueLimit
                     << " hardLimit=" << status.hardQueueLimit
                     << " remainingHard=" << status.remainingHardSlots
                     << " reason=" << reason);

        if (m_admissionControlWarningHandler) {
            m_admissionControlWarningHandler(status);
        }
    }

    void ServiceUser::rejectPendingCallByAdmission(const ndn::Name& requestId,
                                                   const char* reason,
                                                   size_t softQueueLimit,
                                                   size_t hardQueueLimit)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return;
        }

        auto status = makeAdmissionControlStatus(requestId,
                                                m_adaptiveAdmissionQueue.size(),
                                                reason,
                                                softQueueLimit,
                                                hardQueueLimit);
        NDN_LOG_WARN("[NDNSF_ADMISSION_REJECT] requestId=" << requestId.toUri()
                     << " depth=" << status.queueDepth
                     << " softLimit=" << status.softQueueLimit
                     << " hardLimit=" << status.hardQueueLimit
                     << " remainingHard=" << status.remainingHardSlots
                     << " reason=" << reason);
        if (m_admissionControlRejectHandler) {
            m_admissionControlRejectHandler(status);
        }
        updateRequestLifecycleState(requestId,
                                    RequestLifecycleState::ADMISSION_REJECTED,
                                    reason);
        NDN_LOG_WARN("[NDNSF_TRACE] role=user event=ADMISSION_REJECTED timestamp_us="
                     << nowMicroseconds()
                     << " requestId=" << requestId.toUri()
                     << " queueDepth=" << m_adaptiveAdmissionQueue.size()
                     << " hardQueueLimit=" << status.hardQueueLimit
                     << " reason=" << reason);
        erasePendingCallWithTrace(requestId, pendingCall, reason);
    }

    void ServiceUser::publishAdmittedPendingCall(const ndn::Name& requestId)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end() || pendingCall->second.admissionPublished) {
            return;
        }

        pendingCall->second.admissionPublished = true;
        if (m_adaptiveAdmissionOptions.enabled) {
            ++m_adaptiveAdmissionInflight;
            scheduleAdaptiveAdmissionControl();
        }
        updateRequestLifecycleState(requestId, RequestLifecycleState::ADMITTED);

        const auto providers = pendingCall->second.providers;
        const auto serviceName = pendingCall->second.serviceName;
        const auto payload = pendingCall->second.requestMessage.getPayload();
        const auto strategy = pendingCall->second.strategy;
        const auto timeoutMs = pendingCall->second.timeoutMs;
        const auto ackTimeoutMs = pendingCall->second.ackTimeoutMs;
        const bool scheduleAckTimeout = pendingCall->second.scheduleAckTimeoutAfterPublish;
        const bool scheduleImmediateAckTimeout =
            pendingCall->second.scheduleImmediateAckTimeoutAfterPublish;
        pendingCall->second.learnedAckProviderCountAtPublish =
            getRecentAckProviderCount(serviceName, nowMicroseconds());

        PublishRequestV2(providers, serviceName, requestId, payload, strategy);

        pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return;
        }
        if (scheduleAckTimeout && !pendingCall->second.ackTimeoutScheduled) {
            pendingCall->second.ackTimeoutScheduled = true;
            if (ackTimeoutMs > 0) {
                pendingCall->second.ackWindowDeadlineUs =
                    pendingCall->second.publishedAtUs +
                    static_cast<uint64_t>(ackTimeoutMs) * 1000;
                m_scheduler.schedule(ndn::time::milliseconds(ackTimeoutMs), [this, requestId]() {
                    handleAckCollectionTimeout(requestId);
                });
            }
            else if (scheduleImmediateAckTimeout) {
                m_scheduler.schedule(ndn::time::milliseconds(0), [this, requestId]() {
                    handleAckCollectionTimeout(requestId);
                });
            }
        }
        if (!pendingCall->second.requestTimeoutScheduled) {
            pendingCall->second.requestTimeoutScheduled = true;
            scheduleRequestTimeout(requestId, timeoutMs);
        }
    }

    size_t ServiceUser::getEffectiveAdaptiveAdmissionWindow() const
    {
        size_t activeLimit = std::max<size_t>(
            1,
            std::min(m_adaptiveAdmissionWindow,
                     m_adaptiveAdmissionOptions.hardInflightLimit));
        if (m_adaptiveAdmissionSuccessfulControlIntervals < 3) {
            const size_t startupProbeLimit = std::min(
                m_adaptiveAdmissionOptions.maxWindow,
                std::max(activeLimit, m_adaptiveAdmissionOptions.initialWindow * 3));
            activeLimit = std::min(startupProbeLimit,
                                   m_adaptiveAdmissionOptions.hardInflightLimit);
        }
        return activeLimit;
    }

    void ServiceUser::drainAdaptiveAdmissionQueue()
    {
        if (!m_adaptiveAdmissionOptions.enabled) {
            return;
        }

        const size_t activeLimit = getEffectiveAdaptiveAdmissionWindow();
        while (m_adaptiveAdmissionInflight < activeLimit &&
               !m_adaptiveAdmissionQueue.empty()) {
            const ndn::Name requestId = m_adaptiveAdmissionQueue.front();
            m_adaptiveAdmissionQueue.pop_front();
            auto pendingCall = m_pendingCalls.find(requestId);
            if (pendingCall == m_pendingCalls.end() ||
                pendingCall->second.admissionPublished) {
                continue;
            }
            publishAdmittedPendingCall(requestId);
        }
    }

    void ServiceUser::scheduleAdaptiveAdmissionControl()
    {
        if (!m_adaptiveAdmissionOptions.enabled ||
            m_adaptiveAdmissionControlScheduled) {
            return;
        }
        m_adaptiveAdmissionControlScheduled = true;
        m_scheduler.schedule(
            ndn::time::milliseconds(m_adaptiveAdmissionOptions.controlIntervalMs),
            [this]() {
                m_adaptiveAdmissionControlScheduled = false;
                controlAdaptiveAdmissionWindow();
            });
    }

    void ServiceUser::controlAdaptiveAdmissionWindow()
    {
        if (!m_adaptiveAdmissionOptions.enabled) {
            return;
        }

        const size_t oldWindow = m_adaptiveAdmissionWindow;
        const size_t activeLimit = getEffectiveAdaptiveAdmissionWindow();
        const bool queueBacklogged = !m_adaptiveAdmissionQueue.empty();
        const bool aboveWindow = m_adaptiveAdmissionInflight > activeLimit;
        const double averageLatencyMs =
            m_adaptiveAdmissionIntervalLatencyCount == 0 ? 0.0 :
            m_adaptiveAdmissionIntervalLatencySumMs /
                static_cast<double>(m_adaptiveAdmissionIntervalLatencyCount);
        const double targetLatencyMs =
            static_cast<double>(m_adaptiveAdmissionOptions.targetLatencyMs);
        const double hardTargetLatencyMs =
            static_cast<double>(m_adaptiveAdmissionOptions.hardTargetLatencyMs);
        const double p50LatencyMs =
            percentileLatency(m_adaptiveAdmissionIntervalLatenciesMs, 50.0);
        const double p95LatencyMs =
            percentileLatency(m_adaptiveAdmissionIntervalLatenciesMs, 95.0);
        const double maxLatencyMs =
            m_adaptiveAdmissionIntervalLatenciesMs.empty() ? 0.0 :
            *std::max_element(m_adaptiveAdmissionIntervalLatenciesMs.begin(),
                              m_adaptiveAdmissionIntervalLatenciesMs.end());
        if (p50LatencyMs > 0.0 &&
            (p50LatencyMs < hardTargetLatencyMs ||
             m_adaptiveAdmissionBaselineLatencyMs > 0.0)) {
            if (m_adaptiveAdmissionBaselineLatencyMs <= 0.0) {
                m_adaptiveAdmissionBaselineLatencyMs = p50LatencyMs;
            }
            else if (p50LatencyMs < m_adaptiveAdmissionBaselineLatencyMs) {
                m_adaptiveAdmissionBaselineLatencyMs =
                    0.80 * m_adaptiveAdmissionBaselineLatencyMs +
                    0.20 * p50LatencyMs;
            }
            else {
                m_adaptiveAdmissionBaselineLatencyMs =
                    0.98 * m_adaptiveAdmissionBaselineLatencyMs +
                    0.02 * p50LatencyMs;
            }
        }
        const double queueDelayMs =
            p95LatencyMs > 0.0 && m_adaptiveAdmissionBaselineLatencyMs > 0.0 ?
            std::max(0.0, p95LatencyMs - m_adaptiveAdmissionBaselineLatencyMs) :
            0.0;
        const double queueDelayGradientMs =
            queueDelayMs - m_adaptiveAdmissionPreviousQueueDelayMs;
        const double queueDelayTargetMs =
            m_adaptiveAdmissionBaselineLatencyMs > 0.0 ?
            std::max(50.0, targetLatencyMs - m_adaptiveAdmissionBaselineLatencyMs) :
            std::max(50.0, 0.35 * targetLatencyMs);
        const double queueDelaySevereMs =
            m_adaptiveAdmissionBaselineLatencyMs > 0.0 ?
            std::max(queueDelayTargetMs + 50.0,
                     hardTargetLatencyMs - m_adaptiveAdmissionBaselineLatencyMs) :
            std::max(queueDelayTargetMs + 50.0, 0.75 * hardTargetLatencyMs);
        const double intervalSeconds = std::max(
            0.001,
            static_cast<double>(m_adaptiveAdmissionOptions.controlIntervalMs) /
                1000.0);
        const double completionRateRps =
            static_cast<double>(m_adaptiveAdmissionIntervalSuccesses) /
            intervalSeconds;
        if (completionRateRps > 0.0) {
            if (m_adaptiveAdmissionCompletionRateEmaRps <= 0.0) {
                m_adaptiveAdmissionCompletionRateEmaRps = completionRateRps;
            }
            else {
                m_adaptiveAdmissionCompletionRateEmaRps =
                    0.70 * m_adaptiveAdmissionCompletionRateEmaRps +
                    0.30 * completionRateRps;
            }
        }
        if (m_adaptiveAdmissionIntervalSuccesses > 0) {
            ++m_adaptiveAdmissionSuccessfulControlIntervals;
        }
        const bool latencySamplesWarmed =
            m_adaptiveAdmissionSuccessfulControlIntervals >= 3;
        const bool latencyBaselineTrusted =
            latencySamplesWarmed && m_adaptiveAdmissionBaselineLatencyMs > 0.0;
        if (queueDelayMs > queueDelayTargetMs) {
            ++m_adaptiveAdmissionQueueDelayOverTargetIntervals;
        }
        else {
            m_adaptiveAdmissionQueueDelayOverTargetIntervals = 0;
        }
        const auto queueLimits =
            getEffectiveAdaptiveAdmissionQueueLimits(m_adaptiveAdmissionWindow);
        const size_t effectiveSoftQueueLimit = queueLimits.first;
        const size_t effectiveHardQueueLimit = queueLimits.second;
        const bool queuePastSoftLimit =
            m_adaptiveAdmissionQueue.size() >= effectiveSoftQueueLimit;
        const bool queuePressure =
            (queueBacklogged &&
            m_adaptiveAdmissionInflight >=
                static_cast<size_t>(std::ceil(static_cast<double>(activeLimit) * 0.8)));
        const bool queueSevere =
            m_adaptiveAdmissionQueue.size() >= std::max<size_t>(
                effectiveSoftQueueLimit,
                static_cast<size_t>(std::ceil(
                    static_cast<double>(effectiveHardQueueLimit) * 0.75)));
        const bool demandBacklogged =
            queueBacklogged || m_adaptiveAdmissionIntervalBackpressure > 0;
        const double averageLatencyGradientMs =
            averageLatencyMs > 0.0 &&
            m_adaptiveAdmissionPreviousAverageLatencyMs > 0.0 ?
            averageLatencyMs - m_adaptiveAdmissionPreviousAverageLatencyMs : 0.0;
        const bool averageLatencySignificantlyRising =
            averageLatencyMs > 0.0 &&
            m_adaptiveAdmissionPreviousAverageLatencyMs > 0.0 &&
            averageLatencyGradientMs >
                std::max(35.0,
                         0.12 * m_adaptiveAdmissionPreviousAverageLatencyMs);
        const bool averageLatencyStable =
            averageLatencyMs == 0.0 ||
            m_adaptiveAdmissionPreviousAverageLatencyMs <= 0.0 ||
            averageLatencyGradientMs <=
                std::max(25.0,
                         0.08 * m_adaptiveAdmissionPreviousAverageLatencyMs);
        const bool averageLatencyFalling =
            averageLatencyMs > 0.0 &&
            m_adaptiveAdmissionPreviousAverageLatencyMs > 0.0 &&
            averageLatencyGradientMs <=
                -std::max(15.0,
                          0.05 * m_adaptiveAdmissionPreviousAverageLatencyMs);
        if (averageLatencySignificantlyRising && demandBacklogged) {
            ++m_adaptiveAdmissionAverageLatencyRisingIntervals;
        }
        else if (averageLatencyStable || averageLatencyFalling ||
                 !demandBacklogged) {
            m_adaptiveAdmissionAverageLatencyRisingIntervals = 0;
        }
        const bool latencyRising =
            p95LatencyMs > 0.0 &&
            m_adaptiveAdmissionPreviousP95LatencyMs > 0.0 &&
            p95LatencyMs >
                (m_adaptiveAdmissionPreviousP95LatencyMs +
                 std::max(30.0, 0.10 * m_adaptiveAdmissionPreviousP95LatencyMs));
        const bool latencyWithinStableRegion =
            averageLatencyStable &&
            (averageLatencyMs == 0.0 ||
             averageLatencyMs < hardTargetLatencyMs ||
             averageLatencyFalling);
        if ((averageLatencySignificantlyRising ||
             (latencyRising && queueDelayGradientMs > 0.0)) &&
            !latencyWithinStableRegion) {
            ++m_adaptiveAdmissionLatencyRisingIntervals;
        }
        else if (latencyWithinStableRegion ||
                 averageLatencyFalling ||
                 queueDelayGradientMs <= 0.0 ||
                 (p95LatencyMs > 0.0 &&
                  m_adaptiveAdmissionPreviousP95LatencyMs > 0.0 &&
                  p95LatencyMs < m_adaptiveAdmissionPreviousP95LatencyMs)) {
            m_adaptiveAdmissionLatencyRisingIntervals = 0;
        }
        const bool queueDelayBuilding =
            demandBacklogged &&
            queueDelayMs > queueDelayTargetMs &&
            queueDelayGradientMs > std::max(10.0, 0.10 * queueDelayTargetMs) &&
            (averageLatencySignificantlyRising ||
             m_adaptiveAdmissionAverageLatencyRisingIntervals >= 2);
        const bool latencyStable =
            latencyWithinStableRegion ||
            (averageLatencyStable &&
             !queueDelayBuilding &&
             m_adaptiveAdmissionAverageLatencyRisingIntervals == 0);
        const bool latencyCongested =
            demandBacklogged &&
            latencyBaselineTrusted &&
            !latencyStable &&
            (m_adaptiveAdmissionLatencyRisingIntervals >= 2 ||
             m_adaptiveAdmissionAverageLatencyRisingIntervals >= 2 ||
             m_adaptiveAdmissionQueueDelayOverTargetIntervals >= 3);
        const bool latencySevere =
            demandBacklogged &&
            latencyBaselineTrusted &&
            queueDelayMs > queueDelaySevereMs &&
            (averageLatencyMs > hardTargetLatencyMs ||
             p95LatencyMs > 2.0 * hardTargetLatencyMs);
        const bool tailLatencyDebt =
            demandBacklogged &&
            latencyBaselineTrusted &&
            p95LatencyMs > hardTargetLatencyMs &&
            queueDelayMs > queueDelaySevereMs;
        const bool recoveryMode = false;
        if (latencyCongested) {
            m_adaptiveAdmissionIntervalCongested = true;
        }
        if (latencySevere || queueSevere) {
            m_adaptiveAdmissionIntervalSevere = true;
        }
        const double lowQueueAllowanceMs =
            std::min(75.0, std::max(25.0, 0.50 * queueDelayTargetMs));
        const double latencyBudgetMs =
            m_adaptiveAdmissionBaselineLatencyMs > 0.0 ?
            std::max(m_adaptiveAdmissionBaselineLatencyMs + lowQueueAllowanceMs,
                     p50LatencyMs > 0.0 ? p50LatencyMs : 0.0) :
            targetLatencyMs;
        const bool lossSignal =
            m_adaptiveAdmissionIntervalTimeouts > 0 || queueSevere;
        const bool ecnSignal =
            m_adaptiveAdmissionIntervalQueueWarnings > 0 || queuePastSoftLimit;
        const bool ecnCongestionSignal = ecnSignal && !latencyStable;
        const bool hardDelaySignal = latencySevere || tailLatencyDebt;
        const bool delaySignal =
            latencyCongested || queueDelayBuilding ||
            (latencyBaselineTrusted &&
             m_adaptiveAdmissionQueueDelayOverTargetIntervals >= 3 &&
             !averageLatencyFalling);

        if (lossSignal || hardDelaySignal) {
            const double factor =
                lossSignal ?
                m_adaptiveAdmissionOptions.severeMdFactor :
                m_adaptiveAdmissionOptions.mdFactor;
            m_adaptiveAdmissionSlowStartThreshold = std::max(
                m_adaptiveAdmissionOptions.minWindow,
                static_cast<size_t>(std::ceil(
                    static_cast<double>(m_adaptiveAdmissionWindow) * factor)));
            m_adaptiveAdmissionWindow = std::max(
                m_adaptiveAdmissionOptions.minWindow,
                m_adaptiveAdmissionSlowStartThreshold);
        }
        else if (delaySignal || ecnCongestionSignal) {
            m_adaptiveAdmissionSlowStartThreshold = std::max(
                m_adaptiveAdmissionOptions.minWindow,
                m_adaptiveAdmissionWindow);
            const double factor = delaySignal ? 0.85 : 0.92;
            m_adaptiveAdmissionWindow = std::max(
                m_adaptiveAdmissionOptions.minWindow,
                static_cast<size_t>(std::ceil(
                    static_cast<double>(m_adaptiveAdmissionWindow) * factor)));
        }
        else if (m_adaptiveAdmissionIntervalSuccesses > 0 &&
                 (demandBacklogged || queuePressure ||
                  m_adaptiveAdmissionInflight >=
                    static_cast<size_t>(std::ceil(static_cast<double>(activeLimit) * 0.8))) &&
                 (latencyStable || averageLatencyStable || !latencyBaselineTrusted)) {
            const size_t growthStep = std::max<size_t>(
                m_adaptiveAdmissionOptions.aiStep,
                std::min<size_t>(
                    m_adaptiveAdmissionOptions.aiStep * 4,
                    static_cast<size_t>(std::ceil(
                        static_cast<double>(m_adaptiveAdmissionWindow) * 0.25))));
            m_adaptiveAdmissionWindow = std::min(
                m_adaptiveAdmissionOptions.maxWindow,
                m_adaptiveAdmissionWindow + growthStep);
        }

        if (m_adaptiveAdmissionOptions.rateRecommendationEnabled) {
            const double recommendedLatencyMs =
                averageLatencyMs > 0.0 ? averageLatencyMs :
                p50LatencyMs > 0.0 ? p50LatencyMs :
                m_adaptiveAdmissionBaselineLatencyMs > 0.0 ?
                    m_adaptiveAdmissionBaselineLatencyMs :
                    targetLatencyMs;
            double recommendedRate =
                1000.0 * static_cast<double>(m_adaptiveAdmissionWindow) /
                std::max(1.0, recommendedLatencyMs);
            recommendedRate = std::max(
                m_adaptiveAdmissionOptions.minRecommendedRateRps,
                recommendedRate);
            if (m_adaptiveAdmissionOptions.maxRecommendedRateRps > 0.0) {
                recommendedRate = std::min(
                    recommendedRate,
                    m_adaptiveAdmissionOptions.maxRecommendedRateRps);
            }
            m_adaptiveAdmissionRecommendedRateRps = recommendedRate;
        }

        NDN_LOG_INFO("[NDNSF_ADMISSION] window=" << m_adaptiveAdmissionWindow
                  << " oldWindow=" << oldWindow
                  << " inflight=" << m_adaptiveAdmissionInflight
                  << " queueDepth=" << m_adaptiveAdmissionQueue.size()
                  << " backpressure="
                  << m_adaptiveAdmissionIntervalBackpressure
                  << " queueWarnings="
                  << m_adaptiveAdmissionIntervalQueueWarnings
                  << " successes=" << m_adaptiveAdmissionIntervalSuccesses
                  << " timeouts=" << m_adaptiveAdmissionIntervalTimeouts
                  << " avgLatencyMs=" << averageLatencyMs
                  << " avgLatencyGradientMs=" << averageLatencyGradientMs
                  << " p50LatencyMs=" << p50LatencyMs
                  << " p95LatencyMs=" << p95LatencyMs
                  << " maxLatencyMs=" << maxLatencyMs
                  << " targetLatencyMs=" << targetLatencyMs
                  << " hardTargetLatencyMs=" << hardTargetLatencyMs
                  << " baselineLatencyMs="
                  << m_adaptiveAdmissionBaselineLatencyMs
                  << " queueDelayMs=" << queueDelayMs
                  << " queueDelayGradientMs=" << queueDelayGradientMs
                  << " queueDelayTargetMs=" << queueDelayTargetMs
                  << " queueDelaySevereMs=" << queueDelaySevereMs
                  << " queueDelayOverTargetIntervals="
                  << m_adaptiveAdmissionQueueDelayOverTargetIntervals
                  << " queueDelayBuilding=" << queueDelayBuilding
                  << " lowQueueAllowanceMs=" << lowQueueAllowanceMs
                  << " latencyBudgetMs=" << latencyBudgetMs
                  << " completionRateRps=" << completionRateRps
                  << " completionRateEmaRps="
                  << m_adaptiveAdmissionCompletionRateEmaRps
                  << " recommendedRateRps="
                  << m_adaptiveAdmissionRecommendedRateRps
                  << " warmedIntervals="
                  << m_adaptiveAdmissionSuccessfulControlIntervals
                  << " latencyBaselineTrusted="
                  << latencyBaselineTrusted
                  << " latencyWithinStableRegion="
                  << latencyWithinStableRegion
                  << " latencyRising=" << latencyRising
                  << " latencyStable=" << latencyStable
                  << " latencyRisingIntervals="
                  << m_adaptiveAdmissionLatencyRisingIntervals
                  << " averageLatencyStable=" << averageLatencyStable
                  << " averageLatencyRisingIntervals="
                  << m_adaptiveAdmissionAverageLatencyRisingIntervals
                  << " tailLatencyDebt=" << tailLatencyDebt
                  << " recoveryMode=" << recoveryMode
                  << " recoveryIntervals="
                  << m_adaptiveAdmissionRecoveryIntervals
                  << " aboveWindow=" << aboveWindow
                  << " queuePastSoftLimit=" << queuePastSoftLimit
                  << " queuePressure=" << queuePressure
                  << " queueSevere=" << queueSevere
                  << " latencyCongested=" << latencyCongested
                  << " latencySevere=" << latencySevere
                  << " congested=" << m_adaptiveAdmissionIntervalCongested
                  << " severe=" << m_adaptiveAdmissionIntervalSevere);

        m_adaptiveAdmissionIntervalSuccesses = 0;
        m_adaptiveAdmissionIntervalTimeouts = 0;
        m_adaptiveAdmissionIntervalBackpressure = 0;
        m_adaptiveAdmissionIntervalQueueWarnings = 0;
        m_adaptiveAdmissionIntervalLatencySumMs = 0.0;
        m_adaptiveAdmissionIntervalLatencyCount = 0;
        m_adaptiveAdmissionIntervalLatenciesMs.clear();
        m_adaptiveAdmissionIntervalCongested = false;
        m_adaptiveAdmissionIntervalSevere = false;
        m_adaptiveAdmissionPreviousQueueDelayMs = queueDelayMs;
        if (averageLatencyMs > 0.0) {
            m_adaptiveAdmissionPreviousAverageLatencyMs = averageLatencyMs;
        }
        if (p95LatencyMs > 0.0) {
            m_adaptiveAdmissionPreviousP95LatencyMs = p95LatencyMs;
        }

        drainAdaptiveAdmissionQueue();
        scheduleAdaptiveAdmissionControl();
    }

    void ServiceUser::releaseAdaptiveAdmissionSlot(const ndn::Name& requestId,
                                                   PendingCall& pendingCall,
                                                   const char* reason,
                                                   uint64_t terminalTimestampUs)
    {
        if (!m_adaptiveAdmissionOptions.enabled ||
            !pendingCall.admissionPublished ||
            pendingCall.admissionReleased) {
            return;
        }
        pendingCall.admissionReleased = true;
        if (m_adaptiveAdmissionInflight > 0) {
            --m_adaptiveAdmissionInflight;
        }

        const std::string reasonText = reason == nullptr ? "" : reason;
        const bool timedOut = pendingCall.timedOut || reasonText == "timeout";
        const bool admissionRejected = reasonText == "no_provider_selected";
        if (timedOut) {
            ++m_adaptiveAdmissionIntervalTimeouts;
            m_adaptiveAdmissionIntervalCongested = true;
            m_adaptiveAdmissionIntervalSevere = true;
        }
        else if (admissionRejected) {
            m_adaptiveAdmissionIntervalCongested = true;
        }
        else if (pendingCall.hasResponse || reasonText == "response_callback" ||
                 reasonText == "completed") {
            ++m_adaptiveAdmissionIntervalSuccesses;
        }

        if (pendingCall.publishedAtUs != 0 &&
            terminalTimestampUs >= pendingCall.publishedAtUs &&
            pendingCall.timeoutMs > 0) {
            const double latencyMs =
                static_cast<double>(terminalTimestampUs - pendingCall.publishedAtUs) / 1000.0;
            m_adaptiveAdmissionIntervalLatencySumMs += latencyMs;
            ++m_adaptiveAdmissionIntervalLatencyCount;
            m_adaptiveAdmissionIntervalLatenciesMs.push_back(latencyMs);
            if (latencyMs > 0.9 * static_cast<double>(pendingCall.timeoutMs)) {
                m_adaptiveAdmissionIntervalSevere = true;
            }
        }

        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ADMISSION_RELEASED timestamp_us="
                  << terminalTimestampUs
                  << " requestId=" << requestId.toUri()
                  << " reason=" << reasonText
                  << " inflight=" << m_adaptiveAdmissionInflight
                  << " window=" << m_adaptiveAdmissionWindow
                  << " queueDepth=" << m_adaptiveAdmissionQueue.size());
        drainAdaptiveAdmissionQueue();
    }

    void ServiceUser::logAckMatchAttempt(const ndn::Name& requestId,
                                         const ndn::Name& ackName,
                                         const ndn::Name& providerName,
                                         uint64_t ackReceiveUs,
                                         const char* phase)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        const uint64_t createdAtUs =
            pendingCall != m_pendingCalls.end() ? pendingCall->second.createdAtUs : 0;
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_MATCH_ATTEMPT"
                  << " timestamp_us=" << ackReceiveUs
                  << " requestId=" << requestId.toUri()
                  << " callId=" << requestId.toUri()
                  << " requestName="
                  << (pendingCall != m_pendingCalls.end() &&
                      !pendingCall->second.requestName.empty() ?
                      pendingCall->second.requestName.toUri() : "-")
                  << " ackName=" << ackName.toUri()
                  << " providerName=" << providerName.toUri()
                  << " phase=" << phase
                  << " pendingCallsSize=" << m_pendingCalls.size()
                  << " callCreatedAtUs=" << createdAtUs
                  << " ackReceiveUs=" << ackReceiveUs
                  << " ackToCallCreatedDeltaUs="
                  << (createdAtUs == 0 ? 0 :
                      static_cast<int64_t>(ackReceiveUs) -
                      static_cast<int64_t>(createdAtUs))
                  << " threadId=" << currentThreadIdForTrace());
    }

    void ServiceUser::logAckNoPending(const ndn::Name& requestId,
                                      const ndn::Name& ackName,
                                      const ndn::Name& providerName,
                                      uint64_t ackReceiveUs)
    {
        const auto history = m_pendingCallTraceHistory.find(requestId);
        const bool knownCall = history != m_pendingCallTraceHistory.end();
        const bool afterTimeoutCleanup = knownCall && history->second.timedOut;
        const bool afterCompletionCleanup = knownCall && history->second.completed;
        uint64_t earliestCreatedAtUs = 0;
        for (const auto& item : m_pendingCalls) {
            if (earliestCreatedAtUs == 0 ||
                item.second.createdAtUs < earliestCreatedAtUs) {
                earliestCreatedAtUs = item.second.createdAtUs;
            }
        }
        const bool beforeEarliestPendingCreation =
            earliestCreatedAtUs != 0 && ackReceiveUs < earliestCreatedAtUs;
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_MATCH_FAILED_NO_PENDING_CALL"
                  << " timestamp_us=" << ackReceiveUs
                  << " requestId=" << requestId.toUri()
                  << " callId=" << requestId.toUri()
                  << " ackName=" << ackName.toUri()
                  << " providerName=" << providerName.toUri()
                  << " pendingCallsSize=" << m_pendingCalls.size()
                  << " pendingSample=" << samplePendingCallKeys(5)
                  << " knownCall=" << knownCall
                  << " knownCallCreatedAtUs="
                  << (knownCall ? history->second.createdAtUs : 0)
                  << " knownCallErasedAtUs="
                  << (knownCall ? history->second.erasedAtUs : 0)
                  << " beforeEarliestPendingCreation="
                  << beforeEarliestPendingCreation
                  << " afterTimeoutCleanup=" << afterTimeoutCleanup
                  << " afterCompletionCleanup=" << afterCompletionCleanup
                  << " threadId=" << currentThreadIdForTrace());
        if (afterTimeoutCleanup || afterCompletionCleanup) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_MATCH_FAILED_EXPIRED_CALL"
                      << " timestamp_us=" << ackReceiveUs
                      << " requestId=" << requestId.toUri()
                      << " ackName=" << ackName.toUri()
                      << " providerName=" << providerName.toUri()
                      << " afterTimeoutCleanup=" << afterTimeoutCleanup
                      << " afterCompletionCleanup=" << afterCompletionCleanup);
        }
        if (!m_pendingCalls.empty()) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_MATCH_FAILED_REQUEST_ID_MISMATCH"
                      << " timestamp_us=" << ackReceiveUs
                      << " requestId=" << requestId.toUri()
                      << " ackName=" << ackName.toUri()
                      << " providerName=" << providerName.toUri()
                      << " pendingSample=" << samplePendingCallKeys(5));
        }
    }

    ndn::Name ServiceUser::getName()
    {
        return identity;
    }

    std::shared_ptr<LiveStreamConsumerHandle>
    ServiceUser::openLiveStream(const LiveStreamDescriptor& descriptor,
                                LiveStreamOpenOptions options)
    {
        return std::make_shared<LiveStreamConsumerHandle>(
            descriptor, std::move(options), m_face, validator);
    }

    std::shared_ptr<PredictiveStreamSubscriber>
    ServiceUser::subscribeStream(const PredictiveStreamDescriptor& descriptor,
                                 StreamSubscriptionOptions options)
    {
        auto handle = std::make_shared<PredictiveStreamSubscriber>(
          m_face, validator, descriptor, std::move(options));
        try {
            handle->start();
            return handle;
        }
        catch (...) {
            handle->stop();
            throw;
        }
    }

    void ServiceUser::fetchPermissionsFromController(const ndn::Name& controllerPrefix)
    {
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            m_controllerPrefix = controllerPrefix;
        }
        fetchPolicyManifestFromController(controllerPrefix);

        const auto prepared = prepareControllerControlQuery(controllerPrefix, "USER-PERMISSIONS",
            encodeControlQueryParameters({identity, ndn::Name(), 0, 0}), 1);
        if (!prepared) return;
        const auto interest = *prepared;
        const auto interestName = interest.getName();

        NDN_LOG_INFO("Fetch user permissions: " << interestName
                     << " attempt=1/" << permissionFetchMaxAttempts());
        m_face.expressInterest(
            interest,
            std::bind(&ServiceUser::onPermissionResponseData, this, _1, _2),
            [this](const ndn::Interest& interest, const ndn::lp::Nack&) {
                onPermissionResponseTimeout(interest, 1);
            },
            [this](const ndn::Interest& interest) {
                onPermissionResponseTimeout(interest, 1);
            });
    }

    bool ServiceUser::isRequestBootstrapReady(
        const ndn::Name& serviceName,
        const std::vector<ndn::Name>& providers)
    {
        if (serviceName.empty()) {
            return false;
        }
        // LocalMock fixtures deliberately install their own authorization and
        // crypto boundary. Calling this predicate must not replace the
        // fixture's existing BeginCollaboration checks with a synthetic
        // production bootstrap.
        if (m_isLocalMock) {
            return true;
        }
        if (!activeNacConsumer().readyForDecryption() ||
            !hasUserPermissionForRequest(providers, serviceName)) {
            return false;
        }
        // A configured runtime must also have an accepted service-scoped
        // ControllerVersion before it publishes a protected Request. A
        // controller-free production user is not a supported DI path, but
        // retaining the empty-prefix branch keeps the predicate consistent
        // with ServiceUser's existing compatibility semantics.
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        if (m_controllerPrefix.empty()) {
            return true;
        }
        const auto it = m_revocationStates.find(serviceName.toUri());
        return it != m_revocationStates.end() && it->second.hasCurrentStatus();
    }

    void ServiceUser::applyPermissionResponse(const PermissionResponse& response)
    {
        if (response.getPermissionKind() != tlv::UserPermission) {
            NDN_LOG_ERROR("Ignoring non-user PermissionResponse for "
                          << response.getTargetIdentity());
            return;
        }
        // PermissionResponse carries a controller-version hint, but it is not
        // authoritative for any single service.  A single response can contain
        // records from services at different policy epochs; rejecting it
        // against the process-wide version would let one service suppress
        // another service's renewal.  Exact per-service PolicyStatus validation
        // below owns freshness and monotonicity.

        // A revoked identity may receive an otherwise valid renewal with an
        // empty permission list.  Preserve the previously known service
        // scopes long enough to fetch the corresponding version-addressable
        // PolicyStatus; otherwise the empty response would silently skip the
        // status refresh and leave the old revocation snapshot installed.
        std::set<std::string> previouslyKnownServices;
        for (const auto& existing : m_authorizations.snapshot()) {
            if (existing.permissionKind == tlv::UserPermission &&
                !existing.serviceName.empty()) {
                previouslyKnownServices.insert(ndn::Name(existing.serviceName).toUri(ndn::name::UriFormat::CANONICAL));
            }
        }

        std::vector<ServiceAuthorizationRecord> records;
        records.reserve(response.getEntries().size());
        std::vector<std::pair<ndn::Name, ndn::Name>> producerServices;
        producerServices.reserve(response.getEntries().size());
        for (const auto& entry : response.getEntries()) {
            if (entry.getVersion() != 0 &&
                entry.getVersion() != response.getPolicyEpoch()) {
                NDN_LOG_WARN("Permission entry epoch differs from response epoch provider="
                             << entry.getProviderName()
                             << " service=" << entry.getServiceName()
                             << " entryEpoch=" << entry.getVersion()
                             << " responseEpoch=" << response.getPolicyEpoch());
            }
            const ndn::Name providerServiceName =
                makePermissionFullServiceName(ndn::Name(entry.getProviderName()),
                                              ndn::Name(entry.getServiceName()));
            records.push_back(ServiceAuthorizationRecord{
                providerServiceName.toUri(), entry.getServiceName(),
                tlv::UserPermission, response.getPolicyEpoch()});
            producerServices.emplace_back(ndn::Name(entry.getProviderName()), ndn::Name(entry.getServiceName()));
        }
        if (!m_authorizations.replacePermissions(tlv::UserPermission,
                                                 response.getPolicyEpoch(),
                                                 records)) {
            NDN_LOG_ERROR("Rejected invalid or stale user PermissionResponse epoch="
                          << response.getPolicyEpoch());
            return;
        }
        try { replaceBasicProfileAudience(producerServices); }
        catch (const std::exception&) {
            m_basicProfileAudiences.clear();
            NDN_LOG_WARN("Basic profile audience unavailable; retain legacy Request metadata");
        }
        // Permission responses are scoped to this identity, but their
        // ABE attributes are service-level, independent of provider routes.
        // Detect an actual service authorization change before
        // requesting a replacement DKEY; a newer ControllerVersion caused by
        // another identity must not make this identity refetch its DKEY.
        std::set<std::string> currentServices;
        for (const auto& record : records) {
            currentServices.insert(ndn::Name(record.serviceName).toUri(ndn::name::UriFormat::CANONICAL));
        }
        if (previouslyKnownServices != currentServices) {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            for (const auto& record : records)
                m_nacDkeyRefreshPendingServices.insert(ndn::Name(record.serviceName).toUri(ndn::name::UriFormat::CANONICAL));
            for (const auto& existing : previouslyKnownServices)
                m_nacDkeyRefreshPendingServices.insert(existing);
            NDN_LOG_INFO("NDNSF_NAC_DKEY_REFRESH_PENDING role=user reason=grant-only");
        }
        m_currentPolicyEpoch = response.getPolicyEpoch();
        if (response.hasControllerVersion()) {
            adoptControllerVersion(response.getControllerVersion());
        }
        for (const auto& record : records) {
            NDN_LOG_INFO("Installed user permission provider="
                         << record.providerServiceName
                         << " service=" << record.serviceName
                         << " policyEpoch=" << record.policyEpoch);
        }

        ndn::Name controllerPrefix;
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            controllerPrefix = m_controllerPrefix;
        }
        if (!controllerPrefix.empty()) {
            std::set<std::string> services;
            for (const auto& serviceUri : m_serviceNames) {
                if (!serviceUri.empty()) {
                    services.insert(serviceUri);
                }
            }
            services.insert(previouslyKnownServices.begin(),
                            previouslyKnownServices.end());
            for (const auto& record : records) {
                if (!record.serviceName.empty()) {
                    services.insert(record.serviceName);
                }
            }
            for (const auto& serviceUri : services) {
                fetchPolicyStatusFromController(controllerPrefix,
                                                ndn::Name(serviceUri));
            }
        }
    }

    bool ServiceUser::installControllerStatus(const PolicyStatusData& status,
                                              bool controllerSignatureValid)
    {
        const auto serviceKey = status.getServiceName().toUri();
        if (serviceKey.empty()) {
            return false;
        }
        bool accepted = false;
        bool versionChanged = false;
        bool protectedVersionAdvanced = false;
        bool abeGenerationChanged = true;
        bool grantOnlyDkeyRefresh = false;
        // Controller status and protected serving share one linearization
        // interval.  The commit mutex is acquired first by both status and
        // durable publication finalization; no Face/FileLock work occurs in
        // this interval.
        std::unique_lock<std::mutex> protectedCommit(
            m_protectedReferenceCommitMutex);
        std::unique_lock<std::shared_mutex> protectedFence(
            m_protectedReuseMutex);
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            // Validate both state machines against private copies first.  A
            // coordinator rejection must not leave RevocationState advanced
            // while refresh bookkeeping still describes the old status.
            std::optional<RevocationState> stagedState;
            const auto stateIt = m_revocationStates.find(serviceKey);
            if (stateIt != m_revocationStates.end()) {
                stagedState.emplace(stateIt->second);
            }
            else {
                stagedState.emplace(status.getServiceName());
            }
            const auto previousVersion = stagedState->hasCurrentStatus() ?
                std::optional<ControllerVersion>(stagedState->currentVersion()) :
                std::nullopt;
            const auto previousAbeName = stagedState->hasCurrentStatus() ?
                stagedState->currentStatus().getAbePublicParametersName() : ndn::Name();
            const auto previousAbeDigest = stagedState->hasCurrentStatus() ?
                stagedState->currentStatus().getAbePublicParametersDigest() : std::string();
            if (!stagedState->acceptStatus(status, nowMilliseconds(),
                                           controllerSignatureValid)) {
                return false;
            }

            std::optional<PolicyRefreshCoordinator> stagedCoordinator;
            const auto coordinatorIt = m_policyRefreshCoordinators.find(serviceKey);
            if (coordinatorIt != m_policyRefreshCoordinators.end()) {
                stagedCoordinator.emplace(coordinatorIt->second);
            }
            else {
                stagedCoordinator.emplace(status.getServiceName());
            }
            if (!stagedCoordinator->installCurrentStatus(
                    status, nowMilliseconds(), controllerSignatureValid,
                    m_policyStatusFetchInFlight.count(serviceKey) != 0)) {
                return false;
            }

            m_revocationStates[serviceKey] = std::move(*stagedState);
            m_policyRefreshCoordinators.insert_or_assign(
                serviceKey, std::move(*stagedCoordinator));
            if (!m_controllerVersion ||
                status.getControllerVersion().compare(*m_controllerVersion) > 0) {
                m_controllerVersion = status.getControllerVersion();
            }
            // Keep this process-wide accessor monotonic for legacy callers;
            // protected paths use the service-scoped accessor below.
            m_currentPolicyEpoch = std::max(
                m_currentPolicyEpoch,
                static_cast<size_t>(status.getControllerVersion().controllerEpoch));
            accepted = true;
            versionChanged = !previousVersion ||
                status.getControllerVersion().compare(*previousVersion) > 0;
            // First status installation preserves the existing bootstrap
            // invalidation behavior, but it is not a protected-material
            // retirement transition.  Only a strictly newer service-scoped
            // version advances the durable serving fence.
            protectedVersionAdvanced = previousVersion &&
                status.getControllerVersion().compare(*previousVersion) > 0;
            // ABE public parameters are controller-global rather than
            // per-service.  A first install of this service's status is a
            // generation change only when the identity has no public
            // parameter identity at all yet (cold bootstrap) or the status
            // moves to a different parameter identity than the one already
            // active across installed services.  A same-identity first
            // install (a grant-only addition of a new service) retains the
            // unaffected caches and refreshes only the target DKEY.
            const auto consumerParamsName =
                activeNacConsumer().getPublicParamsDataName();
            const auto consumerParamsDigest =
                activeNacConsumer().getPublicParamsDigest();
            const bool consumerHasParams = !consumerParamsName.empty();
            const bool sameParameterIdentity =
                consumerHasParams &&
                status.getAbePublicParametersName() == consumerParamsName &&
                status.getAbePublicParametersDigest() == consumerParamsDigest;
            abeGenerationChanged = !consumerHasParams
                ? (!previousVersion ||
                   previousAbeName != status.getAbePublicParametersName() ||
                   previousAbeDigest != status.getAbePublicParametersDigest())
                : !sameParameterIdentity;
            const auto pendingServiceKey = status.getServiceName().toUri(ndn::name::UriFormat::CANONICAL);
            const auto pendingDkeyRefresh =
                m_nacDkeyRefreshPendingServices.find(pendingServiceKey);
            if (abeGenerationChanged || pendingDkeyRefresh !=
                m_nacDkeyRefreshPendingServices.end()) {
                // The consumer DKEY is identity-wide: the several service
                // statuses of one permission wave (same ControllerVersion)
                // must collapse into a single replacement fetch.  A
                // generation change always refreshes; a pending grant-only
                // hit refreshes only when this wave has not already driven
                // one (RV-U21 single-issuance).
                const bool refreshForCurrentWave =
                    abeGenerationChanged ||
                    !m_lastDkeyRefreshWave ||
                    *m_lastDkeyRefreshWave != status.getControllerVersion();
                if (refreshForCurrentWave) {
                    m_lastDkeyRefreshWave = status.getControllerVersion();
                }
                grantOnlyDkeyRefresh = !abeGenerationChanged &&
                    refreshForCurrentWave &&
                    pendingDkeyRefresh != m_nacDkeyRefreshPendingServices.end();
                if (abeGenerationChanged)
                    m_nacDkeyRefreshPendingServices.erase(pendingServiceKey);
                else
                    m_nacDkeyRefreshPendingServices.erase(pendingDkeyRefresh);
            }
        }
        if (accepted && protectedVersionAdvanced) {
            retireProtectedPublications(status.getServiceName(),
                                        status.getControllerVersion());
        }
        protectedFence.unlock();
        protectedCommit.unlock();
        if (accepted) {
            if (protectedVersionAdvanced) {
                // Reference pruning is deliberately outside the in-memory
                // fence and never makes status installation fail.  A stale
                // reference remains unusable because durable lookup compares
                // the full service-scoped ControllerVersion.
                const auto identitySnapshot = identity;
                const auto serviceSnapshot = status.getServiceName();
                const auto versionSnapshot = status.getControllerVersion();
                try {
                    postToIo([identitySnapshot, serviceSnapshot, versionSnapshot] {
                        pruneDurableKeyReferences(identitySnapshot,
                                                  serviceSnapshot,
                                                  versionSnapshot);
                    });
                }
                catch (const std::exception&) {
                    // The status transition is already fail-closed in memory;
                    // a stopped I/O context must not roll it back.
                }
            }
            if (versionChanged) {
                invalidateControllerScopedCaches(status.getServiceName(),
                                                  status.getControllerVersion(),
                                                  abeGenerationChanged, &status,
                                                  grantOnlyDkeyRefresh);
            }
            else if (grantOnlyDkeyRefresh) {
                // Reverse-order grant-only install: the status channel (a
                // scheduled refresh or a restore confirmation) installed this
                // exact version before the PermissionResponse recorded the
                // grant, so the version-advance invalidate above already ran
                // with an empty pending set and refreshed nothing.  The
                // pending entry consumed here must still drive the single
                // DKEY-only refresh of this wave (FR-017/SC-022); skipping it
                // would silently lose the grant until the next real version
                // advance.
                refreshNacDkeyForControllerStatus(
                    status.getServiceName(),
                    status.getControllerVersion(),
                    abeGenerationChanged, grantOnlyDkeyRefresh);
            }
            scheduleControllerStatusRefresh(status.getServiceName(), status);
        }
        return accepted;
    }

    void ServiceUser::persistAcceptedControllerStatus(
        const ndn::Data& validatedData, const PolicyStatusData& status)
    {
        if (m_runtimeStatusStore == nullptr) {
            return;
        }
        const auto serviceKey = status.getServiceName().toUri();
        if (serviceKey.empty()) {
            return;
        }
        RuntimeStatusStore::Record record;
        record.serviceName = status.getServiceName();
        record.controllerVersion = status.getControllerVersion();
        record.installTimeMs = nowMilliseconds();
        record.abePublicParametersName = status.getAbePublicParametersName();
        record.abePublicParametersDigest = status.getAbePublicParametersDigest();
        const auto policyWire = status.wireEncode();
        const auto dataWire = validatedData.wireEncode();
        record.policyStatusWire =
            ndn::Buffer(policyWire.data(), policyWire.size());
        record.statusDataWire =
            ndn::Buffer(dataWire.data(), dataWire.size());
        m_persistedRuntimeStatuses[serviceKey] = std::move(record);
        std::vector<RuntimeStatusStore::Record> records;
        records.reserve(m_persistedRuntimeStatuses.size());
        for (const auto& entry : m_persistedRuntimeStatuses) {
            records.push_back(entry.second);
        }
        if (!m_runtimeStatusStore->persist(records)) {
            NDN_LOG_ERROR("Runtime status persist failed service=" << serviceKey);
        }
    }

    void ServiceUser::restorePersistedRuntimeStatuses()
    {
        if (m_runtimeStatusStore == nullptr) {
            return;
        }
        std::vector<RuntimeStatusStore::Record> records;
        try {
            if (!m_runtimeStatusStore->load(records)) {
                NDN_LOG_WARN("Runtime status restore skipped: no usable store"
                             " (cold start or corrupt store fails closed)");
                return;
            }
        }
        catch (const std::exception& e) {
            NDN_LOG_ERROR("Runtime status restore aborted: " << e.what());
            return;
        }
        if (records.empty()) {
            return;
        }
        NDN_LOG_WARN("Runtime status restore candidates=" << records.size());
        const auto nowMs = nowMilliseconds();
        for (const auto& record : records) {
            const auto& serviceName = record.serviceName;
            const auto serviceKey = serviceName.toUri();
            if (record.policyStatusWire.empty() || record.statusDataWire.empty()) {
                continue;
            }
            // Decode the persisted status for the expiry gate and for the
            // Controller identity the signed Data must be bound to.
            PolicyStatusData persistedStatus;
            bool decoded = false;
            try {
                decoded = persistedStatus.wireDecode(ndn::Block(ndn::span<const uint8_t>(
                        record.policyStatusWire.data(),
                        record.policyStatusWire.size())));
            }
            catch (const std::exception&) {
                decoded = false;
            }
            if (!decoded) {
                NDN_LOG_ERROR("Runtime status restore: undecodable status"
                              << " service=" << serviceKey);
                continue;
            }
            if (!persistedStatus.validate(nowMs)) {
                NDN_LOG_WARN("Runtime status restore: expired, not restored"
                             << " service=" << serviceKey);
                continue;
            }
            ndn::Name controllerIdentity;
            try {
                controllerIdentity = ndn::security::extractIdentityFromCertName(
                    persistedStatus.getControllerCertificate());
            }
            catch (const std::exception&) {
                controllerIdentity = ndn::Name();
            }
            if (controllerIdentity.empty()) {
                NDN_LOG_ERROR("Runtime status restore: malformed controller cert"
                              << " service=" << serviceKey);
                continue;
            }
            const auto expectedVersion = record.controllerVersion;
            const auto seedRecord =
                std::make_shared<RuntimeStatusStore::Record>(record);
            auto signedData = std::make_shared<ndn::Data>();
            try {
                signedData->wireDecode(ndn::Block(ndn::span<const uint8_t>(
                    record.statusDataWire.data(),
                    record.statusDataWire.size())));
            }
            catch (const std::exception&) {
                NDN_LOG_ERROR("Runtime status restore: undecodable signed Data"
                              << " service=" << serviceKey);
                continue;
            }
            // A persisted record is only seeded after the very same checks a
            // live status Data must pass: trust-anchor validation of the
            // signed Data, the signer-identity binding, and the exact-version
            // accept gate.  Until a record passes, this process behaves as a
            // cold start for that service.
            validator->validateWithConfiguredTrustSchema(
                *signedData,
                [this, serviceKey, serviceName, controllerIdentity,
                 expectedVersion, seedRecord](const ndn::Data& validatedData) {
                    if (!isSignedByIdentity(validatedData, controllerIdentity)) {
                        NDN_LOG_ERROR("Runtime status restore signer mismatch"
                                     << " service=" << serviceKey);
                        return;
                    }
                    PolicyStatusData status;
                    const auto& content = validatedData.getContent();
                    bool ok = content.type() == PolicyStatusData::TYPE &&
                        status.wireDecode(content);
                    if (!ok && content.value_size() > 0) {
                        auto [parsed, block] = ndn::Block::fromBuffer(
                            ndn::span<const uint8_t>(content.value(),
                                                     content.value_size()));
                        ok = parsed && status.wireDecode(block);
                    }
                    if (!ok || status.getServiceName() != serviceName ||
                        status.getControllerVersion() != expectedVersion ||
                        !status.validate(nowMilliseconds()) ||
                        !installControllerStatus(status, true)) {
                        NDN_LOG_ERROR("Runtime status restore rejected"
                                     << " service=" << serviceKey);
                        return;
                    }
                    {
                        RuntimeStatusStore::Record current = *seedRecord;
                        current.installTimeMs = nowMilliseconds();
                        m_persistedRuntimeStatuses[serviceKey] = std::move(current);
                    }
                    std::vector<RuntimeStatusStore::Record> refresh;
                    refresh.reserve(m_persistedRuntimeStatuses.size());
                    for (const auto& entry : m_persistedRuntimeStatuses) {
                        refresh.push_back(entry.second);
                    }
                    if (!m_runtimeStatusStore->persist(refresh)) {
                        NDN_LOG_ERROR("Runtime status restore persist failed"
                                     << " service=" << serviceKey);
                    }
                    NDN_LOG_WARN("Runtime status restored service=" << serviceKey
                                 << " generation="
                                 << expectedVersion.controllerGenerationTimestamp
                                 << " epoch=" << expectedVersion.controllerEpoch);
                    // Bounded online confirmation: the Controller's current
                    // status is authority.  If it has advanced or revoked, the
                    // normal accept path installs the newer version; offline,
                    // the restore stands until the scheduled refresh or an
                    // explicit fetch supersedes it.
                    fetchPolicyStatusFromController(
                        controllerIdentity, serviceName, 1);
                },
                [serviceKey](const ndn::Data& badData,
                                   const ndn::security::ValidationError& error) {
                    NDN_LOG_ERROR("Runtime status restore validation failed"
                                 << " service=" << serviceKey
                                 << " reason=" << error);
                });
        }
    }

    void ServiceUser::invalidateControllerScopedCaches(
        const ndn::Name& serviceName,
        const ControllerVersion& version,
        bool abeGenerationChanged,
        const PolicyStatusData* status,
        bool grantOnlyDkeyRefresh)
    {
        if (serviceName.empty() || !version.isValid()) {
            return;
        }

        if (m_securityProfiles) m_securityProfiles->invalidate();
        m_profileReaderObservations.clear();
        // Snapshot affected requests before evicting credentials.  An
        // ordinary unary request must remain until its original deadline so
        // the public timeout callback still receives a terminal outcome when
        // an in-flight response is rejected under the newer status.  Streams
        // are different: their consumer can report Unauthorized immediately,
        // after which the pending request may be cleaned up.
        std::vector<ndn::Name> affectedRequests;
        for (const auto& item : m_pendingCalls) {
            if (item.second.serviceName == serviceName) {
                affectedRequests.push_back(item.first);
            }
        }
        for (const auto& requestId : affectedRequests) {
            auto pending = m_pendingCalls.find(requestId);
            if (pending == m_pendingCalls.end()) {
                continue;
            }

            if (pending->second.streamOptions) {
                auto consumer = m_streamConsumers.find(requestId);
                if (consumer != m_streamConsumers.end()) {
                    consumer->second->reject(
                        StreamedInvocationErrorCode::Unauthorized,
                        "stream request rejected by Controller version change");
                    m_streamConsumers.erase(consumer);
                }
                disarmStreamInactivityTimer(requestId);
                auto state = m_streamStates.find(requestId);
                if (state != m_streamStates.end()) {
                    std::lock_guard<std::mutex> lock(state->second->mutex);
                    state->second->status = StreamedInvocationStatus::Failed;
                }
                erasePendingCallWithTrace(requestId, pending,
                                          "controller_version_changed");
                m_streamEventKeysPending.erase(requestId);
                m_streamStates.erase(requestId);
                {
                    std::lock_guard<std::mutex> lock(m_streamInactivityMutex);
                    m_streamInactivityEpochs.erase(requestId);
                }
            }
            else {
                // Keep the PendingCall and deadline alive, but remove every
                // request-scoped secret and binding that could authorize a
                // response after withdrawal.  A later response is rejected
                // by authorizeControllerTransition; the existing deadline
                // then invokes the normal public timeout callback.
                if (pending->second.requestScopedKeys) {
                    const auto keyId = pending->second.requestScopedKeys->keyId;
                    pending->second.requestScopedKeys->zeroize();
                    m_requestScopedNonceRegistry.invalidate(keyId);
                }
                for (auto& [provider, state] :
                     pending->second.requestScopedProviderStates) {
                    if (!state.keys.keyId.empty()) {
                        const auto keyId = state.keys.keyId;
                        state.keys.zeroize();
                        m_requestScopedNonceRegistry.invalidate(keyId);
                    }
                }
                pending->second.requestScopedKeys.reset();
                pending->second.requestScopedBinding.reset();
                pending->second.requestScopedPlaintext.clear();
                pending->second.selectionGatedInputKey.clear();
                pending->second.requestScopedInputDataName.clear();
                pending->second.requestScopedProviderStates.clear();
                pending->second.providerTokens.clear();
                pending->second.selectionAssignmentPayloads.clear();
                pending->second.collaborationScopeKeys.clear();
                pending->second.collaborationAssignments.clear();
            }
            {
                std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
                m_userCollaborationScopeKeys.erase(requestId);
                m_verifiedCollaborationData.erase(requestId);
            }
        }

        const auto serviceUri = serviceName.toUri();
        const auto hybridKeys = m_hybridMessageCrypto.invalidateService(serviceName);
        // NAC-ABE's DKEY and encrypted-CK caches are authority/identity
        // scoped rather than service-keyed.  Clear the shared instances on a
        // validated status change so an old DKEY cannot authorize a future
        // policy epoch.  NDNSF's service-scoped gates and caches above remain
        // selective; unaffected services may re-bootstrap their same current
        // rights after this bounded refresh.
        auto& nacConsumer = activeNacConsumer();
        if (abeGenerationChanged) {
            if (status != nullptr) {
                nacConsumer.clearCache(status->getAbePublicParametersName(),
                                       status->getAbePublicParametersDigest());
            }
            else {
                nacConsumer.clearCache();
            }
        }
        // The NAC-ABE cache reset intentionally removes the identity DKEY as
        // well as service content keys.  A grant-only wave refreshes only the
        // target DKEY; a generation wave refetches everything (shared helper
        // keeps the equal-version install path from losing the refresh).
        refreshNacDkeyForControllerStatus(serviceName, version,
                                          abeGenerationChanged,
                                          grantOnlyDkeyRefresh);
        if (abeGenerationChanged) {
            activeNacProducer().clearCache();
            if (status != nullptr) {
                activeNacProducer().refreshPublicParameters(
                    status->getAbePublicParametersName(),
                    status->getAbePublicParametersDigest());
            }
        }
        size_t targetedPools = 0;
        {
            std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
            auto isServicePool = [&serviceUri](const auto& item) {
                const auto delimiter = item.first.rfind('|');
                return delimiter != std::string::npos &&
                       item.first.substr(delimiter + 1) == serviceUri;
            };
            for (auto it = m_targetedTokenPools.begin();
                 it != m_targetedTokenPools.end();) {
                if (isServicePool(*it)) {
                    ++targetedPools;
                    it = m_targetedTokenPools.erase(it);
                }
                else {
                    ++it;
                }
            }
            for (auto it = m_targetedTokenPoolControls.begin();
                 it != m_targetedTokenPoolControls.end();) {
                if (isServicePool(*it)) {
                    it = m_targetedTokenPoolControls.erase(it);
                }
                else {
                    ++it;
                }
            }
            for (auto it = m_targetedStreamOffers.begin();
                 it != m_targetedStreamOffers.end();) {
                if (isServicePool(*it)) {
                    it = m_targetedStreamOffers.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        m_recentAckProvidersByService.erase(serviceName);

        NDN_LOG_WARN("NDNSF_CONTROLLER_CACHE_INVALIDATED role=user"
                     << " serviceName=" << serviceUri
                     << " generation=" << version.controllerGenerationTimestamp
                     << " epoch=" << version.controllerEpoch
                     << " pendingRequests=" << affectedRequests.size()
                     << " hybridKeys=" << hybridKeys
                     << " targetedPools=" << targetedPools
                     << " abeGenerationChanged=" << (abeGenerationChanged ? "true" : "false")
                     << " nacAbeCaches="
                     << (abeGenerationChanged ? "authority-shared-cleared" :
                         "retained-grant-only-same-generation")
                     << " runtimeFamilies=targeted-token,selection-binding,nonce,in-flight-request"
                     << " stateFamilies=abe,message-key,replay");
    }

    void ServiceUser::refreshNacDkeyForControllerStatus(
        const ndn::Name& serviceName, const ControllerVersion& version,
        bool abeGenerationChanged, bool grantOnlyDkeyRefresh)
    {
        if (serviceName.empty() || !version.isValid()) {
            return;
        }
        const auto serviceUri = serviceName.toUri();
        // The NAC-ABE cache reset intentionally removes the identity DKEY as
        // well as service content keys. Re-arm the DKEY fetch immediately so
        // the next request can recover after a generation transition. For a
        // grant-only transition the old key remains active until the complete
        // replacement is decoded and installed. Consumer coalescing handles
        // a constructor-time fetch that is still in flight. A LocalMock that
        // has not installed its fixture-owned Consumer defers this operation;
        // its explicit bootstrap will perform the initial fetch.
        auto& nacConsumer = activeNacConsumer();
        const bool canRefreshDkey = !m_isLocalMock || m_testNacConsumer != nullptr;
        if ((abeGenerationChanged || grantOnlyDkeyRefresh) && canRefreshDkey) {
            if (grantOnlyDkeyRefresh)
                nacConsumer.refreshDecryptionKey();
            else
                nacConsumer.obtainDecryptionKey();
            NDN_LOG_INFO("NDNSF_NAC_DKEY_REFRESH_REQUESTED role=user"
                         << " serviceName=" << serviceUri
                         << " epoch=" << version.controllerEpoch
                         << " reason=" << (abeGenerationChanged ?
                             "generation-change" : "grant-only"));
        }
        else if (abeGenerationChanged || grantOnlyDkeyRefresh) {
            NDN_LOG_INFO("NDNSF_NAC_DKEY_REFRESH_DEFERRED role=user"
                         << " serviceName=" << serviceUri
                         << " epoch=" << version.controllerEpoch
                         << " reason=local-mock-bootstrap");
            scheduleDeferredDkeyRefreshRetry(serviceName);
        }
        else {
            NDN_LOG_DEBUG("NDNSF_NAC_DKEY_REFRESH_NOT_REQUIRED role=user"
                         << " serviceName=" << serviceUri
                         << " epoch=" << version.controllerEpoch
                         << " reason=same-generation-no-grant");
        }
    }

    void ServiceUser::scheduleControllerStatusRefresh(
        const ndn::Name& serviceName, const PolicyStatusData& status)
    {
        if (serviceName.empty() || !status.getControllerVersion().isValid()) {
            return;
        }

        ndn::Name controllerPrefix;
        const auto serviceKey = serviceName.toUri();
        const auto now = nowMilliseconds();
        const auto revalidationPeriodMs = policyRevalidationPeriodMs();
        const auto expiry = status.getValidUntilMs();
        const auto remaining = expiry > now ? expiry - now : 0;
        const auto lead = remaining == 0 ? 0 :
            std::min<uint64_t>(60000, std::max<uint64_t>(1000, remaining / 4));
        // The revalidation knob (default 0 = off) replaces the far-future
        // near-expiry delay with a fixed period so the production scheduled
        // refresh path discovers a controller-side change inside a bounded
        // MiniNDN window.  Each completed fetch re-arms the next period in
        // the fire handler below, because the single-flight version guard
        // would otherwise suppress every same-version re-arm after the
        // first fire.
        const auto delay = revalidationPeriodMs > 0 ? revalidationPeriodMs :
            (remaining > lead ? remaining - lead : 0);
        const auto version = status.getControllerVersion();
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            controllerPrefix = m_controllerPrefix;
            if (controllerPrefix.empty()) {
                return;
            }
            const auto scheduled = m_policyStatusRefreshScheduled.find(serviceKey);
            if (scheduled != m_policyStatusRefreshScheduled.end() &&
                scheduled->second.compare(version) >= 0) {
                return;
            }
            m_policyStatusRefreshScheduled[serviceKey] = version;
        }

        m_scheduler.schedule(ndn::time::milliseconds(delay),
            [this, serviceName, serviceKey, version, lead, revalidationPeriodMs] {
                ndn::Name prefix;
                bool shouldFetch = false;
                bool rearmForPeriodicRefresh = false;
                {
                    std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
                    const auto scheduled = m_policyStatusRefreshScheduled.find(serviceKey);
                    if (scheduled == m_policyStatusRefreshScheduled.end() ||
                        scheduled->second != version) {
                        return;
                    }
                    prefix = m_controllerPrefix;
                    const auto coordinator = m_policyRefreshCoordinators.find(serviceKey);
                    if (coordinator != m_policyRefreshCoordinators.end()) {
                        // The coordinator admits a scheduled refresh only near
                        // status expiry; under the knob the status is still far
                        // from expiry, so pass an unbounded lead and let the
                        // revalidation fetch proceed.
                        const auto result = coordinator->second.startScheduledRefresh(
                            nowMilliseconds(),
                            revalidationPeriodMs > 0
                                ? std::numeric_limits<uint64_t>::max() / 2 : lead);
                        shouldFetch = result.outcome ==
                            PolicyRefreshCoordinator::Outcome::FETCH_STARTED;
                        rearmForPeriodicRefresh = revalidationPeriodMs > 0 &&
                            (result.outcome ==
                                 PolicyRefreshCoordinator::Outcome::FETCH_STARTED ||
                             result.outcome ==
                                 PolicyRefreshCoordinator::Outcome::FETCH_COALESCED);
                    }
                    if (rearmForPeriodicRefresh) {
                        const auto current = m_policyStatusRefreshScheduled.find(serviceKey);
                        if (current != m_policyStatusRefreshScheduled.end() &&
                            current->second == version) {
                            // Drop the armed version so the receipt of this
                            // fetch re-arms the next periodic revalidation;
                            // the single-flight guard above would otherwise
                            // suppress a same-version re-arm forever.
                            m_policyStatusRefreshScheduled.erase(current);
                        }
                    }
                }
                if (shouldFetch && !prefix.empty()) {
                    NDN_LOG_INFO("NDNSF_POLICY_STATUS_REVALIDATION role=user"
                                 << " serviceName=" << serviceName.toUri()
                                 << " generation=" << version.controllerGenerationTimestamp
                                 << " epoch=" << version.controllerEpoch);
                    // Both periodic and near-expiry refresh discover the current
                    // status; the coordinator enforces the required version floor.
                    fetchPolicyStatusFromController(prefix, serviceName, 1, std::nullopt);
                }
            });
    }

    void ServiceUser::scheduleDeferredDkeyRefreshRetry(const ndn::Name& serviceName)
    {
        const auto attempts = std::make_shared<size_t>(0);
        const auto retry = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weakRetry = retry;
        *retry = [this, serviceName, attempts, weakRetry] {
            if (*attempts >= 20) {
                NDN_LOG_WARN("NDNSF_NAC_DKEY_RETRY_EXHAUSTED role=user"
                             << " serviceName=" << serviceName.toUri());
                return;
            }
            ++*attempts;
            if (nacConsumer.readyForDecryption())
                return;
            nacConsumer.obtainDecryptionKey();
            if (const auto self = weakRetry.lock()) {
                m_scheduler.schedule(ndn::time::milliseconds(250), *self);
            }
        };
        m_scheduler.schedule(ndn::time::milliseconds(250), *retry);
    }

    bool ServiceUser::hasRequestScopedDefault() const
    {
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        return !m_controllerPrefix.empty() && m_controllerVersion.has_value() &&
               m_controllerVersion->isValid();
    }

    std::optional<ControllerVersion> ServiceUser::getControllerVersion() const
    {
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        return m_controllerVersion;
    }

    std::optional<ControllerVersion>
    ServiceUser::getControllerVersion(const ndn::Name& serviceName) const
    {
        if (serviceName.empty()) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        const auto it = m_revocationStates.find(serviceName.toUri());
        if (it != m_revocationStates.end() && it->second.hasCurrentStatus()) {
            return it->second.currentVersion();
        }
        // Controller-free unit/local callers historically use the process
        // value as their compatibility snapshot.  Configured runtimes must
        // not borrow another service's authority.
        if (m_controllerPrefix.empty()) {
            return m_controllerVersion;
        }
        return std::nullopt;
    }

    void ServiceUser::adoptControllerVersion(const ControllerVersion& version)
    {
        if (!version.isValid()) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        // Permission responses and manifests carry only a refresh hint.  A
        // configured runtime may adopt authority exclusively through an
        // exact, Controller-signed PolicyStatus; retaining a hint here would
        // allow an unverified version to become the local comparison point.
        if (!m_controllerPrefix.empty()) {
            NDN_LOG_DEBUG("Ignoring non-status ControllerVersion hint generation="
                          << version.controllerGenerationTimestamp
                          << " epoch=" << version.controllerEpoch);
            return;
        }
        if (!m_controllerVersion || version.compare(*m_controllerVersion) > 0) {
            m_controllerVersion = version;
        }
    }

    size_t ServiceUser::getCurrentPolicyEpoch() const
    {
        return m_currentPolicyEpoch;
    }

    size_t ServiceUser::getCurrentPolicyEpoch(const ndn::Name& serviceName) const
    {
        if (serviceName.empty()) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        const auto it = m_revocationStates.find(serviceName.toUri());
        if (it != m_revocationStates.end() && it->second.hasCurrentStatus()) {
            return static_cast<size_t>(
                it->second.currentVersion().controllerEpoch);
        }
        return m_controllerPrefix.empty() ? m_currentPolicyEpoch : 0;
    }

    std::vector<std::tuple<std::string, std::string, size_t>>
    ServiceUser::getAllowedServices() const
    {
        return m_authorizations.dumpAllowedServices();
    }

    std::map<std::string, ndnsd::discovery::Details>
    ServiceUser::getNdnsdReceivedDetails() const
    {
        return m_ServiceDiscovery.getReceivedServiceDetails();
    }

    bool ServiceUser::isAcceptablePolicyEpoch(size_t messageEpoch) const
    {
        return m_currentPolicyEpoch == 0 || messageEpoch == 0 ||
               messageEpoch == m_currentPolicyEpoch;
    }

    bool ServiceUser::isAcceptablePolicyEpoch(const ndn::Name& serviceName,
                                              size_t messageEpoch) const
    {
        if (serviceName.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        const auto it = m_revocationStates.find(serviceName.toUri());
        if (it != m_revocationStates.end() && it->second.hasCurrentStatus()) {
            const auto currentEpoch = static_cast<size_t>(
                it->second.currentVersion().controllerEpoch);
            // LocalMock fixtures have no Controller authority and historically
            // omit policyEpoch.  Keep that compatibility only for the
            // controller-free path; configured runtimes require exact, nonzero
            // service-scoped epochs.
            return m_controllerPrefix.empty() ?
                (messageEpoch == 0 || messageEpoch == currentEpoch) :
                messageEpoch == currentEpoch;
        }
        return m_controllerPrefix.empty() &&
               (m_currentPolicyEpoch == 0 || messageEpoch == 0 ||
                messageEpoch == m_currentPolicyEpoch);
    }

    bool ServiceUser::isAcceptableControllerVersion(
        const std::optional<ControllerVersion>& messageVersion) const
    {
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        if (!m_controllerVersion) {
            // A configured Controller marks this as a protected runtime.  Until a
            // signed, non-zero status is installed for the service, accepting a
            // missing message version would be an authority bypass.  Controller-
            // free LocalMock/unit callers retain the historical compatibility
            // behavior so unrelated API tests do not become an implicit bootstrap.
            return m_controllerPrefix.empty();
        }
        return messageVersion && *messageVersion == *m_controllerVersion;
    }

    bool ServiceUser::isAcceptableControllerVersion(
        const ndn::Name& serviceName,
        const std::optional<ControllerVersion>& messageVersion) const
    {
        if (serviceName.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        const auto it = m_revocationStates.find(serviceName.toUri());
        if (it != m_revocationStates.end() && it->second.hasCurrentStatus()) {
            return messageVersion &&
                   *messageVersion == it->second.currentVersion();
        }
        if (m_controllerPrefix.empty()) {
            if (!m_controllerVersion) {
                return true;
            }
            return messageVersion && *messageVersion == *m_controllerVersion;
        }
        return false;
    }

    bool ServiceUser::prepareRequestControllerVersion(
        ndn_service_framework::RequestMessage& requestMessage,
        const ndn::Name& serviceName,
        const ndn::Name& requestId)
    {
        if (!authorizeControllerTransition(serviceName,
                                           ProtectedTransition::DISCOVERY)) {
            NDN_LOG_ERROR("Reject request under revoked Controller status serviceName="
                          << serviceName.toUri());
            return false;
        }

        // Constructors return while the initial DKEY fetch is pending. A
        // permission response alone cannot make ACK decryption usable yet.
        // LocalMock fixtures own their explicit crypto/publication boundary.
        if (!m_isLocalMock && !activeNacConsumer().readyForDecryption()) {
            NDN_LOG_ERROR("Reject request without decryption readiness serviceName="
                          << serviceName.toUri());
            return false;
        }

        const auto currentControllerVersion = getControllerVersion(serviceName);
        if (!currentControllerVersion) {
            // Controller-free LocalMock callers intentionally retain the
            // historical versionless path.  A configured runtime with no
            // accepted service status must fail closed before publishing a
            // request that the Provider would reject as stale.
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            if (!m_controllerPrefix.empty()) {
                NDN_LOG_ERROR("Reject request without installed ControllerVersion serviceName="
                              << serviceName.toUri());
                return false;
            }
            return true;
        }

        if (!requestMessage.hasControllerVersion()) {
            requestMessage.setControllerVersion(*currentControllerVersion);
        }
        else if (!isAcceptableControllerVersion(
                     serviceName, requestMessage.getControllerVersion())) {
            NDN_LOG_ERROR("Reject request with stale ControllerVersion serviceName="
                          << serviceName.toUri()
                          << " requestId=" << requestId.toUri());
            return false;
        }
        return true;
    }

    bool ServiceUser::authorizeControllerTransition(
        const ndn::Name& serviceName,
        ProtectedTransition transition) const
    {
        if (serviceName.empty()) {
            return false;
        }

        const auto certificateWire = identityCert.wireEncode();
        const auto certificateDigest = sha256DigestString(ndn::Buffer(
            certificateWire.data(), certificateWire.data() + certificateWire.size()));
        const AuthorizationSubject subject{
            identityCert.getIdentity(), certificateDigest, serviceName,
            makeUserAuthorizationAttribute(serviceName)};

        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        const auto it = m_revocationStates.find(serviceName.toUri());
        if (it == m_revocationStates.end()) {
            // Once permissions were obtained from a Controller, a missing
            // per-service signed status is fail-closed.  The empty-prefix
            // branch is only for controller-free LocalMock/unit callers.
            return m_controllerPrefix.empty();
        }
        const auto decision = it->second.authorize(subject, transition,
                                                   nowMilliseconds());
        if (!decision.allowed) {
            NDN_LOG_WARN("NDNSF_USER_REVOCATION_REJECT service="
                         << serviceName << " transition="
                         << static_cast<int>(transition)
                         << " reason=" << decision.reason);
        }
        return decision.allowed;
    }

    bool ServiceUser::handlePermissionResponseData(const ndn::Data& data,
                                                   const ndn::Name& identity,
                                                   ndn::KeyChain& keyChain,
                                                   ServiceAuthorizationTable& permissionTable)
    {
        PermissionResponse response;
        EncryptedPermissionResponse encryptedResponse;
        if (decodeEncryptedPermissionResponseFromDataContent(data, encryptedResponse)) {
            try {
                response = decryptPermissionResponseWithKeyChain(encryptedResponse, keyChain);
            }
            catch (const std::exception& e) {
                NDN_LOG_ERROR("Failed to decrypt encrypted PermissionResponse from "
                              << data.getName() << ": " << e.what());
                return false;
            }

            NDN_LOG_INFO("Received encrypted PermissionResponse: "
                         << response.toString());
        }
        else {
            NDN_LOG_ERROR("Reject plaintext PermissionResponse from "
                          << data.getName());
            return false;
        }

        if (ndn::Name(response.getTargetIdentity()) != identity) {
            NDN_LOG_ERROR("Ignoring PermissionResponse for unexpected targetIdentity="
                          << response.getTargetIdentity()
                          << " expected=" << identity.toUri());
            return false;
        }

        if (response.getPermissionKind() != tlv::UserPermission) {
            NDN_LOG_ERROR("Ignoring non-user PermissionResponse for "
                          << response.getTargetIdentity());
            return false;
        }

        std::vector<ServiceAuthorizationRecord> records;
        records.reserve(response.getEntries().size());
        for (const auto& entry : response.getEntries()) {
            const ndn::Name providerServiceName =
                makePermissionFullServiceName(ndn::Name(entry.getProviderName()),
                                              ndn::Name(entry.getServiceName()));
            records.push_back(ServiceAuthorizationRecord{
                providerServiceName.toUri(), entry.getServiceName(),
                tlv::UserPermission, response.getPolicyEpoch()});
        }
        return permissionTable.replacePermissions(tlv::UserPermission,
                                                  response.getPolicyEpoch(),
                                                  records);
    }

    void ServiceUser::replaceBasicProfileAudience(
        const std::vector<std::pair<ndn::Name, ndn::Name>>& producerServices)
    {
        // No stale projection may survive an allocation/validation failure.
        // Execution still uses the authoritative permission table.
        m_basicProfileAudiences.clear();
        std::map<std::string, std::vector<ProfileAudienceMember>> next;
        for (const auto& [producer, service] : producerServices) {
            if (producer.empty() || service.empty()) return;
            auto& audience = next[service.toUri(ndn::name::UriFormat::CANONICAL)];
            if (std::none_of(audience.begin(), audience.end(), [&](const auto& member) {
                    return member.producer == producer;
                })) {
                audience.push_back(ProfileAudienceMember{producer,
                    makePermissionFullServiceName(producer, service).toUri(ndn::name::UriFormat::CANONICAL)});
            }
        }
        m_basicProfileAudiences = std::move(next);
    }

    void ServiceUser::rememberBasicProfileReader(const ndn::Name& producer, const ndn::Name& service,
        const RequestAckMessage& ack, const AckAuthenticationEvidence& evidence)
    {
        if (!evidence.trustSchemaValidated || evidence.signerIdentity.empty() ||
            evidence.signerKeyLocator.empty() || evidence.wireDigest.empty() ||
            !hasUserPermissionForProvider(producer, service)) return;
        const auto version = ack.hasControllerVersion() ?
            std::optional<ControllerVersion>(ack.getControllerVersion()) : std::nullopt;
        if (!isAcceptablePolicyEpoch(service, ack.getPolicyEpoch()) ||
            !isAcceptableControllerVersion(service, version)) return;
        const auto key = std::make_pair(producer, service);
        try {
            if (ndn::Name(evidence.signerIdentity) != producer) return;
            if (!ack.getStatus() || !ack.hasProviderSecurityProfile() ||
                !ack.hasProviderEncryptionCertificate()) {
                m_profileReaderObservations.erase(key);
                return;
            }
            const auto now = nowMilliseconds();
            const auto& advertisement = ack.getProviderEncryptionCertificate();
            const auto epoch = getCurrentPolicyEpoch(service);
            if (epoch == 0 || ack.getPolicyEpoch() != epoch || !advertisement.isValid(now) ||
                ndn::security::extractIdentityFromCertName(advertisement.certificateName) != producer ||
                !producer.isPrefixOf(ack.getProviderSecurityProfile())) return;
            for (auto it = m_profileReaderObservations.begin(); it != m_profileReaderObservations.end();) {
                if (it->second.expiresAtMs <= now) it = m_profileReaderObservations.erase(it);
                else ++it;
            }
            if (!m_profileReaderObservations.count(key) && m_profileReaderObservations.size() >= 64)
                m_profileReaderObservations.erase(m_profileReaderObservations.begin());
            m_profileReaderObservations[key] = ProfileReaderObservation{
                producer, service, epoch, std::min<uint64_t>(advertisement.validUntilMs, now + 5000)};
        }
        catch (const std::exception&) { }
    }

    bool ServiceUser::isBasicReaderObservationCurrent(const ndn::Name& requestID,
        const ndn::Name& producer, uint64_t serial) const
    {
        const auto pending = m_pendingCalls.find(requestID);
        if (pending == m_pendingCalls.end() || pending->second.targetedMode ||
            pending->second.hasResponse || pending->second.timedOut) return false;
        const auto& call = pending->second;
        const auto deadline = call.requestDeadlineUs != 0 ? call.requestDeadlineUs :
            (call.timeoutMs > 0 ? call.createdAtUs + static_cast<uint64_t>(call.timeoutMs) * 1000 : 0);
        if (deadline != 0 && deadline <= nowMicroseconds()) return false;
        if (!(call.requestMessage.hasUserSecurityProfile() ||
              (call.requestMessage.hasRequestCapabilities() &&
               call.requestMessage.getRequestCapabilities().hasField("SecurityProfileReferenceV1") &&
               call.requestMessage.getRequestCapabilities().getField("SecurityProfileReferenceV1") == "1")) ||
            call.requestMessage.getPolicyEpoch() != getCurrentPolicyEpoch(call.serviceName) ||
            !hasUserPermissionForProvider(producer, call.serviceName)) return false;
        const auto audience = m_basicProfileAudiences.find(
            call.serviceName.toUri(ndn::name::UriFormat::CANONICAL));
        if (audience == m_basicProfileAudiences.end() ||
            std::none_of(audience->second.begin(), audience->second.end(),
                [&](const auto& member) { return member.producer == producer; })) return false;
        if (serial == 0) return true;
        const auto ticket = call.readerObservations.find(producer);
        return ticket != call.readerObservations.end() && ticket->second == serial;
    }

    void ServiceUser::releaseBasicReaderObservation(const ndn::Name& requestID,
        const ndn::Name& producer, uint64_t serial)
    {
        const auto pending = m_pendingCalls.find(requestID);
        if (pending == m_pendingCalls.end()) return;
        const auto ticket = pending->second.readerObservations.find(producer);
        if (ticket != pending->second.readerObservations.end() && ticket->second == serial)
            pending->second.readerObservations.erase(ticket);
    }

    uint64_t ServiceUser::beginBasicReaderObservation(const ndn::Name& requestID,
        const ndn::Name& producer)
    {
        if (!isBasicReaderObservationCurrent(requestID, producer, 0)) return 0;
        auto& call = m_pendingCalls.at(requestID);
        const auto known = m_profileReaderObservations.find({producer, call.serviceName});
        if (known != m_profileReaderObservations.end() &&
            known->second.policyEpoch == getCurrentPolicyEpoch(call.serviceName) &&
            known->second.expiresAtMs > nowMilliseconds()) return 0;
        size_t inFlight = 0;
        for (const auto& [id, other] : m_pendingCalls) {
            inFlight += other.readerObservations.size();
            if (other.serviceName == call.serviceName && other.readerObservations.count(producer)) return 0;
        }
        if (inFlight >= 64 || call.readerObservationSerial == std::numeric_limits<uint64_t>::max()) return 0;
        const auto serial = ++call.readerObservationSerial;
        call.readerObservations.emplace(producer, serial);
        try {
            m_scheduler.schedule(ndn::time::milliseconds(5000),
                [this, requestID, producer, serial] {
                    releaseBasicReaderObservation(requestID, producer, serial);
                });
        }
        catch (...) {
            releaseBasicReaderObservation(requestID, producer, serial);
            throw;
        }
        return serial;
    }

    bool ServiceUser::canUseBasicProfileReference(const ndn::Name& service) const
    {
        const auto epoch = getCurrentPolicyEpoch(service);
        if (epoch == 0) return false;
        const auto now = nowMilliseconds();
        const auto serviceUri = service.toUri(ndn::name::UriFormat::CANONICAL);
        const auto audience = m_basicProfileAudiences.find(serviceUri);
        if (audience == m_basicProfileAudiences.end() || audience->second.empty()) return false;
        const auto permissions = m_authorizations.snapshot();
        bool authorizedAudience = false;
        for (const auto& permission : permissions) {
            if (permission.permissionKind != tlv::UserPermission || permission.serviceName != serviceUri) continue;
            authorizedAudience = true;
            if (permission.policyEpoch != epoch || std::none_of(audience->second.begin(), audience->second.end(),
                    [&](const auto& member) { return member.permissionKey == permission.providerServiceName; })) return false;
        }
        for (const auto& member : audience->second) {
            if (std::none_of(permissions.begin(), permissions.end(), [&](const auto& permission) {
                    return permission.permissionKind == tlv::UserPermission && permission.serviceName == serviceUri &&
                           permission.providerServiceName == member.permissionKey && permission.policyEpoch == epoch;
                })) return false;
            const auto observed = m_profileReaderObservations.find(std::make_pair(member.producer, service));
            if (observed == m_profileReaderObservations.end() ||
                observed->second.policyEpoch != epoch || observed->second.service != service ||
                observed->second.producer != member.producer || observed->second.expiresAtMs <= now) return false;
        }
        return authorizedAudience;
    }

    bool ServiceUser::prepareBasicProfileReference(RequestMessage& message, const ndn::Name& service)
    {
        if (!m_useTokens || !isRsaCertificate(identityCert) || !canUseBasicProfileReference(service)) return false;
        const auto record = ensurePublishedSecurityProfile();
        RequestCapabilities capabilities;
        if (message.hasRequestCapabilities()) {
            capabilities.setVersion(message.getRequestCapabilities().getVersion());
            for (const auto& [field, value] : message.getRequestCapabilities().getFields())
                if (field != "SecurityProfileReferenceV1") capabilities.setField(field, value);
        }
        message.setRequestCapabilities(capabilities);
        message.setUserSecurityProfile(record->packet->getName(),
            std::shared_ptr<const EncryptionCertificateAdvertisement>(record, &record->advertisement));
        return true;
    }

    void ServiceUser::PublishRequestV2(const std::vector<ndn::Name>& serviceProviderNames,
                                       const ndn::Name& serviceName,
                                       const ndn::Name& requestId,
                                       const ndn::Buffer& payload,
                                       const size_t& strategy)
    {
        NDN_LOG_DEBUG("PublishRequestV2: " << serviceName << requestId);

        ndn_service_framework::RequestMessage requestMessage;
        auto pendingIt = m_pendingCalls.find(requestId);
        if (pendingIt != m_pendingCalls.end()) {
            requestMessage = pendingIt->second.requestMessage;
        }
        if (m_useTokens && requestMessage.getUserToken().empty()) {
            requestMessage.setUserToken(makeOneTimeToken());
        }
        if (pendingIt == m_pendingCalls.end() ||
            !pendingIt->second.requestScopedConfidentiality) {
            requestMessage.setPayload(const_cast<ndn::Buffer&>(payload), payload.size());
        }
        else {
            // Request-scoped input is published under an exact User-owned
            // Data name after Selection.  Keep the sanitized discovery
            // envelope already stored in the PendingCall; never put the
            // plaintext API payload back on the wire.
        }
        if (detail::hasProtectedTargetedCarrier(requestMessage)) {
            const auto advertisement = makeEncryptionCertificateAdvertisement(identityCert);
            if (requestMessage.getStrategy() != strategy ||
                requestMessage.getPolicyEpoch() != getCurrentPolicyEpoch(serviceName) ||
                !requestMessage.hasUserEncryptionCertificate() ||
                requestMessage.getUserEncryptionCertificate().certificateName != advertisement.certificateName ||
                requestMessage.getUserEncryptionCertificate().certificateDigest != advertisement.certificateDigest) {
                throw std::runtime_error("Protected Targeted authority changed after key binding");
            }
        }
        else {
            requestMessage.setStrategy(strategy);
            requestMessage.setPolicyEpoch(getCurrentPolicyEpoch(serviceName));
            // Bootstrap/discovery requests advertise the User certificate;
            // selected recipients receive fresh keys in their Selection.
            requestMessage.setUserEncryptionCertificate(
                makeEncryptionCertificateAdvertisement(identityCert));
            if (m_useTokens && isRsaCertificate(identityCert)) {
                auto capabilities = requestMessage.hasRequestCapabilities() ?
                    requestMessage.getRequestCapabilities() : RequestCapabilities{};
                capabilities.setField("SecurityProfileReferenceV1", "1");
                requestMessage.setRequestCapabilities(capabilities);
                prepareBasicProfileReference(requestMessage, serviceName);
            }
        }
        requestMessage.WireEncode().data();

        ndn::Name requestName =
            ndn_service_framework::makeRequestNameV2(identity,
                                                     serviceName,
                                                     requestId);
        ndn::Name requestNameWithoutPrefix =
            ndn_service_framework::makeRequestNameWithoutPrefixV2(
                serviceName,
                requestId);

        NDN_LOG_DEBUG("[ServiceUser] selected providerName(s)=");
        if (serviceProviderNames.empty()) {
            NDN_LOG_DEBUG("<discovery>");
        }
        else {
            for (size_t i = 0; i < serviceProviderNames.size(); ++i) {
                if (i != 0) {
                    NDN_LOG_DEBUG(",");
                }
                NDN_LOG_DEBUG(serviceProviderNames[i].toUri());
            }
        }
        NDN_LOG_DEBUG(" selected serviceName=" << serviceName.toUri()
                  << " final request name=" << requestName.toUri()
                  << " userToken=" << requestMessage.getUserToken());
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_PUBLISHED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " requestName=" << requestName.toUri());
        NDN_LOG_DEBUG("PublishRequestV2 selected serviceName=" << serviceName.toUri()
                     << " final request name=" << requestName.toUri());

        pendingIt = m_pendingCalls.find(requestId);
        if (pendingIt != m_pendingCalls.end()) {
            pendingIt->second.providers = serviceProviderNames;
            pendingIt->second.serviceName = serviceName;
            pendingIt->second.requestName = requestName;
            pendingIt->second.requestNameWithoutPrefix = requestNameWithoutPrefix;
            pendingIt->second.requestMessage = requestMessage;
            pendingIt->second.strategy = strategy;
            pendingIt->second.publishedAtUs = nowMicroseconds();
            pendingIt->second.responseRetryEnabled =
                m_responseRetryOptions.enabled &&
                strategy == ndn_service_framework::tlv::FirstResponding &&
                !pendingIt->second.targetedMode &&
                !pendingIt->second.isCollaboration &&
                !pendingIt->second.acksHandler &&
                !pendingIt->second.ackCandidatesHandler;
            pendingIt->second.responseAttemptTimeoutMs =
                m_responseRetryOptions.attemptTimeoutMs;
            pendingIt->second.responseMaxAttempts =
                m_responseRetryOptions.maxAttempts;
        }
        updateRequestLifecycleState(requestId, RequestLifecycleState::REQUEST_PUBLISHED);

        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_PUBLISH_BEGIN timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " strategy=" << strategy
                  << " providerCount=" << serviceProviderNames.size());
        if (m_timelineTrace) {
            logTimelineTrace("user", "request_publish_start", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"strategy", std::to_string(strategy)}});
        }
        if (m_requestPublisher) {
            m_requestPublisher(requestId,
                               requestName,
                               serviceProviderNames,
                               serviceName,
                               requestMessage,
                               strategy);
        }
        else {
            PublishMessage(requestName, requestNameWithoutPrefix, requestMessage);
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_PUBLISH_RETURN timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri());
        if (m_timelineTrace) {
            logTimelineTrace("user", "request_publish_done", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"strategy", std::to_string(strategy)},
                              {"attempt", pendingIt != m_pendingCalls.end() &&
                                  pendingIt->second.streamOptions &&
                                  pendingIt->second.streamOptions->attemptEpoch != 0 ?
                                std::to_string(pendingIt->second.streamOptions->attemptEpoch) :
                                "request"}});
        }

        m_strategyMap.emplace(requestId, strategy);

        if (strategy == tlv::RandomSelection){
            m_AckInfoMap[requestId] = std::vector<ndn_service_framework::AckInfo>();

            m_scheduler.schedule(100_ms,[this, requestId](){
                auto ackInfoVec = m_AckInfoMap.find(requestId);
                if (ackInfoVec == m_AckInfoMap.end()){
                    NDN_LOG_ERROR("AckInfo vector not found for RequestID: " << requestId.toUri());
                    return;
                }

                if (ackInfoVec->second.size() == 0){
                    NDN_LOG_ERROR("After waiting for 100 ms, No AckInfo found for RequestID: " << requestId.toUri());
                    NDN_LOG_INFO("Change strategy of "<< requestId<< " to FirstResponding");
                    m_strategyMap[requestId] = tlv::FirstResponding;
                    m_AckInfoMap.erase(ackInfoVec);
                    return;
                }

                auto randomAckInfo = ackInfoVec->second[rand() % ackInfoVec->second.size()];
                NDN_LOG_INFO("Choosen AckInfo for RandomSelection: "
                             << randomAckInfo.providerName.toUri() << " "
                             << randomAckInfo.requestID.toUri());
                PublishServiceSelectionMessageV2(
                    randomAckInfo.providerName,
                    randomAckInfo.serviceName,
                    randomAckInfo.requestID);
            });
        }
    }

    PreparedServiceRequest ServiceUser::prepareServiceRequest(const std::string& serviceName)
    {
        PreparedServiceRequest ctx;
        ctx.serviceName = ndn::Name(serviceName);
        ctx.requestId = makeRequestId();
        return ctx;
    }

    LargeDataPublishResult ServiceUser::publishEncryptedLargeData(
        const PreparedServiceRequest& ctx,
        const std::vector<uint8_t>& plaintext,
        const std::string& objectLabel,
        ndn::time::milliseconds freshness,
        bool retainWhileLeased,
        const std::function<void()>& requireActive)
    {
        return publishEncryptedLargeDataImpl(ctx, plaintext, objectLabel, freshness,
                                              retainWhileLeased, requireActive,
                                              LargeDataPublishOptions{}, false);
    }

    LargeDataPublishResult ServiceUser::publishEncryptedLargeData(
        const PreparedServiceRequest& ctx,
        const std::vector<uint8_t>& plaintext,
        const std::string& objectLabel,
        ndn::time::milliseconds freshness,
        bool retainWhileLeased,
        const LargeDataPublishOptions& options,
        const std::function<void()>& requireActive)
    {
        return publishEncryptedLargeDataImpl(ctx, plaintext, objectLabel, freshness,
                                              retainWhileLeased, requireActive,
                                              options, false);
    }

    LargeDataPublishResult ServiceUser::publishEncryptedLargeDataFromWorker(
        const PreparedServiceRequest& ctx, const std::vector<uint8_t>& plaintext,
        const std::string& objectLabel, ndn::time::milliseconds freshness,
        const std::function<void()>& requireActive)
    {
        if (isOnIoThread())
            throw std::logic_error("large-data worker publication cannot run on Face I/O");
        return publishEncryptedLargeDataImpl(ctx, plaintext, objectLabel, freshness,
                                              true, requireActive,
                                              LargeDataPublishOptions{}, true);
    }

    LargeDataPublishResult ServiceUser::publishEncryptedLargeDataFromWorker(
        const PreparedServiceRequest& ctx, const std::vector<uint8_t>& plaintext,
        const std::string& objectLabel, ndn::time::milliseconds freshness,
        const LargeDataPublishOptions& options,
        const std::function<void()>& requireActive)
    {
        if (isOnIoThread())
            throw std::logic_error("large-data worker publication cannot run on Face I/O");
        return publishEncryptedLargeDataImpl(ctx, plaintext, objectLabel, freshness,
                                              true, requireActive, options, true);
    }

    LargeDataPublishResult ServiceUser::publishEncryptedLargeDataImpl(
        const PreparedServiceRequest& ctx, const std::vector<uint8_t>& plaintext,
        const std::string& objectLabel, ndn::time::milliseconds freshness,
        bool retainWhileLeased, const std::function<void()>& requireActive,
        const LargeDataPublishOptions& options, bool marshalIo)
    {
        LargeDataPublishResult result;
        enum PublicationPhase : std::size_t {
          TOTAL, DURABLE_LOOKUP, KEY_STATE, KEY_WRAP, ENCRYPT, MANIFEST,
          ORIGINAL_PACKETS, STORE_COMMIT, KEY_REFERENCE, IO_DISPATCH_WAIT,
          IO_CALLBACK_WORK, PHASE_COUNT
        };
        struct PublicationSample
        {
          std::uint64_t durationUs = 0;
          std::uint64_t cpuUs = 0;
          std::uint64_t calls = 0;
          bool cpuAvailable = true;
        };
        const bool measurePublication = phaseTimingEnvEnabled();
        const bool publicationCallerOnIo = measurePublication && isOnIoThread();
        static std::atomic<std::uint64_t> publicationSequence{0};
        const auto publicationCall = measurePublication
          ? publicationSequence.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        std::array<PublicationSample, PHASE_COUNT> samples{};
        const auto threadCpu = []() -> std::optional<std::uint64_t> {
#ifdef CLOCK_THREAD_CPUTIME_ID
          timespec value{};
          if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0 && value.tv_sec >= 0 &&
              value.tv_nsec >= 0 && value.tv_nsec < 1000000000L &&
              static_cast<std::uint64_t>(value.tv_sec) <=
                std::numeric_limits<std::uint64_t>::max() / 1000000)
            return static_cast<std::uint64_t>(value.tv_sec) * 1000000 + value.tv_nsec / 1000;
#endif
          return std::nullopt;
        };
        const auto measure = [&](PublicationPhase phase) {
          const auto started = measurePublication ? timelineSteadyMicroseconds() : 0;
          const auto cpuStarted = measurePublication ? threadCpu() : std::nullopt;
          return ndn::make_scope_exit([&, phase, started, cpuStarted] {
            if (!measurePublication) return;
            const auto cpuEnded = threadCpu();
            auto& sample = samples[phase];
            sample.durationUs += timelineSteadyMicroseconds() - started;
            ++sample.calls;
            if (cpuStarted && cpuEnded && *cpuEnded >= *cpuStarted)
              sample.cpuUs += *cpuEnded - *cpuStarted;
            else
              sample.cpuAvailable = false;
          });
        };
        const auto measureCall = [&](PublicationPhase phase, auto&& work) -> decltype(auto) {
          auto timing = measure(phase);
          return work();
        };
        // Aggregate per object, never per packet. The synchronous I/O handoff
        // below drains its callback before these stack-owned measurements end.
        auto reportPublication = ndn::make_scope_exit([&] {
          if (!measurePublication) return;
          try {
            constexpr std::array<const char*, PHASE_COUNT> names = {
              "total", "durable_lookup_reuse", "key_state", "key_wrap", "encrypt",
              "manifest", "original_packets", "store_commit", "key_reference",
              "io_dispatch_wait", "io_callback_work"};
            for (std::size_t i = 0; i < samples.size(); ++i) {
              const auto& sample = samples[i];
              if (sample.calls == 0) continue;
              logPhaseTiming("user-publication", "publicationMeasure", ctx.requestId,
                {{"publicationCall", std::to_string(publicationCall)},
                 {"step", names[i]}, {"calls", std::to_string(sample.calls)},
                 {"duration_us", std::to_string(sample.durationUs)},
                 {"measured_thread_cpu_us", sample.cpuAvailable
                    ? std::to_string(sample.cpuUs) : "NA"},
                 {"cpu_scope", i == IO_DISPATCH_WAIT ? "unavailable" :
                    i == IO_CALLBACK_WORK ? (marshalIo && !publicationCallerOnIo
                      ? "face_callback" : "caller_callback") : "caller"},
                 {"marshal_io", marshalIo ? "1" : "0"},
                 {"caller_on_io", publicationCallerOnIo ? "1" : "0"},
                 {"plaintextBytes", std::to_string(plaintext.size())},
                 {"success", result.success ? "1" : "0"},
                 {"durable", options.retention == EncryptedLargeDataRetention::Durable ? "1" : "0"}});
            }
          }
          catch (...) {
            // Observation must not replace a publication result or exception.
          }
        });
        auto totalPublication = measure(TOTAL);
        const auto onIo = [this, marshalIo, &measure, &samples, measurePublication]
                          (std::function<void()> work) {
            if (!marshalIo || isOnIoThread()) {
                auto callbackTiming = measure(IO_CALLBACK_WORK);
                work();
                return;
            }
            const auto queuedAt = measurePublication ? timelineSteadyMicroseconds() : 0;
            auto task = std::make_shared<std::packaged_task<void()>>(
              [&, work = std::move(work), queuedAt] {
                if (measurePublication) {
                  auto& sample = samples[IO_DISPATCH_WAIT];
                  sample.durationUs += timelineSteadyMicroseconds() - queuedAt;
                  ++sample.calls;
                  sample.cpuAvailable = false;
                }
                auto callbackTiming = measure(IO_CALLBACK_WORK);
                work();
              });
            auto completed = task->get_future();
            postToIo([task] { (*task)(); });
            // Never abandon a callback capturing local state. The caller's
            // owner/drain barrier keeps I/O alive until this call settles.
            completed.get();
        };
        try { ProtectedBlobManifest::validateSuffix(options.manifestSuffix); }
        catch (const std::exception& error) { result.errorMessage = error.what(); return result; }
        if (ctx.serviceName.empty()) {
            result.errorMessage = "PreparedServiceRequest serviceName is empty";
            return result;
        }
        if (ctx.requestId.empty()) {
            result.errorMessage = "PreparedServiceRequest requestId is empty";
            return result;
        }

        result.objectId = sanitizeLargeDataObjectId(objectLabel);
        if (result.objectId.empty()) {
            if (options.retention == EncryptedLargeDataRetention::Durable) {
                result.errorMessage =
                    "durable large-data publication requires a stable object label";
                return result;
            }
            result.objectId = "object-" + RandomString(16);
        }

        const bool durable = options.retention == EncryptedLargeDataRetention::Durable;
        if (durable && (options.publicationIdentity.size() != 71 ||
                        options.publicationIdentity.compare(0, 7, "sha256:") != 0)) {
            result.errorMessage = "durable large-data publication identity is invalid";
            return result;
        }
        PublishedContentName contentName;
        std::string durablePublicationBase;
        try {
            if (options.contentName) contentName = *options.contentName;
            else contentName.label = objectLabel; // Explicit generic Core publication label.
            contentName.encode(); // Validate full semantic metadata before crypto/backend effects.
            if (durable) {
                const auto ownerDigest = makeNamingDigestComponent(options.publicationIdentity.substr(7));
                const auto semanticDigest = makeNamingDigestComponent(contentName.objectIdentity());
                const auto& suffixWire = options.manifestSuffix.wireEncode();
                durablePublicationBase = "sha256:" + computeNamingDigest("ndnsf-naming/publication",
                    {{1, 4, {ownerDigest.value(), ownerDigest.value() + ownerDigest.value_size()}},
                     {2, 4, {semanticDigest.value(), semanticDigest.value() + semanticDigest.value_size()}},
                     {3, 1, {suffixWire.begin(), suffixWire.end()}}});
            }
        }
        catch (const std::exception& error) { result.errorMessage = error.what(); return result; }
        std::string durablePublicationIdentity = durablePublicationBase;
        ndn::Name encryptedDataName;
        ProtectedBlobIdentity blobIdentity;
        static constexpr std::size_t maxSegmentBytes = 7000;
        const bool activePut =
            !isTruthyEnv("NDNSF_REQUEST_LARGE_DISABLE_ACTIVE_PUT");
        // A large object is served from a private wire file once it reaches
        // the bounded streaming threshold.  The explicit environment switch
        // is retained for fixtures and operators that need to exercise the
        // file path with a smaller object.
        static constexpr std::size_t fileBackedThresholdBytes = 1 * 1024 * 1024;
        std::shared_ptr<EncryptedLargeDataRangeStore> rangeStore;
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            rangeStore = m_largeDataRangeStore;
        }
        if (durable && (!rangeStore || !rangeStore->supportsDurableRetention())) {
            result.errorMessage = "DURABLE_RETENTION_UNSUPPORTED";
            return result;
        }
        if (durable && !rangeStore->supportsOriginalDataPackets()) {
            result.errorMessage = "ORIGINAL_DATA_PACKET_UNSUPPORTED";
            return result;
        }
        const bool fileBackedRequested = rangeStore || retainWhileLeased ||
            useFileBackedLargeData() || plaintext.size() >= fileBackedThresholdBytes;
        const auto fileBackedRetention = fileBackedRequested ?
            std::optional<ndn::time::milliseconds>(largeDataFileRetention()) :
            std::nullopt;
        if (activePut && !fileBackedRequested &&
            (m_IMS.getLimit() == 0 ||
             plaintext.size() > (m_IMS.getLimit() - 1) * maxSegmentBytes)) {
            result.errorMessage =
                "large-data publication exceeds transactional IMS staging capacity";
            return result;
        }
        const std::vector<std::string> attributes = {
            makeAbeServiceAttribute(ctx.serviceName, "SERVICE")
        };

        result.plaintextSize = plaintext.size();
        result.contentDigest = sha256DigestString(
            ndn::span<const uint8_t>(plaintext.data(), plaintext.size()), requireActive);
        result.authorizationScope = attributes.front();

        // Protected durable reuse is versioned by the service-scoped signed
        // status.  LocalMock fixtures are the sole explicit compatibility
        // exception: they may use an unversioned (0,0) snapshot, but a
        // configured runtime with no status must fail closed.
        std::optional<ControllerVersion> durableControllerVersion;
        bool durableVersioned = false;
        if (durable) {
            std::shared_lock<std::shared_mutex> fence(m_protectedReuseMutex);
            {
                std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
                const auto statusIt = m_revocationStates.find(ctx.serviceName.toUri());
                if (statusIt != m_revocationStates.end() &&
                    statusIt->second.hasCurrentStatus()) {
                    durableControllerVersion = statusIt->second.currentVersion();
                    durableVersioned = true;
                }
            }
            if (!durableVersioned && !m_isLocalMock) {
                result.errorMessage = "PROTECTED_CONTROLLER_VERSION_UNAVAILABLE";
                return result;
            }
        }
        const auto currentDurableVersion = [&]() -> std::optional<ControllerVersion> {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            const auto statusIt = m_revocationStates.find(ctx.serviceName.toUri());
            if (statusIt != m_revocationStates.end() &&
                statusIt->second.hasCurrentStatus()) {
                return statusIt->second.currentVersion();
            }
            return std::nullopt;
        };
        const auto durableVersionMatches = [&]() {
            const auto current = currentDurableVersion();
            return durableVersioned == current.has_value() &&
                (!durableVersioned ||
                 current->compare(*durableControllerVersion) == 0);
        };

        bool wrappedKeyReferenceHeld = false;
        std::string wrappedKeyId;
        try {
            const auto messageType = std::string("REQUEST-LARGE");
            if (requireActive) requireActive();
            const auto accessAttribute = makeAbeServiceAttribute(ctx.serviceName, "SERVICE");
            std::optional<EncryptedLargeDataLookupResult> durableHit;
            std::optional<DurableLargeDataKeyReference> durableReference;
            bool migrateLegacyReference = false;
            bool repairReferenceFromRepo = false;
            if (durable) {
                auto lookupTiming = measure(DURABLE_LOOKUP);
                const auto referenceVersionMatches = [&]() {
                    if (!durableReference) {
                        return false;
                    }
                    return durableVersioned
                        ? durableReference->controllerGenerationTimestamp ==
                              durableControllerVersion->controllerGenerationTimestamp &&
                          durableReference->policyEpoch ==
                              durableControllerVersion->controllerEpoch
                        : durableReference->controllerGenerationTimestamp == 0 &&
                          durableReference->policyEpoch == 0;
                };
                const auto referenceMetadataMatches = [&]() {
                    return durableReference &&
                        ndn::Name(durableReference->serviceName) == ctx.serviceName &&
                        ndn::Name(durableReference->encryptedDataName) == encryptedDataName &&
                        durableReference->contentDigest == result.contentDigest &&
                        durableReference->plaintextSize == result.plaintextSize &&
                        durableReference->protectionEpoch == durableHit->protectionEpoch &&
                        durableReference->keyReferenceId == durableHit->keyReferenceId &&
                        durableReference->keyReferenceVersion ==
                            durableHit->keyReferenceVersion &&
                        durableReference->ciphertextManifestDigest ==
                            durableHit->ciphertextManifestDigest &&
                        ndn::Name(durableReference->servingLocator) == encryptedDataName;
                };
                const auto rebindDurableIdentity = [&] {
                    const auto versionText = durableVersioned
                        ? durableControllerVersion->toString() : "unversioned";
                    const auto seed = durablePublicationBase +
                        "|ndnsf-protected-rebind-v2|" + versionText;
                    durablePublicationIdentity = sha256DigestString(ndn::Buffer(
                        reinterpret_cast<const uint8_t*>(seed.data()), seed.size()));
                    durableHit = rangeStore->lookupDurable(
                        durablePublicationIdentity, requireActive);
                    if (durableHit) {
                        encryptedDataName = ndn::Name(durableHit->encryptedName);
                        if (!isProtectedBlobDataPrefix(encryptedDataName, identity, result.contentDigest, contentName))
                            throw std::runtime_error("DURABLE_LOOKUP_NAME_MISMATCH");
                        checkNamingNameSize(encryptedDataName);
                    }
                    durableReference = durableHit ? loadDurableKeyReference(
                        identity, durablePublicationIdentity) : std::nullopt;
                    migrateLegacyReference = false;
                    repairReferenceFromRepo = false;
                    if (durableHit) {
                        // The derived identity itself binds the current
                        // service version.  Repo metadata is sufficient to
                        // reconstruct the non-secret V2 reference after a
                        // crash between durable commit and reference rename;
                        // no key or plaintext is recovered from Repo.
                        durableReference = DurableLargeDataKeyReference{
                            durablePublicationIdentity,
                            ctx.serviceName.toUri(),
                            durableHit->encryptedName,
                            result.contentDigest,
                            result.plaintextSize,
                            durableVersioned ? durableControllerVersion->controllerEpoch : 0,
                            durableVersioned ?
                                durableControllerVersion->controllerGenerationTimestamp : 0,
                            durableHit->protectionEpoch,
                            durableHit->keyReferenceId,
                            durableHit->keyReferenceVersion,
                            durableHit->ciphertextManifestDigest,
                            durableHit->servingLocator,
                            durableHit->manifestName, durableHit->packetStorageFormat};
                        repairReferenceFromRepo = true;
                    }
                };
                durableHit = rangeStore->lookupDurable(
                    durablePublicationIdentity, requireActive);
                if (durableHit) {
                    encryptedDataName = ndn::Name(durableHit->encryptedName);
                    if (!isProtectedBlobDataPrefix(encryptedDataName, identity, result.contentDigest, contentName))
                        throw std::runtime_error("DURABLE_LOOKUP_NAME_MISMATCH");
                    checkNamingNameSize(encryptedDataName);
                    durableReference = loadDurableKeyReference(
                        identity, durablePublicationIdentity);
                    if (!durableReference && m_isLocalMock) {
                        durableReference = loadLegacyDurableKeyReference(
                            identity, durablePublicationIdentity);
                        migrateLegacyReference = durableReference.has_value();
                    }
                    if (migrateLegacyReference && !referenceMetadataMatches()) {
                        migrateLegacyReference = false;
                    }
                    // A missing/old reference cannot authorize the existing
                    // ciphertext.  Rebind to a version-derived immutable
                    // publication identity so a new protected publication
                    // can be committed without deleting the old Repo object.
                    if ((!durableReference || !referenceVersionMatches() ||
                         !referenceMetadataMatches()) &&
                        !migrateLegacyReference) {
                        rebindDurableIdentity();
                    }
                }
                if (durableHit) {
                    if (!durableReference ||
                        ndn::Name(durableReference->serviceName) != ctx.serviceName ||
                        ndn::Name(durableReference->encryptedDataName) != encryptedDataName ||
                        durableReference->contentDigest != result.contentDigest ||
                        durableReference->plaintextSize != result.plaintextSize ||
                        (durableVersioned &&
                         (durableReference->controllerGenerationTimestamp !=
                              durableControllerVersion->controllerGenerationTimestamp ||
                          durableReference->policyEpoch !=
                              durableControllerVersion->controllerEpoch)) ||
                        (!durableVersioned &&
                         (durableReference->controllerGenerationTimestamp != 0 ||
                          durableReference->policyEpoch != 0)) ||
                        durableReference->protectionEpoch != durableHit->protectionEpoch ||
                        durableReference->keyReferenceId != durableHit->keyReferenceId ||
                        durableReference->keyReferenceVersion != durableHit->keyReferenceVersion ||
                        durableReference->ciphertextManifestDigest !=
                            durableHit->ciphertextManifestDigest ||
                        ndn::Name(durableReference->servingLocator) != encryptedDataName ||
                        !durableHit->source || !durableHit->source->isDurable() ||
                        ndn::Name(durableHit->encryptedName) != encryptedDataName ||
                        durableHit->publicationIdentity != durablePublicationIdentity ||
                        durableHit->contentDigest != result.contentDigest ||
                        durableHit->plaintextSize != result.plaintextSize ||
                        durableHit->keyReferenceVersion.empty() ||
                        durableHit->ciphertextManifestDigest.empty() ||
                        ndn::Name(durableHit->servingLocator) != encryptedDataName ||
                        durableHit->source->size() == 0) {
                        throw std::runtime_error("DURABLE_LOOKUP_METADATA_MISMATCH");
                    }
                    if (durableHit->packetStorageFormat != "original-packet-set" ||
                        durableHit->dataPrefix.empty() || durableHit->manifestName.empty() ||
                        ndn::Name(durableHit->dataPrefix) != encryptedDataName)
                        throw std::runtime_error("DURABLE_ORIGINAL_PACKET_METADATA_REQUIRED");
                    const ndn::Name manifestName(durableHit->manifestName);
                    if (durableReference->packetStorageFormat == "original-packet-set" &&
                        ndn::Name(durableReference->manifestName) != manifestName)
                        throw std::runtime_error("DURABLE_REFERENCE_MANIFEST_MISMATCH");
                    const auto rootWire = durableHit->source->readOriginalDataPacket(manifestName);
                    if (rootWire.empty() || rootWire.size() > ndn::MAX_NDN_PACKET_SIZE)
                        throw std::runtime_error("DURABLE_LOOKUP_UNREADABLE");
                    const auto restoredRoot = std::make_shared<ndn::Data>(ndn::Block(
                        ndn::span<const std::uint8_t>(rootWire.data(), rootWire.size())));
                    if (restoredRoot->getName() != manifestName)
                        throw std::runtime_error("DURABLE_MANIFEST_NAME_MISMATCH");
                    const auto& rootContent = restoredRoot->getContent();
                    const auto restoredManifest = ProtectedBlobManifest::decode(
                        std::vector<std::uint8_t>(rootContent.value(), rootContent.value() + rootContent.value_size()),
                        identity, manifestName);
                    if (!restoredManifest.contentName ||
                        restoredManifest.contentName->encode() != contentName.encode() ||
                        restoredManifest.manifestSuffix != options.manifestSuffix ||
                        ndn::Name(restoredManifest.authorizationScope) !=
                            ndn::Name("/SERVICE").append(ctx.serviceName) ||
                        restoredManifest.dataPrefix != encryptedDataName ||
                        restoredManifest.envelopeBytes != durableHit->source->size() ||
                        restoredManifest.plaintextBytes != result.plaintextSize ||
                        restoredManifest.keyReferenceId != durableHit->keyReferenceId ||
                        restoredManifest.protectionEpoch != durableHit->protectionEpoch)
                        throw std::runtime_error("DURABLE_MANIFEST_METADATA_MISMATCH");
                    const auto digestComponent = makeNamingDigestComponent(result.contentDigest.substr(7));
                    if (ndn::name::Component(ndn::span<const std::uint8_t>(
                            restoredManifest.plaintextDigest.data(), restoredManifest.plaintextDigest.size())) != digestComponent)
                        throw std::runtime_error("DURABLE_MANIFEST_CONTENT_MISMATCH");

                    encryptedDataName = ndn::Name(durableHit->encryptedName);
                    result.encryptedDataName = encryptedDataName;
                    result.dataPrefix = encryptedDataName;
                    result.manifestName = manifestName;
                    result.publicationIdentity = durableHit->publicationIdentity;
                    result.protectionEpoch = durableHit->protectionEpoch;
                    result.keyReferenceId = durableHit->keyReferenceId;
                    result.keyReferenceVersion = durableHit->keyReferenceVersion;
                    result.manifestDigest = durableHit->ciphertextManifestDigest;
                    result.ciphertextManifestDigest = durableHit->ciphertextManifestDigest;
                    result.servingLocator = durableHit->servingLocator;
                    result.fileBacked = true;

                    const auto publicationKey = encryptedDataName.toUri();
                    if (migrateLegacyReference || repairReferenceFromRepo ||
                        durableReference->packetStorageFormat != "original-packet-set" ||
                        durableReference->needsLocationMigration) {
                        std::lock_guard<std::mutex> commitFence(
                            m_protectedReferenceCommitMutex);
                        {
                            std::unique_lock<std::shared_mutex> fence(
                                m_protectedReuseMutex);
                            if (!durableVersionMatches()) {
                                throw std::runtime_error(
                                    "DURABLE_STALE_CONTROLLER_VERSION");
                            }
                        }
                        // Preserve the already-validated Controller version,
                        // including when upgrading a versioned V2 record.
                        auto migratedReference = *durableReference;
                        migratedReference.manifestName = manifestName.toUri(ndn::name::UriFormat::CANONICAL);
                        migratedReference.packetStorageFormat = "original-packet-set";
                        if (!persistDurableKeyReference(identity, migratedReference,
                                                        repairReferenceFromRepo)) {
                            throw std::runtime_error(
                                "DURABLE_KEY_REFERENCE_STORE_UNAVAILABLE");
                        }
                    }
                    std::shared_ptr<LargeDataFilePublication> publication;
                    bool scheduleExpiry = false;
                    {
                        std::unique_lock<std::shared_mutex> fence(m_protectedReuseMutex);
                        if (!durableVersionMatches()) {
                            throw std::runtime_error(
                                "DURABLE_STALE_CONTROLLER_VERSION");
                        }
                        std::lock_guard<std::mutex> lock(_cache_mutex);
                        const auto existing = m_largeDataFiles.find(publicationKey);
                        if (existing != m_largeDataFiles.end()) {
                            publication = existing->second;
                            if (retainWhileLeased) {
                                auto lease = publication->servingLease.lock();
                                if (!lease) {
                                    result.servingLease = std::make_shared<unsigned char>(0);
                                    publication->servingLease = result.servingLease;
                                }
                                else {
                                    result.servingLease = std::move(lease);
                                }
                            }
                        }
                        else {
                            publication = std::make_shared<LargeDataFilePublication>();
                            publication->baseName = encryptedDataName;
                            publication->fileSize = durableHit->source->size();
                            publication->segmentCount = 1 +
                                (publication->fileSize - 1) / maxSegmentBytes;
                            publication->maxSegmentBytes = maxSegmentBytes;
                            publication->freshness = freshness;
                            publication->windowCapacity = largeDataWindowSegments();
                            publication->rangeSource = durableHit->source;
                            publication->originalPacketSource = true;
                            publication->manifestPacket = restoredRoot;
                            publication->segmentCount = restoredManifest.segmentCount;
                            publication->maxSegmentBytes = restoredManifest.segmentBytes;
                            publication->serviceName = ctx.serviceName.toUri();
                            publication->publicationIdentity = result.publicationIdentity;
                            publication->controllerVersioned = durableVersioned;
                            if (durableVersioned)
                                publication->controllerVersion = *durableControllerVersion;
                            m_retiredProtectedPrefixes.erase(publicationKey);
                            if (retainWhileLeased) {
                                result.servingLease = std::make_shared<unsigned char>(0);
                                publication->servingLease = result.servingLease;
                            }
                            const auto [it, inserted] = m_largeDataFiles.emplace(
                                publicationKey, publication);
                            if (!inserted)
                                throw std::runtime_error(
                                    "duplicate durable large-data publication name");
                            (void)it;
                            scheduleExpiry = true;
                        }
                    }
                    if (scheduleExpiry) {
                        const auto expiry = *fileBackedRetention;
                        onIo([&] { m_scheduler.schedule(expiry, [this, publicationKey,
                                                       weakPublication = std::weak_ptr<LargeDataFilePublication>(publication)] {
                            expireLargeDataPublication(publicationKey, weakPublication);
                        }); });
                    }
                    if (publication->manifestPacket)
                        result.manifestName = publication->manifestPacket->getName();
                    NDN_LOG_INFO("LARGE_DATA_PUBLISH_DURABLE_HIT"
                                 << " name=" << result.encryptedDataName.toUri()
                                 << " plaintextBytes=" << result.plaintextSize
                                 << " envelopeBytes=" << durableHit->source->size());
                    result.success = true;
                    return result;
                }
            }

            HybridMessageKey key;
            measureCall(KEY_STATE, [&] { onIo([&] {
                if (!m_largeDataKeyReleaseState) {
                    m_largeDataKeyReleaseState = std::make_shared<LargeDataKeyReleaseState>();
                    m_largeDataKeyReleaseState->crypto = &m_hybridMessageCrypto;
                }
                key = m_hybridMessageCrypto.getOrCreateSendKey(
                    ctx.serviceName, identity, accessAttribute, messageType, m_hybridCryptoCounters);
            }); });
            wrappedKeyId = key.keyId;
            const auto keyReferenceText = key.keyId + "|" + key.epochId + "|" +
                ctx.serviceName.toUri(ndn::name::UriFormat::CANONICAL) + "|" + accessAttribute;
            const auto currentKeyReferenceId = sha256DigestString(ndn::Buffer(
                reinterpret_cast<const uint8_t*>(keyReferenceText.data()),
                keyReferenceText.size()));
            blobIdentity = makeProtectedBlobDataPrefix(
                identity, result.contentDigest, currentKeyReferenceId, key.epochId,
                accessAttribute, durable ? durablePublicationIdentity : std::string{},
                freshness, maxSegmentBytes, contentName);
            encryptedDataName = blobIdentity.dataPrefix;
            result.dataPrefix = encryptedDataName;

            const auto prepareManifest = [&](std::uint64_t encodedBytes) {
                ProtectedBlobManifest manifest;
                manifest.producer = identity;
                manifest.dataPrefix = encryptedDataName;
                const auto digest = makeNamingDigestComponent(result.contentDigest.substr(7));
                manifest.plaintextDigest.assign(digest.value(), digest.value() + digest.value_size());
                manifest.plaintextBytes = result.plaintextSize;
                manifest.envelopeBytes = encodedBytes;
                manifest.segmentBytes = maxSegmentBytes;
                manifest.segmentCount = 1 + (encodedBytes - 1) / maxSegmentBytes;
                manifest.representationSpec = blobIdentity.representationSpec;
                manifest.protectionSpec = blobIdentity.protectionSpec;
                manifest.authorizationScope = accessAttribute;
                manifest.protectionEpoch = key.epochId;
                manifest.keyReferenceId = currentKeyReferenceId;
                manifest.keyIdentifier = key.keyId;
                manifest.manifestSuffix = options.manifestSuffix;
                manifest.contentName = contentName;
                const auto payload = manifest.encode();
                auto packet = std::make_shared<ndn::Data>(manifest.getName());
                packet->setContent(payload);
                packet->setFreshnessPeriod(freshness);
                onIo([&] {
                    auto& signingKeyChain = m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain;
                    signingKeyChain.sign(*packet, m_signingInfo);
                });
                if (packet->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
                    throw ndn::Face::OversizedPacketError('D', packet->getName(), packet->wireEncode().size());
                result.manifestName = packet->getName();
                result.manifestDigest = sha256DigestString(ndn::Buffer(payload.begin(), payload.end()));
                return packet;
            };

            ndn::nacabe::SPtrVector<ndn::Data> wrappedContentData;
            ndn::nacabe::SPtrVector<ndn::Data> wrappedCkData;
            ndn::Buffer wrappedMessageKey;
            bool hasWrappedMessageKey = false;

            // Keep the publication metadata in the native crypto owner.  A
            // DI caller may bind the returned object to its request, but it
            // cannot invent the protection epoch, scope, or manifest digest
            // in Python after this point.
            result.authorizationScope = accessAttribute;
            result.protectionEpoch = key.epochId;
            result.publicationIdentity = durable ? durablePublicationIdentity : std::string{};
            result.keyReferenceVersion = durable ? "v1" : std::string{};
            result.keyReferenceId = durable ? currentKeyReferenceId : std::string{};

            HybridMessageEnvelope envelope;
            envelope.setKeyId(key.keyId);
            envelope.setEpochId(key.epochId);
            envelope.setMessageType(messageType);
            envelope.setNonce(blobIdentity.nonce);

            measureCall(KEY_WRAP, [&] { onIo([&] {
            if (m_hybridMessageCrypto.shouldAttachWrappedKey(key.keyId)) {
                std::tie(wrappedContentData, wrappedCkData) =
                    (m_testNacProducer ? *m_testNacProducer : nacProducer).produce(key.keyName,
                                        attributes,
                                        ndn::span<const uint8_t>(key.key.data(), key.key.size()),
                                        m_signingInfo);
                const auto wrapped = mergeDataContents(wrappedContentData);
                wrappedMessageKey.assign(wrapped.begin(), wrapped.end());
                if (wrappedMessageKey.empty()) {
                    throw std::runtime_error("NAC-ABE produced no wrapped large-data MessageKey");
                }
                wrappedContentData = prepareHybridMessageKeyPackets(ctx.serviceName, identity,
                    accessAttribute, key.epochId,
                    ndn::span<const uint8_t>(wrappedMessageKey.data(), wrappedMessageKey.size()),
                    (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain), m_signingInfo,
                    wrappedContentData.front()->getFreshnessPeriod(), wrappedContentData.front()->getContentType());
                for (const auto& packet : wrappedContentData)
                    if (!validator->isProducerSignerAuthorized(*packet, identity))
                        throw std::invalid_argument("wrapped large-data key signer is not authorized for Producer");
                for (const auto& packet : wrappedCkData)
                    if (!packet || !identity.isPrefixOf(packet->getName()) ||
                        !validator->isProducerSignerAuthorized(*packet, identity) ||
                        packet->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
                        throw std::invalid_argument("original NAC CK signer/namespace/budget rejected");
                m_hybridMessageCrypto.cacheWrappedSendPackets(ctx.serviceName, key.keyId,
                    wrappedMessageKey, wrappedContentData, wrappedCkData);
                wrappedKeyReferenceHeld = true;
                envelope.setWrappedMessageKey(
                    ndn::Buffer(wrappedMessageKey.data(), wrappedMessageKey.size()));
                hasWrappedMessageKey = true;
                if (fileBackedRequested) {

                ++m_hybridCryptoCounters.nac_abe_key_wrap_count;
                }
            }
            else {
                if (!m_hybridMessageCrypto.retainWrappedSendKey(ctx.serviceName, key.keyId))
                    throw std::runtime_error("wrapped large-data key disappeared before publication");
                wrappedKeyReferenceHeld = true;
                if (m_hybridMessageCrypto.getWrappedSendPackets(key.keyId, wrappedContentData, wrappedCkData))
                    serveDataWithIMS(wrappedContentData, wrappedCkData);
            }
            }); });
            wrappedKeyReferenceHeld = true;

            const std::string adText = encryptedDataName.toUri(ndn::name::UriFormat::CANONICAL) + "|" +
                                       messageType + "|" + ctx.serviceName.toUri(ndn::name::UriFormat::CANONICAL);
            const ndn::Buffer ad(reinterpret_cast<const uint8_t*>(adText.data()),
                                 adText.size());
            std::vector<ndn::Name> stagedFullNames;
            std::vector<std::string> protectedImsNames;
            if (fileBackedRequested) {
                const auto directory = largeDataFileDirectory();
                std::error_code directoryError;
                std::filesystem::create_directories(directory, directoryError);
                if (directoryError) {
                    throw std::system_error(directoryError,
                                             "cannot create large-data directory");
                }
                const auto envelopeBudget = static_cast<std::uintmax_t>(plaintext.size()) +
                                            64ULL * 1024ULL;
                const auto packetCountBudget = 1 + (envelopeBudget - 1) / maxSegmentBytes;
                const auto packetRecordBudget = ndn::MAX_NDN_PACKET_SIZE +
                                                2 * sizeof(std::uint64_t);
                if (packetCountBudget >
                    (std::numeric_limits<std::uintmax_t>::max() - envelopeBudget) /
                        packetRecordBudget)
                    throw std::runtime_error("ORIGINAL_PACKET_STORAGE_BUDGET_OVERFLOW");
                const auto packetStorageBudget = packetCountBudget * packetRecordBudget;
                // During preparation both envelope and original signed wire exist.
                // Release the envelope file after packet completion when no backend owns it.
                const auto reservation = envelopeBudget + packetStorageBudget;
                bool reservationHeld = false;
                bool publicationRegistered = false;
                std::filesystem::path filePath;
                std::shared_ptr<LargeDataFilePublication> publication;
                const auto publicationKey = encryptedDataName.toUri();
                std::string pendingPublicationKey;
                auto rollback = [&] {
                    if (!pendingPublicationKey.empty()) {
                        std::lock_guard<std::mutex> commitFence(
                            m_protectedReferenceCommitMutex);
                        std::unique_lock<std::shared_mutex> fence(m_protectedReuseMutex);
                        m_protectedPendingPublications.erase(pendingPublicationKey);
                    }
                    std::shared_ptr<LargeDataFilePublication> removedPublication;
                    {
                        std::lock_guard<std::mutex> lock(_cache_mutex);
                        if (publicationRegistered) {
                            const auto it = m_largeDataFiles.find(publicationKey);
                            if (it != m_largeDataFiles.end() &&
                                it->second == publication) {
                                for (const auto& fullName : publication->protectedImsNames) {
                                    auto owners = m_protectedImsOwners.find(fullName);
                                    if (owners == m_protectedImsOwners.end()) {
                                        continue;
                                    }
                                    owners->second.erase(publicationKey);
                                    if (owners->second.empty()) {
                                        m_protectedImsOwners.erase(owners);
                                        m_IMS.erase(ndn::Name(fullName), false);
                                    }
                                }
                                m_largeDataReservedBytes =
                                    m_largeDataReservedBytes >= it->second->reservedBytes ?
                                        m_largeDataReservedBytes - it->second->reservedBytes : 0;
                                removedPublication = it->second;
                                m_largeDataFiles.erase(it);
                            }
                            publicationRegistered = false;
                        }
                        else if (reservationHeld) {
                            m_largeDataReservedBytes =
                                m_largeDataReservedBytes >= reservation ?
                                    m_largeDataReservedBytes - reservation : 0;
                            reservationHeld = false;
                        }
                    }
                    onIo([&] {
                        std::lock_guard<std::mutex> lock(_cache_mutex);
                        for (const auto& fullName : stagedFullNames) {
                            if (m_protectedImsOwners.find(fullName.toUri()) ==
                                m_protectedImsOwners.end()) {
                                m_IMS.erase(fullName, false);
                            }
                        }
                        stagedFullNames.clear();
                        protectedImsNames.clear();
                    });
                    if (!filePath.empty()) {
                        std::error_code removeError;
                        std::filesystem::remove(filePath, removeError);
                    }
                    // Keep destruction outside _cache_mutex so the stream and
                    // file are released without re-entering the cache path.
                    removedPublication.reset();
                };
                {
                    std::lock_guard<std::mutex> lock(_cache_mutex);
                    std::error_code spaceError;
                    const auto available = std::filesystem::space(directory, spaceError).available;
                    if (spaceError) {
                        throw std::system_error(spaceError,
                                                "cannot inspect large-data filesystem");
                    }
                    const auto minimumFree = largeDataFileMinimumFreeBytes();
                    if (available < minimumFree ||
                        m_largeDataReservedBytes > available - minimumFree ||
                        reservation > available - minimumFree - m_largeDataReservedBytes) {
                        throw std::runtime_error(
                            "large-data file publication has insufficient reserved disk space"
                            " available=" + std::to_string(available) +
                            " minimum=" + std::to_string(minimumFree) +
                            " pending=" + std::to_string(m_largeDataReservedBytes) +
                            " requested=" + std::to_string(reservation));
                    }
                    m_largeDataReservedBytes += reservation;
                    reservationHeld = true;
                }
                try {
                    size_t encodedBytes = 0;
                    filePath = measureCall(ENCRYPT, [&] {
                      return streamEncryptLargeDataEnvelope(
                        key.key, ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                        ndn::span<const uint8_t>(ad.data(), ad.size()), envelope,
                        encryptedDataName, encodedBytes, requireActive);
                    });
                    const std::string manifestText =
                        std::string("version=1\n") +
                        "name=" + encryptedDataName.toUri() + "\n" +
                        "object_id=" + result.objectId + "\n" +
                        "plaintext_size=" + std::to_string(result.plaintextSize) + "\n" +
                        "content_digest=" + result.contentDigest + "\n" +
                        "authorization_scope=" + result.authorizationScope + "\n" +
                        "protection_epoch=" + result.protectionEpoch + "\n" +
                        "encrypted=1\n";
                    result.manifestDigest = sha256DigestString(ndn::Buffer(
                        reinterpret_cast<const uint8_t*>(manifestText.data()),
                        manifestText.size()));
                    const auto segmentCount = 1 + (encodedBytes - 1) / maxSegmentBytes;
                    publication = std::make_shared<LargeDataFilePublication>();
                    publication->baseName = encryptedDataName;
                    publication->filePath = filePath;
                    publication->fileSize = encodedBytes;
                    publication->segmentCount = segmentCount;
                    publication->maxSegmentBytes = maxSegmentBytes;
                    publication->freshness = freshness;
                    publication->windowCapacity = largeDataWindowSegments();
                    publication->manifestPacket = measureCall(MANIFEST, [&] {
                      return prepareManifest(encodedBytes);
                    });
                    publication->reservedBytes = reservation;
                    if (rangeStore && !durable) {
                        auto commitTiming = measure(STORE_COMMIT);
                            publication->rangeSource = rangeStore->commitFile(
                                encryptedDataName.toUri(), filePath, encodedBytes, requireActive);
                        if (!publication->rangeSource ||
                            publication->rangeSource->size() != encodedBytes)
                            throw std::runtime_error("invalid committed encrypted range source");
                        std::filesystem::remove(filePath);
                        publication->filePath.clear();
                        filePath.clear();
                    }
                    else {
                        publication->stream.open(filePath, std::ios::binary);
                        if (!publication->stream)
                            throw std::runtime_error("cannot open committed large-data wire file");
                    }
                    publication->originalPackets = measureCall(ORIGINAL_PACKETS, [&] {
                      return detail::OriginalPacketFile::create(
                        directory, encryptedDataName, segmentCount, packetStorageBudget,
                        [&](std::uint64_t segment) {
                            if (requireActive) requireActive();
                            ndn::Data data(ndn::Name(encryptedDataName).appendSegment(segment));
                            data.setFreshnessPeriod(freshness);
                            data.setFinalBlock(ndn::name::Component::fromSegment(segmentCount - 1));
                            data.setContent(publication->readSegment(segment));
                            onIo([&] {
                                auto& signingKeyChain = m_testSigningKeyChain ?
                                    *m_testSigningKeyChain : m_keyChain;
                                signingKeyChain.sign(data, m_signingInfo);
                            });
                            const auto& wire = data.wireEncode();
                            return std::vector<std::uint8_t>(wire.begin(), wire.end());
                        }, requireActive);
                    });
                    if (durable) {
                        auto commitTiming = measure(STORE_COMMIT);
                        EncryptedLargeDataCommitOptions commitOptions;
                        commitOptions.retention = options.retention;
                        commitOptions.publicationIdentity = result.publicationIdentity;
                        commitOptions.protectionEpoch = result.protectionEpoch;
                        commitOptions.keyReferenceId = result.keyReferenceId;
                        commitOptions.keyReferenceVersion = result.keyReferenceVersion;
                        commitOptions.ciphertextManifestDigest = result.manifestDigest;
                        commitOptions.servingLocator = encryptedDataName.toUri();
                        commitOptions.contentDigest = result.contentDigest;
                        commitOptions.plaintextSize = result.plaintextSize;
                        commitOptions.packetStorageFormat = "original-packet-set";
                        commitOptions.manifestName = result.manifestName.toUri(ndn::name::UriFormat::CANONICAL);
                        commitOptions.dataPrefix = encryptedDataName.toUri(ndn::name::UriFormat::CANONICAL);
                        const auto& rootWire = publication->manifestPacket->wireEncode();
                        commitOptions.packetWireBytes = rootWire.size() +
                            publication->originalPackets->storageBytes() - segmentCount * 2 * sizeof(std::uint64_t);
                        std::uint64_t next = 0;
                        publication->rangeSource = rangeStore->commitOriginalDataPackets(result.manifestName,
                            commitOptions, [&]() -> std::optional<std::pair<ndn::Name, std::vector<std::uint8_t>>> {
                            if (requireActive) requireActive();
                            if (next == 0) {
                                ++next;
                                return std::make_pair(result.manifestName,
                                    std::vector<std::uint8_t>(rootWire.begin(), rootWire.end()));
                            }
                            if (next > segmentCount) return std::nullopt;
                            const auto segment = next++ - 1;
                            return std::make_pair(ndn::Name(encryptedDataName).appendSegment(segment),
                                publication->originalPackets->read(segment));
                        }, requireActive);
                        if (!publication->rangeSource || !publication->rangeSource->isDurable() ||
                            publication->rangeSource->size() != encodedBytes ||
                            publication->rangeSource->readOriginalDataPacket(result.manifestName) !=
                                std::vector<std::uint8_t>(rootWire.begin(), rootWire.end()))
                            throw std::runtime_error("DURABLE_ORIGINAL_PACKET_COMMIT_INVALID");
                        publication->originalPacketSource = true;
                        publication->originalPackets.reset();
                    }
                    // This metric counts serving reads, not preparation reads.
                    publication->segmentReadCount = 0;
                    if (!publication->rangeSource || publication->originalPacketSource) {
                        publication->stream.close();
                        std::filesystem::remove(filePath);
                        publication->filePath.clear();
                        filePath.clear();
                    }
                    {
                        // The completed immutable bytes are already charged
                        // to filesystem.available. Reserve only unfinished
                        // writers, not the same retained file a second time.
                        std::lock_guard<std::mutex> lock(_cache_mutex);
                        m_largeDataReservedBytes -= reservation;
                        reservationHeld = false;
                        publication->reservedBytes = 0;
                    }
                    publication->keyLease = std::make_shared<LargeDataKeyReleaseState::Lease>(
                        m_largeDataKeyReleaseState, ctx.serviceName, key.keyId);
                    wrappedKeyReferenceHeld = false; // ownership transferred exactly once
                    result.fileBacked = true;
                    if (retainWhileLeased) {
                        result.servingLease = std::make_shared<unsigned char>(0);
                        publication->servingLease = result.servingLease;
                    }
                    if (durable) {
                        std::lock_guard<std::mutex> commitFence(
                            m_protectedReferenceCommitMutex);
                        std::unique_lock<std::shared_mutex> fence(m_protectedReuseMutex);
                        if (!durableVersionMatches()) {
                            throw std::runtime_error(
                                "DURABLE_STALE_CONTROLLER_VERSION");
                        }
                        pendingPublicationKey = publicationKey + "#" +
                            std::to_string(++m_protectedPendingSequence);
                        m_protectedPendingPublications.emplace(
                            pendingPublicationKey,
                            ProtectedPublicationPending{
                                ctx.serviceName.toUri(), publicationKey,
                                durableVersioned ? *durableControllerVersion :
                                    ControllerVersion{}, durableVersioned,
                                {}, false});
                    }
                    if (hasWrappedMessageKey) onIo([&] {
                        // Wrapped-key packets remain small control Data and use
                        // the existing IMS path; only the encrypted object body
                        // is moved to the file-backed serving owner.
                        {
                            std::lock_guard<std::mutex> lock(_cache_mutex);
                            for (const auto& data : wrappedContentData) {
                                const auto fullName = data->getFullName();
                                const bool existed = static_cast<bool>(m_IMS.find(fullName));
                                if (!existed) {
                                    stagedFullNames.push_back(fullName);
                                    if (durable)
                                        protectedImsNames.push_back(fullName.toUri());
                                }
                                else if (durable &&
                                         m_protectedImsOwners.find(fullName.toUri()) !=
                                             m_protectedImsOwners.end()) {
                                    protectedImsNames.push_back(fullName.toUri());
                                }
                                m_IMS.insert(*data);
                            }
                            for (const auto& data : wrappedCkData) {
                                const auto fullName = data->getFullName();
                                const bool existed = static_cast<bool>(m_IMS.find(fullName));
                                if (!existed) {
                                    stagedFullNames.push_back(fullName);
                                    if (durable)
                                        protectedImsNames.push_back(fullName.toUri());
                                }
                                else if (durable &&
                                         m_protectedImsOwners.find(fullName.toUri()) !=
                                             m_protectedImsOwners.end()) {
                                    protectedImsNames.push_back(fullName.toUri());
                                }
                                m_IMS.insert(*data);
                            }
                        }
                        if (durable) {
                            std::unique_lock<std::shared_mutex> fence(
                                m_protectedReuseMutex);
                            const auto pending = m_protectedPendingPublications.find(
                                pendingPublicationKey);
                            if (pending == m_protectedPendingPublications.end() ||
                                pending->second.cancelled) {
                                std::lock_guard<std::mutex> cleanupLock(_cache_mutex);
                                for (const auto& fullName : protectedImsNames) {
                                    if (m_protectedImsOwners.find(fullName) ==
                                        m_protectedImsOwners.end()) {
                                        m_IMS.erase(ndn::Name(fullName), false);
                                    }
                                    m_retiredProtectedPrefixes.insert(fullName);
                                }
                            }
                            else {
                                pending->second.imsNames = protectedImsNames;
                            }
                        }
                    });
                    if (requireActive) requireActive();
                    result.encryptedDataName = encryptedDataName;
                    result.ciphertextManifestDigest = result.manifestDigest;
                    result.servingLocator = encryptedDataName.toUri();
                    if (durable) {
                        // The pending entry closes the gap between wrapped-key
                        // insertion and publication-owner registration.  The
                        // short commit fence serializes this final owner
                        // registration with status retirement, while the
                        // actual reference file operation stays outside the
                        // lifecycle shared mutex.
                        std::lock_guard<std::mutex> commitFence(
                            m_protectedReferenceCommitMutex);
                        {
                            std::unique_lock<std::shared_mutex> fence(
                                m_protectedReuseMutex);
                            const auto pending = m_protectedPendingPublications.find(
                                pendingPublicationKey);
                            if (pending == m_protectedPendingPublications.end() ||
                                pending->second.cancelled ||
                                !durableVersionMatches()) {
                                throw std::runtime_error(
                                    "DURABLE_STALE_CONTROLLER_VERSION");
                            }
                            publication->serviceName = ctx.serviceName.toUri();
                            publication->publicationIdentity =
                                result.publicationIdentity;
                            publication->controllerVersioned = durableVersioned;
                            if (durableVersioned)
                                publication->controllerVersion =
                                    *durableControllerVersion;
                            publication->protectedImsNames = protectedImsNames;
                            m_retiredProtectedPrefixes.erase(publicationKey);
                            for (const auto& fullName : protectedImsNames)
                                m_retiredProtectedPrefixes.erase(fullName);
                            std::lock_guard<std::mutex> lock(_cache_mutex);
                            const auto [it, inserted] = m_largeDataFiles.emplace(
                                publicationKey, publication);
                            if (!inserted) {
                                throw std::runtime_error(
                                    "duplicate large-data publication name");
                            }
                            (void)it;
                            for (const auto& fullName : protectedImsNames) {
                                m_protectedImsOwners[fullName][publicationKey] =
                                    ProtectedImsOwner{
                                        ctx.serviceName.toUri(),
                                        durableVersioned ? *durableControllerVersion :
                                            ControllerVersion{}};
                            }
                            m_protectedPendingPublications.erase(pending);
                            publicationRegistered = true;
                            reservationHeld = false;
                        }
                        // The Core-owned reference becomes COMMITTED only
                        // after Repo commit/read-back and local serving-owner
                        // registration have succeeded.  The commit fence
                        // keeps a strictly newer status from linearizing
                        // between that registration and this write.
                        const DurableLargeDataKeyReference keyReference{
                            result.publicationIdentity,
                            ctx.serviceName.toUri(),
                            encryptedDataName.toUri(),
                            result.contentDigest,
                            result.plaintextSize,
                            durableVersioned ? durableControllerVersion->controllerEpoch : 0,
                            durableVersioned ?
                                durableControllerVersion->controllerGenerationTimestamp : 0,
                            result.protectionEpoch,
                            result.keyReferenceId,
                            result.keyReferenceVersion,
                            result.manifestDigest,
                            encryptedDataName.toUri(),
                            result.manifestName.toUri(ndn::name::UriFormat::CANONICAL),
                            "original-packet-set"};
                        if (!measureCall(KEY_REFERENCE, [&] {
                              return persistDurableKeyReference(identity, keyReference);
                            })) {
                            throw std::runtime_error(
                                "DURABLE_KEY_REFERENCE_STORE_UNAVAILABLE");
                        }
                    }
                    else {
                        std::lock_guard<std::mutex> lock(_cache_mutex);
                        const auto [it, inserted] = m_largeDataFiles.emplace(
                            publicationKey, publication);
                        if (!inserted) {
                            throw std::runtime_error(
                                "duplicate large-data publication name");
                        }
                        (void)it;
                        publicationRegistered = true;
                        reservationHeld = false;
                    }
                    const auto expiry = *fileBackedRetention;
                    onIo([&] { m_scheduler.schedule(expiry, [this, publicationKey,
                                                   weakPublication = std::weak_ptr<LargeDataFilePublication>(publication)] {
                        expireLargeDataPublication(publicationKey, weakPublication);
                    }); });
                    if (durable) {
                        result.rollbackDataNames = protectedImsNames;
                    }
                    else {
                        for (const auto& data : wrappedContentData)
                            result.rollbackDataNames.push_back(data->getFullName().toUri());
                        for (const auto& data : wrappedCkData)
                            result.rollbackDataNames.push_back(data->getFullName().toUri());
                    }
                    if (publication->keyLease) {
                        result.rollbackKeyId = key.keyId;
                        result.rollbackServiceName = ctx.serviceName.toUri();
                    }
                    NDN_LOG_INFO("LARGE_DATA_PUBLISH_FILE_BACKED"
                                 << " name=" << result.encryptedDataName.toUri()
                                 << " plaintextBytes=" << plaintext.size()
                                 << " envelopeBytes=" << encodedBytes
                                 << " segments=" << segmentCount
                                 << " window=" << publication->windowCapacity
                                 << " retentionMs=" << expiry.count());
                    result.success = true;
                    return result;
                }
                catch (...) {
                    rollback();
                    throw;
                }
            }
            auto encrypted = hybridAesGcmEncryptWithNonce(
                key.key,
                ndn::span<const uint8_t>(blobIdentity.nonce.data(), blobIdentity.nonce.size()),
                ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                ndn::span<const uint8_t>(ad.data(), ad.size()));
            envelope.setNonce(encrypted.nonce);
            // Transfer the multi-gigabyte ciphertext into the envelope.  The
            // const-reference setter would retain a second full copy during
            // WireEncode, which is unsafe for a real Qwen initializer.
            envelope.setCipherText(std::move(encrypted.ciphertext));
            envelope.setAuthTag(encrypted.tag);

            // Keep one shared wire buffer for segmentation.  A copied
            // ndn::Block shares WireEncode's ConstBufferPtr, so this avoids
            // the second multi-gigabyte vector that an assign(begin,end)
            // conversion would allocate while the source package is live.
            const auto envelopeBlock = envelope.WireEncode();
            if (!envelopeBlock.hasWire() || envelopeBlock.size() == 0)
                throw std::runtime_error("large-data hybrid envelope encoded no wire");
            // Clear() resets the logical size but a vector may retain its
            // multi-gigabyte capacity.  Move an empty buffer through the
            // dedicated overload so the ciphertext allocation is released
            // before Data packets are materialized.
            const bool wrappedKeyAttached = envelope.hasWrappedMessageKey();
            envelope.setCipherText(ndn::Buffer{});
            envelope.Clear();

            const std::string manifestText =
                std::string("version=1\n") +
                "name=" + encryptedDataName.toUri() + "\n" +
                "object_id=" + result.objectId + "\n" +
                "plaintext_size=" + std::to_string(result.plaintextSize) + "\n" +
                "content_digest=" + result.contentDigest + "\n" +
                "authorization_scope=" + result.authorizationScope + "\n" +
                "protection_epoch=" + result.protectionEpoch + "\n" +
                "encrypted=1\n";
            result.manifestDigest = sha256DigestString(ndn::Buffer(
                reinterpret_cast<const uint8_t*>(manifestText.data()),
                manifestText.size()));

            // Segmenter::segment returns a vector owning every Data packet.
            // That is acceptable for small objects but makes a multi-GiB
            // model artifact retain the whole packet set until publication
            // finishes.  The encrypted envelope is already contiguous here;
            // materialize and release one signed packet at a time instead.
            const auto encodedBytes = envelopeBlock.size();
            const auto manifestPacket = prepareManifest(encodedBytes);
            const auto segmentCount = 1 + (encodedBytes - 1) / maxSegmentBytes;
            if (segmentCount == 0) {
                result.errorMessage = "large-data hybrid segmenter produced no segments";
                return result;
            }

            auto& signingKeyChain =
                m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain;
            std::unique_lock<std::mutex> stagingLock(_cache_mutex, std::defer_lock);
            if (activePut) {
                // Keep the capacity reservation and all staging inserts in one
                // critical section.  Otherwise another publisher could evict
                // an early segment before the replay phase reaches it.
                stagingLock.lock();
                const auto currentPackets = m_IMS.size();
                const auto wrappedContentCount = wrappedContentData.size();
                const auto wrappedCkCount = wrappedCkData.size();
                const bool wrappedCountOverflow =
                    wrappedContentCount > std::numeric_limits<std::size_t>::max() -
                        wrappedCkCount;
                const auto wrappedCount = wrappedCountOverflow ? 0 :
                    wrappedContentCount + wrappedCkCount;
                const bool capacityOverflow =
                    wrappedCountOverflow ||
                    segmentCount + 1 > m_IMS.getLimit() ||
                    wrappedCount > m_IMS.getLimit() -
                        std::min(segmentCount + 1, m_IMS.getLimit()) ||
                    segmentCount + 1 + wrappedCount > m_IMS.getLimit() ||
                    currentPackets > m_IMS.getLimit() - (segmentCount + 1 + wrappedCount);
                if (capacityOverflow) {
                    stagingLock.unlock();
                    result.errorMessage =
                        "large-data publication exceeds transactional IMS staging capacity";
                    return result;
                }
            }
            try {
                std::vector<ndn::Name> stagedNames;
                if (activePut) {
                    // NAC-ABE wrapped-key packets are part of the same local
                    // staging transaction.  Insert them under the held lock
                    // instead of calling serveDataWithIMS (which locks again).
                    if (hasWrappedMessageKey) {
                        for (const auto& data : wrappedContentData) {
                            const auto fullName = data->getFullName();
                            const bool existed = static_cast<bool>(m_IMS.find(fullName));
                            if (!existed) {
                                // Register before insert so an exception from
                                // IMS insertion is still covered by rollback.
                                stagedFullNames.push_back(fullName);
                            }
                            m_IMS.insert(*data, freshness);
                        }
                        for (const auto& data : wrappedCkData) {
                            const auto fullName = data->getFullName();
                            const bool existed = static_cast<bool>(m_IMS.find(fullName));
                            if (!existed) {
                                stagedFullNames.push_back(fullName);
                            }
                            m_IMS.insert(*data, freshness);
                        }
                    }
                    stagedNames.reserve(segmentCount);
                }
                else if (hasWrappedMessageKey) {
                    {
                        std::lock_guard<std::mutex> lock(_cache_mutex);
                        for (const auto& data : wrappedContentData) {
                            const auto fullName = data->getFullName();
                            if (!m_IMS.find(fullName))
                                stagedFullNames.push_back(fullName);
                            m_IMS.insert(*data);
                        }
                        for (const auto& data : wrappedCkData) {
                            const auto fullName = data->getFullName();
                            if (!m_IMS.find(fullName))
                                stagedFullNames.push_back(fullName);
                            m_IMS.insert(*data);
                        }
                    }
                }
                if (hasWrappedMessageKey && !activePut) {
                    wrappedKeyReferenceHeld = true;
                    ++m_hybridCryptoCounters.nac_abe_key_wrap_count;
                }
                for (std::size_t segment = 0; segment < segmentCount; ++segment) {
                    const auto offset = segment * maxSegmentBytes;
                    const auto length = std::min(maxSegmentBytes, encodedBytes - offset);
                    auto data = std::make_shared<ndn::Data>(
                        ndn::Name(encryptedDataName).appendSegment(segment));
                    data->setContent(ndn::span<const uint8_t>(envelopeBlock.data() + offset, length));
                    data->setFinalBlock(
                        ndn::name::Component::fromSegment(segmentCount - 1));
                    data->setFreshnessPeriod(freshness);
                    signingKeyChain.sign(*data, m_signingInfo);
                    // Face::put documents OversizedPacketError as its packet
                    // validation boundary.  Check the signed wire before any
                    // network publication so a later packet cannot fail that
                    // boundary after an earlier packet has been queued.
                    if (data->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE) {
                        throw ndn::Face::OversizedPacketError(
                            'D', data->getName(), data->wireEncode().size());
                    }
                    {
                        if (activePut) {
                            const auto fullName = data->getFullName();
                            const bool existed = static_cast<bool>(m_IMS.find(fullName));
                            if (!existed) {
                                stagedFullNames.push_back(fullName);
                            }
                            m_IMS.insert(*data, freshness);
                        }
                        else {
                            std::lock_guard<std::mutex> lock(_cache_mutex);
                            m_IMS.insert(*data, freshness);
                        }
                    }
                    if (activePut) {
                        stagedNames.push_back(data->getName());
                    }
                }
                {
                    // Advertise the descriptor only after all body segments
                    // have been staged. Its full name participates in abort.
                    const auto fullName = manifestPacket->getFullName();
                    if (activePut) {
                        if (!m_IMS.find(fullName)) stagedFullNames.push_back(fullName);
                        m_IMS.insert(*manifestPacket, freshness);
                        stagedNames.push_back(manifestPacket->getName());
                    }
                    else {
                        std::lock_guard<std::mutex> lock(_cache_mutex);
                        if (!m_IMS.find(fullName)) stagedFullNames.push_back(fullName);
                        m_IMS.insert(*manifestPacket, freshness);
                    }
                }
                if (activePut) {
                    // All signing, encoding and IMS insertion has succeeded.
                    // Replay the stable IMS entries only after that prepare
                    // phase while retaining one Data at a time. Keep the
                    // cache lock through replay so another publisher cannot
                    // evict a staged entry before its Face::put call.
                    for (const auto& name : stagedNames) {
                        std::shared_ptr<const ndn::Data> data;
                        // stagingLock still owns _cache_mutex here, so do not
                        // take a nested lock while resolving the staged entry.
                        data = m_IMS.find(name);
                        if (!data) {
                            throw std::runtime_error(
                                "large-data IMS staging entry disappeared before publication");
                        }
                        m_face.put(*data);
                    }
                    if (hasWrappedMessageKey) {
                        wrappedKeyReferenceHeld = true;
                        ++m_hybridCryptoCounters.nac_abe_key_wrap_count;
                    }
                    stagingLock.unlock();
                }
            }
            catch (...) {
                if (stagingLock.owns_lock()) {
                    stagingLock.unlock();
                }
                // The root manifest is published only after every artifact
                // succeeds, so a failed artifact has no externally advertised
                // reference.  Remove any locally retained prefix before
                // returning the failure; already queued Face packets remain
                // unreferenced and expire with their freshness period.
                std::lock_guard<std::mutex> lock(_cache_mutex);
                for (const auto& fullName : stagedFullNames) {
                    m_IMS.erase(fullName, false);
                }
                throw;
            }

            result.encryptedDataName = encryptedDataName;
            for (const auto& fullName : stagedFullNames)
                result.rollbackDataNames.push_back(fullName.toUri());
            if (wrappedKeyReferenceHeld) {
                result.rollbackKeyId = key.keyId;
                result.rollbackServiceName = ctx.serviceName.toUri();
            }
            NDN_LOG_INFO("LARGE_DATA_PUBLISH_SEGMENTS"
                      << " name=" << result.encryptedDataName.toUri()
                      << " plaintextBytes=" << plaintext.size()
                      << " envelopeBytes=" << encodedBytes
                      << " segments=" << segmentCount
                      << " wrappedKeyAttached=" << wrappedKeyAttached
                      << " activePut=" << activePut);
            result.success = true;
        }
        catch (const std::exception& e) {
            if (wrappedKeyReferenceHeld) {
                m_hybridMessageCrypto.eraseWrappedSendKey(ctx.serviceName, wrappedKeyId);
            }
            result.errorMessage = e.what();
        }
        return result;
    }

    void ServiceUser::setEncryptedLargeDataRangeStore(
        std::shared_ptr<EncryptedLargeDataRangeStore> store)
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        if (!m_largeDataFiles.empty())
            throw std::logic_error("cannot change range store after publication");
        m_largeDataRangeStore = std::move(store);
    }

    void ServiceUser::retireProtectedPublications(
        const ndn::Name& serviceName,
        const ControllerVersion& version)
    {
        std::vector<std::shared_ptr<LargeDataFilePublication>> removed;
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            for (auto& [pendingKey, pending] : m_protectedPendingPublications) {
                if (pending.serviceName != serviceName.toUri() ||
                    (pending.versioned && pending.version.compare(version) >= 0)) {
                    continue;
                }
                pending.cancelled = true;
                m_retiredProtectedPrefixes.insert(pending.publicationKey);
                for (const auto& fullName : pending.imsNames) {
                    m_retiredProtectedPrefixes.insert(fullName);
                    if (m_protectedImsOwners.find(fullName) ==
                        m_protectedImsOwners.end()) {
                        m_IMS.erase(ndn::Name(fullName), false);
                    }
                }
                (void)pendingKey;
            }
            for (auto it = m_largeDataFiles.begin(); it != m_largeDataFiles.end();) {
                const auto& publication = it->second;
                if (!publication || publication->serviceName != serviceName.toUri() ||
                    (publication->controllerVersioned &&
                     publication->controllerVersion.compare(version) >= 0)) {
                    ++it;
                    continue;
                }

                const auto publicationKey = it->first;
                m_retiredProtectedPrefixes.insert(publication->baseName.toUri());
                for (const auto& fullName : publication->protectedImsNames) {
                    auto owners = m_protectedImsOwners.find(fullName);
                    if (owners != m_protectedImsOwners.end()) {
                        owners->second.erase(publicationKey);
                        if (owners->second.empty()) {
                            m_protectedImsOwners.erase(owners);
                            m_IMS.erase(ndn::Name(fullName), false);
                            m_retiredProtectedPrefixes.insert(fullName);
                        }
                    }
                }
                if (m_largeDataReservedBytes >= publication->reservedBytes) {
                    m_largeDataReservedBytes -= publication->reservedBytes;
                }
                else {
                    m_largeDataReservedBytes = 0;
                }
                removed.push_back(publication);
                it = m_largeDataFiles.erase(it);
            }
        }
        // `removed` is intentionally destroyed after the cache lock is
        // released.  Durable range-source destruction never calls Repo
        // release(), so ciphertext ownership remains outside this local
        // serving transition.
        removed.clear();
    }

    bool ServiceUser::supportsDurableEncryptedLargeData() const noexcept
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        return m_largeDataRangeStore && m_largeDataRangeStore->supportsDurableRetention() &&
            m_largeDataRangeStore->supportsOriginalDataPackets();
    }

    void ServiceUser::expireLargeDataPublication(const std::string& publicationKey,
        std::weak_ptr<LargeDataFilePublication> weakPublication)
    {
        std::shared_ptr<LargeDataFilePublication> removed;
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            const auto it = m_largeDataFiles.find(publicationKey);
            const auto publication = weakPublication.lock();
            if (it == m_largeDataFiles.end() || !publication || it->second != publication)
                return;
            if (!publication->servingLease.expired()) {
                m_scheduler.schedule(ndn::time::seconds(1),
                    [this, publicationKey, weakPublication] {
                        expireLargeDataPublication(publicationKey, weakPublication);
                    });
                return;
            }
            const auto publicationKey = it->first;
            for (const auto& fullName : publication->protectedImsNames) {
                auto owners = m_protectedImsOwners.find(fullName);
                if (owners == m_protectedImsOwners.end()) {
                    continue;
                }
                owners->second.erase(publicationKey);
                if (owners->second.empty()) {
                    m_protectedImsOwners.erase(owners);
                    m_IMS.erase(ndn::Name(fullName), false);
                }
            }
            m_largeDataReservedBytes -= publication->reservedBytes;
            removed = publication;
            m_largeDataFiles.erase(it);
        }
        // Backend release may perform disk I/O. Do not hold Core's cache lock.
        removed.reset();
    }

    void ServiceUser::abortLargeDataPublications(
        const std::vector<LargeDataPublishResult>& publications) noexcept
    {
        for (const auto& publication : publications) {
            try {
                std::lock_guard<std::mutex> lock(_cache_mutex);
                if (!publication.publicationIdentity.empty()) {
                    const auto publicationKey = publication.encryptedDataName.toUri();
                    for (const auto& value : publication.rollbackDataNames) {
                        auto owners = m_protectedImsOwners.find(value);
                        if (owners == m_protectedImsOwners.end()) {
                            continue;
                        }
                        owners->second.erase(publicationKey);
                        if (owners->second.empty()) {
                            m_protectedImsOwners.erase(owners);
                            m_IMS.erase(ndn::Name(value), false);
                        }
                    }
                }
                else {
                    for (const auto& value : publication.rollbackDataNames) {
                        const ndn::Name prefix(value);
                        if (!prefix.empty())
                            m_IMS.erase(prefix, true);
                    }
                    if (!publication.encryptedDataName.empty())
                        m_IMS.erase(publication.encryptedDataName, true);
                }
            }
            catch (...) {
                // Rollback is deliberately best effort; queued Face packets
                // still expire at their normal freshness boundary.
            }
            std::vector<std::shared_ptr<LargeDataFilePublication>> removed;
            {
                std::lock_guard<std::mutex> lock(_cache_mutex);
                for (auto it = m_largeDataFiles.begin(); it != m_largeDataFiles.end();) {
                    const bool owned = it->first == publication.encryptedDataName.toUri() ||
                        std::find(publication.rollbackDataNames.begin(), publication.rollbackDataNames.end(),
                                  it->first) != publication.rollbackDataNames.end();
                    if (!owned) { ++it; continue; }
                    if (it->second) {
                        m_largeDataReservedBytes =
                            m_largeDataReservedBytes >= it->second->reservedBytes
                              ? m_largeDataReservedBytes - it->second->reservedBytes : 0;
                    }
                    removed.push_back(it->second);
                    it = m_largeDataFiles.erase(it);
                }
            }
            const bool fileOwnedKey = publication.fileBacked || !removed.empty();
            removed.clear();
            if (!fileOwnedKey && !publication.rollbackKeyId.empty() && !publication.rollbackServiceName.empty()) {
                try {
                    m_hybridMessageCrypto.eraseWrappedSendKey(
                        ndn::Name(publication.rollbackServiceName), publication.rollbackKeyId);
                }
                catch (...) {}
            }
        }
    }

    std::shared_ptr<const SecurityProfileStore::Record>
    ServiceUser::ensurePublishedSecurityProfile()
    {
        if (m_localSecurityProfile &&
            m_localSecurityProfile->profile.certificate.getName() == identityCert.getName())
            return m_localSecurityProfile;
        ProducerSecurityProfile profile{identity, identityCert,
            ProducerSecurityProfile::SUPPORTED_CAPABILITIES};
        auto record = std::make_shared<SecurityProfileStore::Record>();
        record->packet = profile.makeData(
            m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain,
            m_signingInfo, ndn::time::seconds(60));
        if (!validator->isProducerSignerAuthorized(*record->packet, identity))
            throw std::invalid_argument("User security profile signer not authorized");
        record->advertisement = profile.advertisement();
        record->publicKeyDigest = profile.publicKeyDigest();
        record->publicKeyHex = selectionGatedHex(profile.certificate.getPublicKey());
        record->profile = std::move(profile);
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            m_IMS.insert(*record->packet, record->packet->getFreshnessPeriod());
        }
        m_localSecurityProfile = record;
        m_face.put(*record->packet);
        return record;
    }

    ndn::Name ServiceUser::publishSignedAppData(
        const ndn::Name& dataName,
        const ndn::Buffer& payload,
        ndn::time::milliseconds freshness)
    {
        ndn::Name allowedPrefix(identity);
        allowedPrefix.append("NDNSF").append("DI");
        // spec181: the KEY-GRANT namespace uses the Spec170 spelling
        // NDNSF-DI (single component); accept both spellings below the
        // local identity.
        ndn::Name grantAllowedPrefix(identity);
        grantAllowedPrefix.append("NDNSF-DI");
        const auto current = parseProducerDataName(dataName, identity);
        const bool currentOwned = current && current->nameSpace == "NDNSF" &&
            (current->category == "GRANT" || current->category == "DEPLOYMENT");
        const bool legacyOwned = allowedPrefix.isPrefixOf(dataName) || grantAllowedPrefix.isPrefixOf(dataName);
        if ((!currentOwned && !legacyOwned) || dataName.size() <= identity.size() + 2)
            throw std::invalid_argument("signed APP Data is outside its actual Producer namespace");
        checkNamingNameSize(dataName);
        if (freshness <= ndn::time::milliseconds::zero()) {
            throw std::invalid_argument("signed APP Data freshness must be positive");
        }

        // InMemoryStorageEntry retains Data through shared_from_this(); a stack
        // Data would deterministically throw std::bad_weak_ptr on insertion.
        auto data = std::make_shared<ndn::Data>(dataName);
        data->setFreshnessPeriod(freshness);
        data->setContent(payload);
        std::shared_ptr<const ndn::Data> original;
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            for (auto entry = m_IMS.lowerBound(dataName);
                 entry != m_IMS.end() && dataName.isPrefixOf(entry->getFullName()); ++entry) {
                const auto& stored = *entry;
                if (stored.getName() != dataName) continue;
                if (!validator->isProducerSignerAuthorized(stored, identity) ||
                    !(stored.getContent() == data->getContent()) ||
                    !(stored.getMetaInfo().wireEncode() == data->getMetaInfo().wireEncode()) ||
                    (original && !(original->wireEncode() == stored.wireEncode())))
                    throw std::invalid_argument("immutable signed APP Data name conflict");
                original = stored.shared_from_this();
            }
            if (!original) {
                (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain).sign(*data, m_signingInfo);
                if (!validator->isProducerSignerAuthorized(*data, identity) ||
                    data->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
                    throw std::invalid_argument("signed APP Data signer or packet budget invalid");
                m_IMS.insert(*data, freshness);
                original = data;
            }
        }
        // Exact-name request-scoped input is a real network Data packet, not
        // merely a local cache entry.  Keep the IMS insertion for local
        // fixtures and also publish through the configured Face so the
        // selected Provider can fetch it by name.
        m_face.put(*original);
        NDN_LOG_INFO("Published signed APP Data name=" << dataName
                     << " bytes=" << payload.size());
        return dataName;
    }

    bool ServiceUser::publishCollaborationData(
        const ndn::Name& targetProvider,
        const ndn::Name& requestId,
        const KeyScope& keyScope,
        const ndn::Name& topic,
        const ndn::Buffer& payload)
    {
        if (targetProvider.empty() || requestId.empty() || keyScope.empty() ||
            topic.empty() || payload.empty() || payload.size() > 16 * 1024 ||
            m_svsps == nullptr) {
            return false;
        }

        ndn::Buffer scopeKey;
        {
            std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
            const auto requestIt =
                m_userCollaborationScopeKeys.find(requestId);
            if (requestIt == m_userCollaborationScopeKeys.end()) {
                return false;
            }
            const auto keyIt = requestIt->second.find(keyScope);
            if (keyIt == requestIt->second.end() ||
                keyIt->second.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                return false;
            }
            scopeKey = keyIt->second;
        }

        const uint64_t sequence =
            m_collaborationSequence.fetch_add(1, std::memory_order_relaxed);
        CollaborationDataMessage message;
        message.setKeyScope(keyScope);
        message.setTopic(topic);
        message.setProducerRole("user-control-v1");
        message.setSequence(sequence);

        const ndn::Name dataName = makeCollaborationDataName(
            identity, targetProvider, requestId, keyScope, topic, sequence);
        const std::string keyId =
            "collab|" + requestId.toUri(ndn::name::UriFormat::CANONICAL) + "|" + keyScope;
        const std::string epochId = "session";
        const auto associatedData = userCollaborationAssociatedData(
            dataName, requestId, message, keyId, epochId);
        const auto encrypted = hybridAesGcmEncrypt(
            scopeKey,
            ndn::span<const uint8_t>(payload.data(), payload.size()),
            ndn::span<const uint8_t>(associatedData.data(), associatedData.size()));

        HybridMessageEnvelope envelope;
        envelope.setKeyId(keyId);
        envelope.setEpochId(epochId);
        envelope.setMessageType("COLLAB");
        envelope.setNonce(encrypted.nonce);
        envelope.setCipherText(encrypted.ciphertext);
        envelope.setAuthTag(encrypted.tag);
        const auto envelopeBlock = envelope.WireEncode();
        message.setPayload(ndn::Buffer(envelopeBlock.begin(), envelopeBlock.end()));
        const auto messageBlock = message.WireEncode();
        const ndn::Buffer encoded(messageBlock.begin(), messageBlock.end());

        {
            std::lock_guard<std::mutex> lock(m_collaborationPublishMutex);
            ++m_pendingCollaborationPublishes;
        }
        try {
            boost::asio::post(m_face.getIoContext(),
                [this, dataName, encoded] {
                    try {
                        if (m_svsps != nullptr) {
                            ndn::Block block(encoded);
                            publishSvs(m_svsps, dataName, block);
                        }
                    }
                    catch (const std::exception& error) {
                        NDN_LOG_ERROR("Collaboration data publish failed for "
                                      << dataName.toUri() << ": " << error.what());
                    }
                    catch (...) {
                        NDN_LOG_ERROR("Collaboration data publish failed for "
                                      << dataName.toUri() << ": unknown exception");
                    }
                    finishCollaborationPublish();
                });
        }
        catch (...) {
            finishCollaborationPublish();
            return false;
        }
        return true;
    }

    void ServiceUser::finishCollaborationPublish() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(m_collaborationPublishMutex);
            if (m_pendingCollaborationPublishes != 0) {
                --m_pendingCollaborationPublishes;
            }
        }
        m_collaborationPublishCv.notify_all();
    }

    bool ServiceUser::waitForCollaborationPublishIdle(
        std::chrono::milliseconds timeout) const
    {
        if (timeout.count() < 0) {
            throw std::invalid_argument(
                "collaboration publish idle timeout must not be negative");
        }
        std::unique_lock<std::mutex> lock(m_collaborationPublishMutex);
        const auto idle = [this] {
            return m_pendingCollaborationPublishes == 0;
        };
        if (idle()) {
            return true;
        }
        if (isOnIoThread()) {
            return false;
        }
        return m_collaborationPublishCv.wait_for(lock, timeout, idle);
    }

    void ServiceUser::fetchSignedAppData(
        const ndn::Name& dataName,
        const ndn::Name& expectedSigner,
        int timeoutMs,
        SignedAppDataHandler onData,
        SignedAppDataFailureHandler onFailure)
    {
        if (dataName.empty() || expectedSigner.empty() || timeoutMs <= 0) {
            throw std::invalid_argument("invalid signed APP Data fetch binding");
        }
        ndn::Interest interest(dataName);
        interest.setCanBePrefix(false);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::milliseconds(timeoutMs));
        m_face.expressInterest(
            interest,
            [this, expectedSigner, onData = std::move(onData),
             onFailure](const ndn::Interest&, const ndn::Data& data) mutable {
                validator->validate(
                    data,
                    [expectedSigner, onData = std::move(onData), onFailure](
                        const ndn::Data& validatedData) mutable {
                        if (!isSignedByIdentity(validatedData, expectedSigner)) {
                            if (onFailure) {
                                onFailure(validatedData.getName(), "signer identity mismatch");
                            }
                            return;
                        }
                        if (onData) {
                            onData(validatedData);
                        }
                    },
                    [onFailure](const ndn::Data& badData,
                                const ndn::security::ValidationError& error) {
                        if (onFailure) {
                            std::ostringstream reason;
                            reason << "signature validation failed: " << error;
                            onFailure(badData.getName(), reason.str());
                        }
                    });
            },
            [onFailure](const ndn::Interest& failed,
                        const ndn::lp::Nack& nack) {
                if (onFailure) {
                    std::ostringstream reason;
                    reason << "network Nack: " << nack.getReason();
                    onFailure(failed.getName(), reason.str());
                }
            },
            [onFailure](const ndn::Interest& timedOut) {
                if (onFailure) {
                    onFailure(timedOut.getName(), "timeout");
                }
            });
    }

    LargeDataReferenceRequestResult ServiceUser::makeRequestWithLargeDataOptimization(
        const PreparedServiceRequest& ctx,
        const std::vector<uint8_t>& payload,
        const std::string& objectLabel,
        const std::string& objectType,
        size_t thresholdBytes,
        ndn::time::milliseconds freshness)
    {
        LargeDataReferenceRequestResult result;
        if (payload.size() <= thresholdBytes) {
            ndn::Buffer inlinePayload;
            if (!payload.empty()) {
                inlinePayload = ndn::Buffer(payload.data(), payload.size());
            }
            result.requestMessage.setPayload(inlinePayload, inlinePayload.size());
            result.success = true;
            return result;
        }

        result.largeData = publishEncryptedLargeData(ctx, payload, objectLabel, freshness);
        if (!result.largeData.success) {
            result.errorMessage = result.largeData.errorMessage.empty()
                ? "failed to publish encrypted large data"
                : result.largeData.errorMessage;
            return result;
        }

        LargeDataReference reference;
        reference.dataName = result.largeData.encryptedDataName;
        reference.objectType = objectType;
        reference.objectId = result.largeData.objectId;
        reference.plaintextSize = payload.size();
        reference.encrypted = true;
        auto referencePayload = encodeLargeDataReferencePayload(reference);
        result.requestMessage.setPayload(referencePayload, referencePayload.size());
        result.usedLargeDataReference = true;
        result.success = true;
        return result;
    }

    ndn::Buffer ServiceUser::prepareSelectionGatedInput(
        RequestMessage& requestMessage,
        const ndn::Name& serviceName,
        const ndn::Name& requestId)
    {
        ndn::Buffer selectionGatedInputKey;
        const bool gatesInput = requestMessage.hasRequestCapabilities() &&
          requestMessage.getRequestCapabilities().hasField("SelectionGatedInputV1") &&
          requestMessage.getRequestCapabilities().getField("SelectionGatedInputV1") == "required";
        if (!gatesInput) return selectionGatedInputKey;
        if (requestMessage.hasEncryptedRequestInput())
            throw std::invalid_argument(
                "pre-encrypted SelectionGatedInputV1 requires an explicit key handle");
        const auto plaintext = requestMessage.getPayload();
        auto encrypted = encryptSelectionGatedInput(
          identity, serviceName, requestId,
          ndn::span<const uint8_t>(plaintext.data(), plaintext.size()));
        requestMessage.setEncryptedRequestInput(encrypted.first);
        ndn::Buffer empty;
        requestMessage.setPayload(empty, 0);
        return std::move(encrypted.second);
    }

    bool ServiceUser::requiresRequestScopedConfidentiality(
        const ndn_service_framework::RequestMessage& requestMessage)
    {
        return requestMessage.hasRequestCapabilities() &&
               requestMessage.getRequestCapabilities().hasField(
                   "RequestScopedConfidentialityV1") &&
                   requestMessage.getRequestCapabilities().getField(
                       "RequestScopedConfidentialityV1") == "required";
    }

    namespace
    {
        // Keep the canonical DI envelope visible during discovery so a
        // Provider can issue a V3 offer, but remove inline application bytes.
        // The original payload remains in requestScopedPlaintext and is
        // published only after Selection.
        std::optional<ndn::Buffer>
        makeRequestScopedDiscoveryPayload(const ndn::Buffer& original)
        {
            if (original.empty()) {
                return std::nullopt;
            }
            const std::string wire(reinterpret_cast<const char*>(original.data()),
                                   original.size());
            if (wire.find("\"schema\":\"ndnsf-di-request-envelope-v2\"") ==
                std::string::npos) {
                return std::nullopt;
            }
            const std::string marker = "\"input_payload_b64\":\"";
            const auto valueStart = wire.find(marker);
            if (valueStart == std::string::npos) {
                return std::nullopt;
            }
            const auto contentStart = valueStart + marker.size();
            const auto contentEnd = wire.find('"', contentStart);
            if (contentEnd == std::string::npos) {
                return std::nullopt;
            }
            std::string discovery = wire;
            discovery.erase(contentStart, contentEnd - contentStart);
            return ndn::Buffer(reinterpret_cast<const uint8_t*>(discovery.data()),
                               discovery.size());
        }
    }

    bool ServiceUser::prepareRequestScopedRequest(
        ndn_service_framework::RequestMessage& requestMessage,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        ndn::Buffer& plaintext) const
    {
        bool requestScoped = requiresRequestScopedConfidentiality(requestMessage);

        // A configured Controller runtime has an authenticated, non-zero
        // version only after exact PolicyStatus Data was installed.  Make the
        // request-scoped path the default for ordinary V2 requests at that
        // point.  Controller-free LocalMock callers remain compatibility
        // fixtures, while an explicit capability container is treated as an
        // application-selected mode and is never silently mixed with this
        // default.
        const bool requestScopedDefault = hasRequestScopedDefault();
        // The temporary NDNSF_REQUEST_SCOPED_COMPATIBILITY rollback switch
        // (and its mixed-mode rejection and counters) was removed together
        // with the old service-wide response-key carrier once the migration
        // MiniNDN and streaming gates passed (T012/R179-M4).  The
        // request-scoped path is the only V2 protected invocation mode when
        // a Controller runtime is configured.
        bool onlyFrameworkDataCapability = false;
        if (requestMessage.hasRequestCapabilities()) {
            onlyFrameworkDataCapability = true;
            for (const auto& [name, value] :
                 requestMessage.getRequestCapabilities().getFields()) {
                if (name != "NDNSF_DATA_V1" || value != "required") {
                    onlyFrameworkDataCapability = false;
                    break;
                }
            }
        }
        if (!requestScoped &&
            (!requestMessage.hasRequestCapabilities() ||
             onlyFrameworkDataCapability) &&
            requestScopedDefault) {
            RequestCapabilities capabilities;
            if (onlyFrameworkDataCapability) {
                capabilities = requestMessage.getRequestCapabilities();
            }
            capabilities.setField("RequestScopedConfidentialityV1", "required");
            requestMessage.setRequestCapabilities(capabilities);
            requestScoped = true;
            NDN_LOG_INFO("NDNSF_REQUEST_SCOPED_DEFAULT role=user"
                         << " requestId=" << requestId.toUri()
                         << " serviceName=" << serviceName.toUri());
        }

        if (!requestScoped) {
            if (requestMessage.hasDiscoveryPayload()) {
                throw std::invalid_argument(
                    "discovery payload requires request-scoped confidentiality");
            }
            return false;
        }
        if (requestMessage.hasEncryptedRequestInput()) {
            throw std::invalid_argument(
                "RequestScopedConfidentialityV1 cannot combine with pre-encrypted input");
        }
        plaintext = requestMessage.getPayload();
        // Empty application input is still represented by an authenticated
        // request-scoped Data packet.  Rejecting it here would make the
        // default path dependent on an undocumented payload convention.
        if (requestMessage.hasDiscoveryPayload()) {
            auto discovery = requestMessage.getDiscoveryPayload();
            requestMessage.setPayload(discovery, discovery.size());
        }
        else if (auto discovery = makeRequestScopedDiscoveryPayload(plaintext)) {
            requestMessage.setPayload(*discovery, discovery->size());
        }
        else {
            ndn::Buffer empty;
            requestMessage.setPayload(empty, 0);
        }
        return true;
    }

    ndn::Name ServiceUser::makeRequestScopedInputDataName(
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        uint64_t attempt,
        const ndn::Name& providerName) const
    {
        if (serviceName.empty() || requestId.empty() || attempt == 0 ||
            providerName.empty()) {
            throw std::invalid_argument("invalid request-scoped input name components");
        }
        ndn::Name dataName(identity);
        dataName.append("NDNSF").append("DI").append("REQUEST-INPUT");
        dataName.append(serviceName).append(requestId).appendNumber(attempt)
            // The key bundle and AAD bind the selected Provider.  Include
            // that identity in the exact Data name as well, otherwise a
            // multi-Provider collaboration would race several ciphertexts
            // under one name and each consumer could fetch another
            // Provider's envelope.
            // SegmentFetcher uses the penultimate component as the versioned
            // object prefix.  Keep the version deterministic from the bound
            // attempt so the Provider can reconstruct the exact name before
            // it unwraps the Selection key envelope.
            .append("PROVIDER").append(providerName).appendVersion(attempt);
        return dataName;
    }

    ndn::Name ServiceUser::startRequestServiceWithRequestId(
        const ndn::Name& requestId,
        const std::vector<ndn::Name>& providers,
        const ndn::Name& serviceName,
        ndn_service_framework::RequestMessage requestMessage,
        int timeoutMs,
        TimeoutHandler onTimeout,
        ResponseHandler onResponseHandler,
        size_t strategy,
        bool trackSelectionStatus,
        SelectionStatusTimeoutHandler statusTimeoutHandler,
        SelectionStatusOptions statusOptions)
    {
        // The base RequestMessage overload enters here directly, while the
        // typed/Targeted wrappers also prepare before reaching this boundary.
        // Keep every publication path behind the same readiness/status gate.
        if (!prepareRequestControllerVersion(requestMessage, serviceName, requestId)) {
            return ndn::Name();
        }
        if (!hasUserPermissionForRequest(providers, serviceName)) {
            NDN_LOG_ERROR("Reject request without user permission serviceName="
                          << serviceName.toUri());
            return ndn::Name();
        }
        ndn::Buffer requestScopedPlaintext;
        const bool requestScopedConfidentiality = prepareRequestScopedRequest(
            requestMessage, serviceName, requestId, requestScopedPlaintext);
        auto selectionGatedInputKey = requestScopedConfidentiality ? ndn::Buffer() :
            prepareSelectionGatedInput(requestMessage, serviceName, requestId);
        PendingCall pendingCall;
        pendingCall.providers = providers;
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
        pendingCall.selectionGatedInputKey = std::move(selectionGatedInputKey);
        pendingCall.requestScopedPlaintext = std::move(requestScopedPlaintext);
        pendingCall.requestScopedConfidentiality = requestScopedConfidentiality;
        pendingCall.strategy = strategy;
        pendingCall.timeoutMs = timeoutMs;
        const bool collectAllAcks = strategy == ndn_service_framework::tlv::AllSelected;
        // This overload has no ACK-window argument. Leave time for execution
        // after a bounded collection window; callers needing another window
        // can use the explicit AckSelectionStrategy overload.
        pendingCall.ackTimeoutMs = collectAllAcks ?
            (timeoutMs > 0 ? std::clamp(timeoutMs / 4, 1, 1000) : 1000) : 0;
        pendingCall.createdAtUs = nowMicroseconds();
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        pendingCall.trackSelectionStatus = trackSelectionStatus;
        pendingCall.statusTimeoutHandler = std::move(statusTimeoutHandler);
        pendingCall.selectionStatusOptions = statusOptions;
        pendingCall.selectionStatusOptions.queryIntervalMs =
            std::max(1, pendingCall.selectionStatusOptions.queryIntervalMs);
        pendingCall.selectionStatusOptions.queryTimeoutMs =
            std::max(1, pendingCall.selectionStatusOptions.queryTimeoutMs);
        auto streamKeyIt = m_streamEventKeysPending.find(requestId);
        if (streamKeyIt != m_streamEventKeysPending.end()) {
            pendingCall.streamEventKey = std::move(streamKeyIt->second);
            m_streamEventKeysPending.erase(streamKeyIt);
        }
        if (requestMessage.hasStreamRequestOptions()) {
            pendingCall.streamOptions = requestMessage.getStreamRequestOptions();
        }
        m_pendingCalls[requestId] = std::move(pendingCall);
        if (requestMessage.hasStreamRequestOptions()) {
            attachStreamLifecycle(requestId, false);
            if (requestMessage.getRequestMode() == tlv::TargetedRequest &&
                providers.size() == 1 &&
                requestMessage.getStreamRequestOptions().eventKeyGrant) {
                ServiceSelectionMessage targetedBinding;
                targetedBinding.setRequestIDs({requestId.toUri()});
                targetedBinding.setPolicyEpoch(requestMessage.getPolicyEpoch());
                targetedBinding.setStreamEventKeyGrant(
                    *requestMessage.getStreamRequestOptions().eventKeyGrant);
                SelectionProviderEntry providerEntry;
                providerEntry.providerName = providers.front();
                targetedBinding.addProviderEntry(providerEntry);
                if (!initializeStreamConsumer(
                        providers.front(), serviceName, requestId,
                        computeSelectionDigest(targetedBinding))) {
                    m_pendingCalls.erase(requestId);
                    throw std::runtime_error(
                        "failed to initialize Targeted streamed consumer");
                }
                if (const auto consumerIt = m_streamConsumers.find(requestId);
                    consumerIt != m_streamConsumers.end()) {
                    consumerIt->second->prefetchWindow();
                }
            }
        }
        updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri());
        if (m_timelineTrace) {
            logTimelineTrace("user", "request_created", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"attempt", requestMessage.hasStreamRequestOptions() &&
                                  requestMessage.getStreamRequestOptions().attemptEpoch != 0 ?
                                std::to_string(requestMessage.getStreamRequestOptions().attemptEpoch) :
                                "request"}});
        }

        admitOrQueuePendingCall(requestId, collectAllAcks, false);
        return requestId;
    }

    std::shared_ptr<StreamedInvocationSharedState>
    ServiceUser::requestServiceStreamingBytes(
        const ndn::Name& serviceName,
        const ndn::Name& targetProvider,
        ndn::Buffer requestPayload,
        StreamedInvocationOptions options,
        StreamBytesCallback onEvent,
        StreamBytesCallback onComplete,
        std::function<void(const StreamedInvocationError&)> onError,
        size_t strategy)
    {
        if (serviceName.empty() ||
            (options.mode == InvocationMode::Normal && !targetProvider.empty()) ||
            (options.mode == InvocationMode::Targeted && targetProvider.empty())) {
            if (onError) onError({StreamedInvocationErrorCode::InvalidOptions,
                                  "invalid streamed service/target binding"});
            return nullptr;
        }
        if (options.generationId == StreamGenerationId{}) {
            ndn::Buffer generation(sizeof(options.generationId));
            ndn::random::generateSecureBytes(
                ndn::span<uint8_t>(generation.data(), generation.size()));
            std::copy(generation.begin(), generation.end(), options.generationId.begin());
        }
        if (options.streamEpoch == 0) options.streamEpoch = 1;
        if (options.deadlineEpochMs == 0) {
            options.deadlineEpochMs = nowMilliseconds() + 30000;
        }
        if (!options.controllerVersion) {
            options.controllerVersion = getControllerVersion(serviceName);
        }
        const auto requestId = makeRequestId();
        ndn::Buffer eventKey(32);
        ndn::random::generateSecureBytes(
            ndn::span<uint8_t>(eventKey.data(), eventKey.size()));
        options.eventKeyCommitment = computeStreamSha256(
            ndn::span<const uint8_t>(eventKey.data(), eventKey.size()));

        TargetedTokenPair targetedTokenPair;
        bool targetedFastPath = false;
        if (options.mode == InvocationMode::Targeted) {
            // A streamed Targeted call may consume a cached token and prepare
            // its event-key grant before the common Request path runs.  Check
            // the Controller authority first so a revoked User cannot spend
            // or rotate Targeted credentials while preparing a stream.
            if (!authorizeControllerTransition(serviceName,
                                               ProtectedTransition::DISCOVERY)) {
                if (onError) onError({StreamedInvocationErrorCode::Unauthorized,
                                      "targeted streamed request rejected by Controller status"});
                return nullptr;
            }
            if (!hasUserPermissionForProvider(targetProvider, serviceName)) {
                if (onError) onError({StreamedInvocationErrorCode::Unauthorized,
                                      "targeted streamed request lacks user permission"});
                return nullptr;
            }

            TargetedStreamOffer streamOffer;
            {
                const auto poolKey = makeTargetedTokenPoolKey(targetProvider, serviceName);
                std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
                const auto offerIt = m_targetedStreamOffers.find(poolKey);
                if (offerIt != m_targetedStreamOffers.end()) {
                    streamOffer = offerIt->second;
                }
            }
            const bool hasStreamOffer =
                !streamOffer.providerBootEpoch.empty() &&
                !streamOffer.recipientCertName.empty() &&
                !streamOffer.recipientCertDigest.empty() &&
                !streamOffer.recipientPublicKey.empty();
            bool hasStreamToken = !m_useTokens;
            if (hasStreamOffer && m_useTokens) {
                hasStreamToken = popTargetedTokenPair(
                    targetProvider, serviceName, targetedTokenPair);
            }

            // A cache miss is represented by one bounded
            // TargetedBootstrapRequest. Its ACK/Selection produces the
            // recipient offer and token batch; the same stream request then
            // receives the selected Provider's event grant.
            targetedFastPath = hasStreamOffer && hasStreamToken;
            if (!targetedFastPath && !m_useTokens) {
                if (onError) onError({StreamedInvocationErrorCode::Unauthorized,
                                      "targeted streamed bootstrap requires tokens"});
                return nullptr;
            }

            if (targetedFastPath) {
                StreamBinding grantBinding;
                grantBinding.requestId = requestId;
                grantBinding.requester = identity;
                grantBinding.serviceName = serviceName;
                grantBinding.producer = targetProvider;
                grantBinding.producerBootId = streamOffer.providerBootEpoch;
                grantBinding.attemptEpoch = options.attemptEpoch;
                grantBinding.planDigest.fill(0);
                grantBinding.generationId = options.generationId;
                grantBinding.streamEpoch = options.streamEpoch;
                grantBinding.eventKeyCommitment = options.eventKeyCommitment;
                grantBinding.userToken = ndn::Buffer(
                    reinterpret_cast<const uint8_t*>(targetedTokenPair.userToken.data()),
                    targetedTokenPair.userToken.size());
                grantBinding.policyEpoch = getCurrentPolicyEpoch(serviceName);
                grantBinding.controllerVersion = options.controllerVersion;
                grantBinding.deadlineEpochMs = options.deadlineEpochMs;
                try {
                    grantBinding.validate();
                    const auto recipientPublicKey =
                        selectionGatedUnhex(streamOffer.recipientPublicKey);
                    if (recipientPublicKey.empty()) throw std::invalid_argument(
                        "empty targeted stream recipient key");
                    const auto wrapped = wrapSelectionGatedInputKey(
                        eventKey, recipientPublicKey);
                    HybridMessageEnvelope grant;
                    // The grant keyId carries the complete 256-bit binding
                    // digest; V2 compact IDs are not sufficient here.
                    grant.setVersion(1);
                    grant.setAlgorithm("RSA-OAEP");
                    const auto grantDigest = computeStreamGrantBindingDigest(grantBinding);
                    grant.setKeyId(selectionGatedHex(ndn::span<const uint8_t>(
                        grantDigest.data(), grantDigest.size())));
                    grant.setEpochId(std::to_string(options.streamEpoch));
                    grant.setMessageType("STREAM-GRANT");
                    grant.setWrappedMessageKey(wrapped);
                    options.eventKeyGrant = grant.WireEncode();
                }
                catch (const std::exception& error) {
                    if (onError) onError({StreamedInvocationErrorCode::Unauthorized,
                                          std::string("failed to prepare targeted stream grant: ") +
                                              error.what()});
                    return nullptr;
                }
            }
        }
        options.validate();

        RequestMessage requestMessage;
        requestMessage.setPayload(requestPayload, requestPayload.size());
        requestMessage.setStrategy(strategy);
        requestMessage.setStreamRequestOptions(options);
        requestMessage.setPolicyEpoch(getCurrentPolicyEpoch(serviceName));
        if (options.controllerVersion) {
            requestMessage.setControllerVersion(*options.controllerVersion);
        }
        const auto requestMode = options.mode == InvocationMode::Targeted ?
            (targetedFastPath ? tlv::TargetedRequest : tlv::TargetedBootstrapRequest) :
            tlv::NormalRequest;
        requestMessage.setRequestMode(requestMode);
        if (!targetProvider.empty()) requestMessage.setTargetProvider(targetProvider);
        if (options.mode == InvocationMode::Targeted && targetedFastPath && m_useTokens) {
            requestMessage.setUserToken(targetedTokenPair.userToken);
            requestMessage.setProviderToken(targetedTokenPair.providerToken);
        }
        else if (options.mode == InvocationMode::Targeted && !targetedFastPath) {
            requestMessage.setTokens({
                {"targeted.batch_hint", std::to_string(
                    getTargetedTokenBatchHint(targetProvider, serviceName))},
            });
        }
        m_streamEventKeysPending.emplace(requestId, eventKey);
        auto state = std::make_shared<StreamedInvocationSharedState>();
        state->requestId = requestId;
        state->status = StreamedInvocationStatus::Requesting;
        state->onEvent = std::move(onEvent);
        state->onComplete = std::move(onComplete);
        state->onError = std::move(onError);
        // StreamedInvocationHandle::cancel() may be called by a Python
        // callback or another application thread.  ServiceUser's stream
        // maps and consumer lifecycle are owned by the Face/io_context
        // thread, so never mutate them directly from the caller thread.
        state->cancel = [this, requestId] {
            boost::asio::post(m_face.getIoContext(), [this, requestId] {
                cancelStreamRequest(requestId);
            });
        };
        m_streamStates[requestId] = state;

        TimeoutHandler timeout = [state](const ndn::Name&) {
            std::function<void(const StreamedInvocationError&)> onError;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->status == StreamedInvocationStatus::Completed ||
                    state->status == StreamedInvocationStatus::Cancelled) return;
                state->status = StreamedInvocationStatus::Failed;
                onError = state->onError;
            }
            if (onError) onError({StreamedInvocationErrorCode::Deadline,
                                  "streamed invocation deadline expired"});
        };
        ResponseHandler response = [](const ResponseMessage&) {};
        try {
            // startRequestServiceWithRequestId rejects at the admission
            // boundary (revoked Controller status, stale ControllerVersion,
            // missing user permission) by returning an empty request id
            // before any PendingCall exists.  A streamed caller cannot see
            // that rejection asynchronously -- no PendingCall means no
            // timeout, no response, and no error callback -- so surface it
            // as an empty handle instead of a state that can never reach a
            // terminal outcome.
            const auto admittedRequestId = startRequestServiceWithRequestId(
                requestId,
                targetProvider.empty() ? std::vector<ndn::Name>{}
                                       : std::vector<ndn::Name>{targetProvider},
                serviceName, std::move(requestMessage), 5000,
                std::move(timeout), std::move(response), strategy);
            if (admittedRequestId.empty()) {
                m_streamEventKeysPending.erase(requestId);
                m_streamStates.erase(requestId);
                return nullptr;
            }
        }
        catch (const std::exception& error) {
            m_streamEventKeysPending.erase(requestId);
            m_streamStates.erase(requestId);
            if (state->onError) state->onError({StreamedInvocationErrorCode::SelectionFailed,
                                                error.what()});
            return nullptr;
        }
        // Cancellation may be requested from the application callback thread
        // before the Face gets to initialize the selected stream consumer.
        // Install a thread-safe lifecycle fence now that the request-owned
        // lifecycle has been attached; Face-owned map cleanup remains queued
        // in state->cancel below.
        if (const auto lifecycle = getStreamLifecycle(requestId)) {
            state->cancelFence = [lifecycle] {
                lifecycle->user().cancel();
            };
        }
        return state;
    }

    bool ServiceUser::hasUserPermissionForProvider(
        const ndn::Name& providerName,
        const ndn::Name& serviceName) const
    {
        if (providerName.empty() || serviceName.empty()) {
            return false;
        }
        const ndn::Name providerServiceName =
            makePermissionFullServiceName(providerName, serviceName);
        return m_authorizations.contains(providerServiceName.toUri(),
                                         serviceName.toUri(),
                                         tlv::UserPermission);
    }

    bool ServiceUser::hasUserPermissionForRequest(
        const std::vector<ndn::Name>& providers,
        const ndn::Name& serviceName) const
    {
        if (serviceName.empty()) {
            return false;
        }
        if (!providers.empty()) {
            return std::any_of(
                providers.begin(), providers.end(),
                [this, &serviceName] (const ndn::Name& provider) {
                    return hasUserPermissionForProvider(provider, serviceName);
                });
        }
        const auto serviceUri = serviceName.toUri(ndn::name::UriFormat::CANONICAL);
        const auto permissions = m_authorizations.snapshot();
        return std::any_of(
            permissions.begin(), permissions.end(),
            [&serviceUri] (const ServiceAuthorizationRecord& record) {
                return record.permissionKind == tlv::UserPermission &&
                       record.serviceName == serviceUri;
            });
    }

    std::string ServiceUser::makeTargetedTokenPoolKey(
        const ndn::Name& providerName,
        const ndn::Name& serviceName)
    {
        return providerName.toUri() + "|" + serviceName.toUri();
    }

    bool ServiceUser::popTargetedTokenPair(const ndn::Name& providerName,
                                           const ndn::Name& serviceName,
                                           TargetedTokenPair& pair)
    {
        std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
        const auto poolKey = makeTargetedTokenPoolKey(providerName, serviceName);
        auto poolIt = m_targetedTokenPools.find(poolKey);
        if (poolIt == m_targetedTokenPools.end() || poolIt->second.empty()) {
            return false;
        }
        pair = poolIt->second.front();
        poolIt->second.pop_front();
        auto& control = m_targetedTokenPoolControls[poolKey];
        ++control.consumedSinceStore;
        if (poolIt->second.empty()) {
            m_targetedTokenPools.erase(poolIt);
        }
        return !pair.providerToken.empty() && !pair.userToken.empty();
    }

    bool ServiceUser::popProtectedTargetedTokenPair(
        const ndn::Name& providerName, const ndn::Name& serviceName,
        const ControllerVersion& version, TargetedTokenPair& pair,
        TargetedStreamOffer& offer, const std::string& expectedProviderBoot)
    {
        std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
        const auto key = makeTargetedTokenPoolKey(providerName, serviceName);
        const auto cached = m_targetedStreamOffers.find(key);
        if (cached == m_targetedStreamOffers.end() || !cached->second.protectedCarrierAllowed ||
            (!expectedProviderBoot.empty() &&
             cached->second.providerBootEpoch != expectedProviderBoot) ||
            cached->second.controllerVersion != version ||
            cached->second.expiresAtMs <= nowMilliseconds() + 1000) return false;
        try {
            const auto& value = cached->second;
            if (ndn::security::extractIdentityFromCertName(ndn::Name(value.recipientCertName)) != providerName ||
                sha256DigestString(selectionGatedUnhex(value.recipientPublicKey)) != value.recipientCertDigest)
                return false;
        }
        catch (const std::exception&) { return false; }
        auto pool = m_targetedTokenPools.find(key);
        if (pool == m_targetedTokenPools.end()) return false;
        while (!pool->second.empty()) {
            const auto& candidate = pool->second.front();
            if (candidate.protectedExpiresAtMs > nowMilliseconds() + 1000 &&
                candidate.registrationGeneration == cached->second.registrationGeneration &&
                !candidate.providerToken.empty() && !candidate.userToken.empty()) {
                pair = candidate;
                offer = cached->second;
                pool->second.pop_front();
                ++m_targetedTokenPoolControls[key].consumedSinceStore;
                if (pool->second.empty()) m_targetedTokenPools.erase(pool);
                return true;
            }
            pool->second.pop_front(); // Expired/unbound pairs are never resurrected.
        }
        m_targetedTokenPools.erase(pool);
        return false;
    }

    void ServiceUser::storeTargetedTokenPairs(
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn_service_framework::ResponseMessage& responseMessage)
    {
        if (!m_useTokens || providerName.empty() || serviceName.empty() ||
            !responseMessage.getStatus()) {
            return;
        }

        // Ordinary responses never carry a targeted token batch.  Avoid
        // taking the pool mutex or creating an empty per-provider pool on
        // every normal request; this path is only for bootstrap/refill data.
        const auto& tokens = responseMessage.getTokens();
        const auto countIt = tokens.find("targeted.count");
        if (countIt == tokens.end()) {
            return;
        }

        const auto poolKey = makeTargetedTokenPoolKey(providerName, serviceName);
        std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
        const auto publicKeyIt = tokens.find("targeted.recipientPublicKey");
        const auto certNameIt = tokens.find("targeted.recipientCertName");
        const auto certDigestIt = tokens.find("targeted.recipientCertDigest");
        const auto bootEpochIt = tokens.find("targeted.providerBootEpoch");
        if (publicKeyIt != tokens.end() && certNameIt != tokens.end() &&
            certDigestIt != tokens.end() && bootEpochIt != tokens.end() &&
            !publicKeyIt->second.empty() && !certNameIt->second.empty() &&
            !certDigestIt->second.empty() && !bootEpochIt->second.empty()) {
            TargetedStreamOffer offer{
                bootEpochIt->second, certNameIt->second,
                certDigestIt->second, publicKeyIt->second};
            const auto number = [&](const char* field, uint64_t& value) {
                const auto it = tokens.find(field);
                return it != tokens.end() && detail::parseProtectedTargetedUint64(it->second, value);
            };
            const auto scoped = tokens.find("targeted.scopedKeyV1");
            offer.protectedCarrierAllowed = scoped != tokens.end() && scoped->second == "1" &&
                number("targeted.expiresAtMs", offer.expiresAtMs) &&
                number("targeted.registrationGeneration", offer.registrationGeneration) &&
                number("targeted.controllerGeneration", offer.controllerVersion.controllerGenerationTimestamp) &&
                number("targeted.controllerEpoch", offer.controllerVersion.controllerEpoch) &&
                offer.controllerVersion.isValid() && offer.registrationGeneration != 0 &&
                offer.expiresAtMs > nowMilliseconds();
            const auto inlineInput = tokens.find("targeted.inlineSignedInputV1");
            offer.inlineSignedInputAllowed = offer.protectedCarrierAllowed &&
                inlineInput != tokens.end() && inlineInput->second == "1";
            offer.binaryKeyEnvelopeAllowed = detail::supportsBinaryTargetedKeyEnvelope(
                tokens, offer.protectedCarrierAllowed);
            const auto compactEnvelope = tokens.find("targeted.compactKeyEnvelopeV1");
            offer.compactKeyEnvelopeAllowed = offer.protectedCarrierAllowed &&
                compactEnvelope != tokens.end() && compactEnvelope->second == "1";
            const auto profileReference = tokens.find("targeted.securityProfileReferenceV1");
            offer.securityProfileReferenceAllowed = offer.protectedCarrierAllowed &&
                profileReference != tokens.end() && profileReference->second == "1";
            const auto previous = m_targetedStreamOffers.find(poolKey);
            if (previous != m_targetedStreamOffers.end() &&
                (previous->second.providerBootEpoch != offer.providerBootEpoch ||
                 previous->second.recipientCertDigest != offer.recipientCertDigest ||
                 previous->second.registrationGeneration != offer.registrationGeneration ||
                 previous->second.controllerVersion != offer.controllerVersion)) {
                m_targetedTokenPools.erase(poolKey);
            }
            m_targetedStreamOffers[poolKey] = std::move(offer);
        }
        else {
            const auto cached = m_targetedStreamOffers.find(poolKey);
            if (cached != m_targetedStreamOffers.end())
                cached->second.protectedCarrierAllowed = false;
        }
        auto& pool = m_targetedTokenPools[poolKey];
        auto& control = m_targetedTokenPoolControls[poolKey];
        const size_t advertisedCount = parseTargetedTokenBatch(countIt->second, 64);
        size_t stored = 0;
        for (size_t i = 0; i < advertisedCount &&
                            pool.size() < TARGETED_TOKEN_POOL_MAX; ++i) {
            const auto providerKey = "targeted." + std::to_string(i) + ".provider";
            const auto userKey = "targeted." + std::to_string(i) + ".user";
            auto providerIt = tokens.find(providerKey);
            auto userIt = tokens.find(userKey);
            if (providerIt == tokens.end() || userIt == tokens.end()) {
                continue;
            }
            if (providerIt->second.empty() || userIt->second.empty()) {
                continue;
            }
            TargetedTokenPair pair{providerIt->second, userIt->second};
            const auto offer = m_targetedStreamOffers.find(poolKey);
            if (offer != m_targetedStreamOffers.end() && offer->second.protectedCarrierAllowed) {
                pair.protectedExpiresAtMs = offer->second.expiresAtMs;
                pair.registrationGeneration = offer->second.registrationGeneration;
            }
            pool.push_back(std::move(pair));
            ++stored;
        }
        if (stored > 0) {
            const auto storedAtUs = nowMicroseconds();
            if (!control.observed) {
                control.nextBatch = configuredTargetedTokenBatch();
            }
            else if (targetedTokenBatchAdaptiveEnabled() &&
                     control.lastStoredAtUs != 0 &&
                     control.consumedSinceStore > 0 &&
                     storedAtUs > control.lastStoredAtUs) {
                const double elapsedSeconds =
                    static_cast<double>(storedAtUs - control.lastStoredAtUs) / 1'000'000.0;
                const double consumptionRate =
                    static_cast<double>(control.consumedSinceStore) / elapsedSeconds;
                const double refillLatencySeconds =
                    control.refillStartedAtUs != 0 &&
                    storedAtUs > control.refillStartedAtUs ?
                    static_cast<double>(storedAtUs - control.refillStartedAtUs) / 1'000'000.0 :
                    0.0;
                // Keep at least one second of demand covered, while allowing
                // the provider-side cap to bound the resulting wire size.
                const double targetHorizonSeconds =
                    std::max(1.0, 4.0 * refillLatencySeconds);
                const auto estimatedBatch = static_cast<size_t>(std::ceil(
                    std::min<double>(TARGETED_TOKEN_BATCH_MAX,
                                     consumptionRate * targetHorizonSeconds)));
                control.nextBatch = clampTargetedTokenBatch(std::max<size_t>(
                    TARGETED_TOKEN_BATCH_ADAPTIVE_MIN, estimatedBatch));
            }
            else if (!targetedTokenBatchAdaptiveEnabled()) {
                control.nextBatch = configuredTargetedTokenBatch();
            }
            control.capacity = clampTargetedTokenBatch(stored);
            control.observed = true;
            control.consumedSinceStore = 0;
            control.lastStoredAtUs = storedAtUs;
            control.refillStartedAtUs = 0;
            control.refillInFlight = false;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=TARGETED_TOKEN_BATCH_STORED timestamp_us="
                      << storedAtUs
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " count=" << stored
                      << " nextBatch=" << control.nextBatch
                      << " poolDepth=" << pool.size());
        }
    }

    size_t ServiceUser::getTargetedTokenBatchHint(const ndn::Name& providerName,
                                                  const ndn::Name& serviceName)
    {
        std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
        const auto poolKey = makeTargetedTokenPoolKey(providerName, serviceName);
        auto& control = m_targetedTokenPoolControls[poolKey];
        if (control.nextBatch == 0) {
            control.nextBatch = configuredTargetedTokenBatch();
        }
        return clampTargetedTokenBatch(control.nextBatch);
    }

    bool ServiceUser::markTargetedTokenRefillInFlight(const ndn::Name& providerName,
                                                      const ndn::Name& serviceName,
                                                      size_t requestedBatch)
    {
        std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
        auto& control = m_targetedTokenPoolControls[
            makeTargetedTokenPoolKey(providerName, serviceName)];
        if (control.refillInFlight) {
            return false;
        }
        if (control.nextBatch == 0) {
            control.nextBatch = configuredTargetedTokenBatch();
        }
        control.nextBatch = clampTargetedTokenBatch(requestedBatch == 0 ?
                                                    control.nextBatch : requestedBatch);
        control.refillInFlight = true;
        control.refillStartedAtUs = nowMicroseconds();
        return true;
    }

    void ServiceUser::clearTargetedTokenRefill(const ndn::Name& providerName,
                                               const ndn::Name& serviceName)
    {
        std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
        auto& control = m_targetedTokenPoolControls[
            makeTargetedTokenPoolKey(providerName, serviceName)];
        control.refillInFlight = false;
        control.refillStartedAtUs = 0;
    }

    void ServiceUser::maybeRefillTargetedTokenPool(const ndn::Name& providerName,
                                                   const ndn::Name& serviceName)
    {
        size_t requestedBatch = 0;
        {
            std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
            const auto poolKey = makeTargetedTokenPoolKey(providerName, serviceName);
            const auto controlIt = m_targetedTokenPoolControls.find(poolKey);
            const auto poolIt = m_targetedTokenPools.find(poolKey);
            if (controlIt == m_targetedTokenPoolControls.end() ||
                !controlIt->second.observed ||
                controlIt->second.refillInFlight) {
                return;
            }
            const auto capacity = std::max<size_t>(1, controlIt->second.capacity);
            const auto lowWatermark = std::max<size_t>(
                1, std::min<size_t>(8, capacity / 4));
            // popTargetedTokenPair erases an empty deque.  An absent pool is
            // therefore a valid zero-depth state and must trigger refill;
            // treating it as a cache miss silently disables refill exactly
            // when the final one-time token is consumed.
            const size_t poolDepth = poolIt == m_targetedTokenPools.end() ?
                0 : poolIt->second.size();
            if (poolDepth > lowWatermark) {
                return;
            }
            requestedBatch = controlIt->second.nextBatch;
        }

        if (!markTargetedTokenRefillInFlight(providerName, serviceName, requestedBatch)) {
            return;
        }

        RequestMessage refillRequest;
        refillRequest.setTokens({
            {"targeted.refill", "1"},
            {"targeted.batch_hint", std::to_string(requestedBatch)},
        });
        try {
            const auto refillRequestId = RequestServiceTargeted(
                providerName,
                serviceName,
                std::move(refillRequest),
                5000,
                [this, providerName, serviceName](const ndn::Name&) {
                    clearTargetedTokenRefill(providerName, serviceName);
                },
                [this, providerName, serviceName](const ResponseMessage&) {
                    clearTargetedTokenRefill(providerName, serviceName);
                });
            if (refillRequestId.empty()) {
                clearTargetedTokenRefill(providerName, serviceName);
            }
            else {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=TARGETED_TOKEN_REFILL_REQUESTED timestamp_us="
                          << nowMicroseconds()
                          << " providerName=" << providerName.toUri()
                          << " serviceName=" << serviceName.toUri()
                          << " requestId=" << refillRequestId.toUri()
                          << " batchHint=" << requestedBatch);
            }
        }
        catch (...) {
            clearTargetedTokenRefill(providerName, serviceName);
            NDN_LOG_WARN("[NDNSF_TRACE] role=user event=TARGETED_TOKEN_REFILL_FAILED timestamp_us="
                         << nowMicroseconds()
                         << " providerName=" << providerName.toUri()
                         << " serviceName=" << serviceName.toUri());
        }
    }

    ndn::Name ServiceUser::RequestService(const PreparedServiceRequest& ctx,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        return RequestService(ctx,
                          {},
                          std::move(requestMessage),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponseHandler),
                          strategy);
    }

    ndn::Name ServiceUser::RequestService(const PreparedServiceRequest& ctx,
                                      const std::vector<ndn::Name>& providers,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        if (ctx.used || ctx.requestId.empty() || ctx.serviceName.empty()) {
            return ndn::Name();
        }
        ctx.used = true;
        return startRequestServiceWithRequestId(ctx.requestId,
                                           providers,
                                           ctx.serviceName,
                                           std::move(requestMessage),
                                           timeoutMs,
                                           std::move(onTimeout),
                                           std::move(onResponseHandler),
                                           strategy);
    }

    ndn::Name ServiceUser::RequestService(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        return startRequestServiceWithRequestId(makeRequestId(),
                                           providers,
                                           serviceName,
                                           std::move(requestMessage),
                                           timeoutMs,
                                           std::move(onTimeout),
                                           std::move(onResponseHandler),
                                           strategy);
    }

    ndn::Name ServiceUser::RequestServiceTracked(
                                      const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      SelectionStatusTimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy,
                                      SelectionStatusOptions statusOptions)
    {
        auto requestId = makeRequestId();
        return startRequestServiceWithRequestId(
            requestId,
            providers,
            serviceName,
            std::move(requestMessage),
            timeoutMs,
            TimeoutHandler{},
            std::move(onResponseHandler),
            strategy,
            true,
            std::move(onTimeout),
            statusOptions);
    }

    void ServiceUser::prepareRequestScopedRecipient(
        ServiceSelectionMessage& selectionMessage,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const ndn::Name& providerName,
        PendingCall& pendingCall,
        const SelectionInputKeyOffer& offer)
    {
        if (!offer.hasField("recipient") ||
            offer.getField("recipient") != providerName.toUri() ||
            !offer.hasField("recipientCertName") ||
            !offer.hasField("recipientPublicKey") ||
            !offer.hasField("recipientCertDigest") ||
            offer.getField("recipientPublicKey").empty() ||
            offer.getField("recipientCertDigest").empty()) {
            throw std::runtime_error(
                "invalid request-scoped Provider key offer binding");
        }

        const auto nowMs = nowMilliseconds();
        const auto lifetimeMs = static_cast<uint64_t>(std::max(
            1000, pendingCall.timeoutMs > 0 ?
                pendingCall.timeoutMs : 30000));
        const bool compactEnvelope = offer.hasField("compactKeyEnvelopeV1") &&
            offer.getField("compactKeyEnvelopeV1") == "1";
        selectionMessage.setCompactKeyEnvelopeMode(compactEnvelope);
        const bool inlineSelection = pendingCall.requestMessage.getRequestMode() == tlv::NormalRequest &&
            offer.hasField("inlineSignedInputV1") && offer.getField("inlineSignedInputV1") == "optional";
        selectionMessage.setOptionalInlineSignedInput(inlineSelection);
        selectionMessage.clearInlineSignedInputData();
        auto& scopedState =
            pendingCall.requestScopedProviderStates[providerName.toUri()];
        std::optional<AeadEnvelope> encrypted;
        if (!scopedState.keys.isValid(nowMs) ||
            scopedState.binding.serviceName != serviceName ||
            scopedState.binding.requestId != requestId) {
            auto keys = generateRequestKeyBundle(nowMs, nowMs + lifetimeMs);

            const auto userAdvertisement =
                makeEncryptionCertificateAdvertisement(identityCert);
            const auto inputName = makeRequestScopedInputDataName(
                serviceName, requestId, selectionMessage.getAttempt(),
                providerName);
            RequestSecurityBinding binding;
            binding.serviceName = serviceName;
            binding.requestId = requestId;
            binding.attempt = selectionMessage.getAttempt();
            binding.controllerVersion = selectionMessage.getControllerVersion();
            binding.userEncryptionCertName = userAdvertisement.certificateName;
            binding.userEncryptionCertDigest = userAdvertisement.certificateDigest;
            binding.providerEncryptionCertName =
                ndn::Name(offer.getField("recipientCertName"));
            binding.providerEncryptionCertDigest =
                offer.getField("recipientCertDigest");
            binding.selectionDigest =
                computeSelectionDigestWithoutKeyEnvelope(selectionMessage);
            binding.inputDataName = inputName;
            binding.segmentOrEventId = "request-input";

            const auto& plaintext = pendingCall.requestScopedPlaintext;
            // A request-scoped input is encrypted with the fresh
            // Provider-specific invocation key, so the generic
            // service-authorized large-data publisher cannot be used as
            // a drop-in replacement.  Keep small inputs as one exact
            // Data packet, but publish larger inputs as independently
            // authenticated envelopes under the same base name.  The
            // Provider resolves the segment sequence after Selection;
            // no oversized Data packet is ever put on the Face.
            constexpr std::size_t inputChunkBytes = 4096;
            constexpr std::size_t inputMaxPlaintextBytes = 16 * 1024 * 1024;
            constexpr std::size_t inputMaxSegments = 4096;
            if (plaintext.size() > inputMaxPlaintextBytes ||
                (plaintext.size() + inputChunkBytes - 1) / inputChunkBytes >
                    inputMaxSegments) {
                throw std::length_error(
                    "request-scoped input exceeds segmented publication bounds");
            }
            const bool useSegments = plaintext.size() > inputChunkBytes;
            if (!useSegments) {
                binding.segmentOrEventId = "request-input";
                encrypted = encryptRequestContent(
                    keys.inputKey, keys.keyId, binding,
                    ndn::span<const uint8_t>(plaintext.data(), plaintext.size()));
                const auto encryptedWire = encrypted->wireEncode();
                ndn::Buffer encryptedPayload(encryptedWire.data(), encryptedWire.size());
                publishSignedAppData(inputName, encryptedPayload,
                                     ndn::time::milliseconds(lifetimeMs));
                if (detail::hasOptionalInlineTargetedInput(pendingCall.requestMessage) &&
                    offer.hasField("inlineSignedInputV1") &&
                    offer.getField("inlineSignedInputV1") == "optional") {
                    std::shared_ptr<const ndn::Data> signedInput;
                    {
                        std::lock_guard<std::mutex> cacheLock(_cache_mutex);
                        signedInput = m_IMS.find(inputName);
                    }
                    if (signedInput && signedInput->getName() == inputName) {
                        const auto& wire = signedInput->wireEncode();
                        if (wire.size() <= detail::TARGETED_INLINE_INPUT_MAX_BYTES) {
                            ndn::Buffer payload(wire.begin(), wire.end());
                            pendingCall.requestMessage.setPayload(payload, payload.size());
                        }
                    }
                }
            }
            else {
                const auto segmentCount = (plaintext.size() + inputChunkBytes - 1) /
                                          inputChunkBytes;
                for (std::size_t segment = 0; segment < segmentCount; ++segment) {
                    const auto offset = segment * inputChunkBytes;
                    const auto length = std::min(inputChunkBytes, plaintext.size() - offset);
                    binding.segmentOrEventId = "request-input/" +
                                               std::to_string(segment);
                    const auto segmentEnvelope = encryptRequestContent(
                        keys.inputKey, keys.keyId, binding,
                        ndn::span<const uint8_t>(plaintext.data() + offset, length));
                    const auto segmentWire = segmentEnvelope.wireEncode();
                    auto data = std::make_shared<ndn::Data>(
                        ndn::Name(inputName).appendSegment(segment));
                    data->setFreshnessPeriod(ndn::time::milliseconds(lifetimeMs));
                    data->setFinalBlock(ndn::name::Component::fromSegment(segmentCount - 1));
                    data->setContent(ndn::Buffer(segmentWire.begin(), segmentWire.end()));
                    (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain)
                        .sign(*data, m_signingInfo);
                    {
                        std::lock_guard<std::mutex> cacheLock(_cache_mutex);
                        m_IMS.insert(*data, ndn::time::milliseconds(lifetimeMs));
                    }
                    m_face.put(*data);
                }
                NDN_LOG_INFO("NDNSF_REQUEST_SCOPED_INPUT_SEGMENTS requestId="
                             << requestId.toUri()
                             << " providerName=" << providerName.toUri()
                             << " dataName=" << inputName.toUri()
                             << " plaintextBytes=" << plaintext.size()
                             << " chunkBytes=" << inputChunkBytes
                             << " segments=" << segmentCount);
            }
            // The Selection envelope authenticates the stable object
            // binding.  Per-segment AAD is derived only while encrypting
            // each segment and must not leak into the stored base binding.
            binding.segmentOrEventId = "request-input";
            scopedState.keys = keys;
            scopedState.binding = binding;
            scopedState.inputDataName = inputName;
            // Retain the legacy single-state view for non-collaboration
            // callers and older response paths.  New multi-Provider
            // paths use requestScopedProviderStates below.
            pendingCall.requestScopedKeys = keys;
            pendingCall.requestScopedBinding = binding;
            pendingCall.requestScopedInputDataName = inputName;
        }

        auto wrapped = wrapSelectionKeyEnvelope(
            scopedState.keys, scopedState.binding,
            selectionGatedUnhex(offer.getField("recipientPublicKey")), nowMs);
        if (compactEnvelope) wrapped.useCompactBinding();
        selectionMessage.setSelectionKeyEnvelope(wrapped.wireEncode());
        if (inlineSelection) {
            std::shared_ptr<const ndn::Data> input;
            {
                std::lock_guard<std::mutex> cacheLock(_cache_mutex);
                input = m_IMS.find(scopedState.binding.inputDataName);
            }
            if (input && input->getName() == scopedState.binding.inputDataName &&
                input->wireEncode().size() <= 2048)
                selectionMessage.setInlineSignedInputData(input->wireEncode());
        }
        bool allSelectedProvidersPrepared = true;
        if (pendingCall.isCollaboration) {
            for (const auto& participant :
                 pendingCall.collaborationCommittedParticipants) {
                if (pendingCall.requestScopedProviderStates.find(
                        participant.provider.toUri()) ==
                    pendingCall.requestScopedProviderStates.end()) {
                    allSelectedProvidersPrepared = false;
                    break;
                }
            }
        }
        else {
            for (const auto& selectedProvider :
                 pendingCall.expectedResponseProviders) {
                if (pendingCall.requestScopedProviderStates.find(
                        selectedProvider.toUri()) ==
                    pendingCall.requestScopedProviderStates.end()) {
                    allSelectedProvidersPrepared = false;
                    break;
                }
            }
        }
        if (allSelectedProvidersPrepared) {
            pendingCall.requestScopedPlaintext.clear();
        }
        NDN_LOG_INFO("NDNSF_REQUEST_SCOPED_INPUT_PUBLISHED requestId="
                     << requestId.toUri()
                     << " serviceName=" << serviceName.toUri()
                     << " providerName=" << providerName.toUri()
                     << " dataName=" << scopedState.inputDataName.toUri()
                     << " bytes=" << (encrypted ? encrypted->ciphertext.size() : 0)
                     << " stateReused=" << (!encrypted.has_value()));
    }


    ndn::Name ServiceUser::RequestServiceTargeted(const ndn::Name& provider,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler)
    {
        if (provider.empty()) {
            return ndn::Name();
        }
        // Targeted requests bypass ACK/Selection only after a token batch has
        // been bootstrapped, but they still enter the same Controller-governed
        // discovery transition.  Check before consuming a cached token or
        // starting a refill so a revoked User cannot spend authorization
        // material while the fast path is being prepared.
        if (!authorizeControllerTransition(serviceName,
                                           ProtectedTransition::DISCOVERY)) {
            NDN_LOG_ERROR("Reject targeted request under revoked Controller status provider="
                          << provider.toUri()
                          << " serviceName=" << serviceName.toUri());
            return ndn::Name();
        }
        if (requestMessage.hasRequestCapabilities() &&
            (requestMessage.getRequestCapabilities().hasField(detail::TARGETED_KEY_ENVELOPE_FIELD) ||
             requestMessage.getRequestCapabilities().hasField(detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD) ||
             requestMessage.getRequestCapabilities().hasField(detail::TARGETED_COMPACT_KEY_ENVELOPE_FIELD) ||
             requestMessage.getRequestCapabilities().hasField(detail::TARGETED_INLINE_INPUT_FIELD))) {
            NDN_LOG_ERROR("Reject application-supplied protected Targeted key carrier");
            return ndn::Name();
        }
        if (requestMessage.hasRequestCapabilities() &&
            requestMessage.getRequestCapabilities().hasField("SelectionGatedInputV1")) {
            NDN_LOG_ERROR("Reject SelectionGatedInputV1 on Selection-free Targeted path");
            return ndn::Name();
        }
        if (!hasUserPermissionForProvider(provider, serviceName)) {
            NDN_LOG_ERROR("Reject targeted request without user permission provider="
                          << provider.toUri()
                          << " serviceName=" << serviceName.toUri());
            return ndn::Name();
        }

        const ndn::Name requestId = detail::makeTargetedRequestId(provider, makeRequestId());
        // The provider compares the request's ControllerVersion against its
        // installed PolicyStatus and rejects a versionless request as stale
        // (fail-closed authority snapshot).  Attach or verify the version
        // before the request leaves the admission path, exactly like the
        // tracked request paths do.
        if (!prepareRequestControllerVersion(requestMessage, serviceName, requestId)) {
            return ndn::Name();
        }
        ndn::Buffer requestScopedPlaintext;
        const bool requestScopedConfidentiality = prepareRequestScopedRequest(
            requestMessage, serviceName, requestId, requestScopedPlaintext);
        const auto scopedBatchHint = requestMessage.getTokens().find(
            "targeted.scoped_batch_hint");
        if (scopedBatchHint != requestMessage.getTokens().end()) {
            // This caller hint is local to Core. Resolve it only after the
            // actual confidentiality mode is known, preserving non-scoped
            // token caching and an explicit standard batch override.
            auto tokens = requestMessage.getTokens();
            if (requestScopedConfidentiality &&
                tokens.count("targeted.batch_hint") == 0) {
                tokens["targeted.batch_hint"] = scopedBatchHint->second;
            }
            tokens.erase("targeted.scoped_batch_hint");
            requestMessage.setTokens(tokens);
        }
        TargetedTokenPair tokenPair;
        TargetedStreamOffer protectedOffer;
        // This local hint only disables stale token reuse; bootstrap still
        // establishes the authenticated boot, keys and current permission.
        std::string expectedProviderBoot;
        auto tokens = requestMessage.getTokens();
        const auto expectedBoot = tokens.find("targeted.expected_provider_boot");
        if (expectedBoot != tokens.end()) {
            expectedProviderBoot = expectedBoot->second;
            tokens.erase(expectedBoot);
            requestMessage.setTokens(tokens);
        }
        const auto& requestTokens = requestMessage.getTokens();
        const bool forceRefill = requestTokens.find("targeted.refill") !=
                                 requestTokens.end();
        bool refillOwner = false;
        bool hasCachedTargetedToken =
            !requestScopedConfidentiality && !forceRefill &&
            (!m_useTokens || popTargetedTokenPair(provider, serviceName, tokenPair));
        const bool protectedFastPath = requestScopedConfidentiality && m_useTokens && !forceRefill &&
            !requestMessage.hasStreamRequestOptions() && !requestMessage.hasConversationContinuation() &&
            !requestMessage.hasDeploymentIntent() && requestMessage.hasControllerVersion() &&
            detail::targetedRequestProvider(requestId).has_value() &&
            popProtectedTargetedTokenPair(provider, serviceName, requestMessage.getControllerVersion(),
                                         tokenPair, protectedOffer, expectedProviderBoot);
        if (protectedFastPath) {
            hasCachedTargetedToken = true;
            requestMessage.setRequestMode(tlv::TargetedRequest);
            requestMessage.setUserToken(tokenPair.userToken);
            requestMessage.setProviderToken(tokenPair.providerToken);
        }
        else if (requestScopedConfidentiality) {
            // A request-scoped invocation needs a Selection envelope to carry
            // K_input/K_response.  A token-only Targeted fast path has no
            // recipient-bound key carrier, so use the bounded bootstrap/
            // ACK/Selection path for this invocation.  The token batch is
            // still attached to the bootstrap response for later non-scoped
            // Targeted calls; no cached token is consumed here.
            requestMessage.setRequestMode(
                ndn_service_framework::tlv::TargetedBootstrapRequest);
            auto bootstrapTokens = requestMessage.getTokens();
            if (bootstrapTokens.count("targeted.batch_hint") == 0) {
                bootstrapTokens["targeted.batch_hint"] = std::to_string(
                    getTargetedTokenBatchHint(provider, serviceName));
            }
            requestMessage.setTokens(bootstrapTokens);
        }
        else if (hasCachedTargetedToken) {
            requestMessage.setRequestMode(ndn_service_framework::tlv::TargetedRequest);
            if (m_useTokens) {
                requestMessage.setUserToken(tokenPair.userToken);
                requestMessage.setProviderToken(tokenPair.providerToken);
            }
        }
        else {
            if (m_useTokens && !forceRefill) {
                refillOwner = markTargetedTokenRefillInFlight(
                    provider, serviceName, getTargetedTokenBatchHint(provider, serviceName));
            }
            if (forceRefill || refillOwner) {
                requestMessage.setRequestMode(
                    ndn_service_framework::tlv::TargetedBootstrapRequest);
                auto bootstrapTokens = requestMessage.getTokens();
                if (bootstrapTokens.count("targeted.batch_hint") == 0) {
                    bootstrapTokens["targeted.batch_hint"] = std::to_string(
                        getTargetedTokenBatchHint(provider, serviceName));
                }
                requestMessage.setTokens(bootstrapTokens);
            }
            else {
                // Another bootstrap is already in flight for this pool.  Keep
                // the request bounded by using the normal one-Provider path
                // instead of issuing duplicate bootstrap requests.
                requestMessage.setRequestMode(ndn_service_framework::tlv::NormalRequest);
                hasCachedTargetedToken = true;
            }
        }
        requestMessage.setTargetProvider(provider);
        requestMessage.setStrategy(ndn_service_framework::tlv::FirstResponding);

        if (forceRefill) {
            NDN_LOG_WARN("NDNSF_DI_TARGETED_REQUEST_MODE requestId="
                         << requestId.toUri()
                         << " provider=" << provider.toUri()
                         << " requestMode="
                         << static_cast<unsigned int>(requestMessage.getRequestMode())
                         << " forceRefill=" << forceRefill
                         << " cachedToken=" << hasCachedTargetedToken
                         << " refillOwner=" << refillOwner);
        }

        PendingCall pendingCall;
        pendingCall.providers = {provider};
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
        pendingCall.requestScopedPlaintext = std::move(requestScopedPlaintext);
        pendingCall.requestScopedConfidentiality = requestScopedConfidentiality;
        pendingCall.strategy = ndn_service_framework::tlv::FirstResponding;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.createdAtUs = nowMicroseconds();
        if (timeoutMs > 0) {
            pendingCall.requestDeadlineUs =
                pendingCall.createdAtUs + static_cast<uint64_t>(timeoutMs) * 1000;
        }
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        pendingCall.targetedMode = hasCachedTargetedToken &&
                                   requestMessage.getRequestMode() ==
                                       ndn_service_framework::tlv::TargetedRequest;
        const bool requestUsesTargetedFastPath = pendingCall.targetedMode;
        addUniqueName(pendingCall.expectedResponseProviders, provider);
        m_pendingCalls[requestId] = std::move(pendingCall);

        auto insertedCall = m_pendingCalls.find(requestId);
        if (insertedCall != m_pendingCalls.end() && timeoutMs > 0) {
            insertedCall->second.requestTimeoutScheduled = true;
            scheduleRequestTimeout(requestId, timeoutMs);
        }
        if (protectedFastPath && insertedCall != m_pendingCalls.end()) {
            try {
                auto& pending = insertedCall->second;
                // PublishRequestV2 normally fills these fields later. Freeze
                // them before computing the protected Request transcript.
                pending.requestMessage.setStrategy(pending.strategy);
                pending.requestMessage.setPolicyEpoch(getCurrentPolicyEpoch(serviceName));
                pending.requestMessage.setUserEncryptionCertificate(
                    makeEncryptionCertificateAdvertisement(identityCert));
                // The protected fast path has no ACK, so it need not repeat
                // the ACK-reader capability already sent during bootstrap.
                if (protectedOffer.securityProfileReferenceAllowed) {
                    const auto record = ensurePublishedSecurityProfile();
                    pending.requestMessage.setUserSecurityProfile(record->packet->getName(),
                        std::shared_ptr<const EncryptionCertificateAdvertisement>(record, &record->advertisement));
                }
                if (protectedOffer.compactKeyEnvelopeAllowed || protectedOffer.binaryKeyEnvelopeAllowed) {
                    auto capabilities = pending.requestMessage.getRequestCapabilities();
                    // Freeze the encoding profile before deriving the AAD;
                    // only the value is filled after the keys are wrapped.
                    capabilities.setField(protectedOffer.compactKeyEnvelopeAllowed ?
                        detail::TARGETED_COMPACT_KEY_ENVELOPE_FIELD : detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD, "");
                    pending.requestMessage.setRequestCapabilities(capabilities);
                }
                if (protectedOffer.inlineSignedInputAllowed) {
                    auto capabilities = pending.requestMessage.getRequestCapabilities();
                    capabilities.setField(detail::TARGETED_INLINE_INPUT_FIELD, "optional");
                    pending.requestMessage.setRequestCapabilities(capabilities);
                }
                auto binding = detail::makeProtectedTargetedBindingSelection(
                    identity, provider, serviceName, requestId, pending.requestMessage);
                SelectionInputKeyOffer offer;
                offer.setField("recipient", provider.toUri());
                offer.setField("recipientCertName", protectedOffer.recipientCertName);
                offer.setField("recipientCertDigest", protectedOffer.recipientCertDigest);
                offer.setField("recipientPublicKey", protectedOffer.recipientPublicKey);
                if (protectedOffer.inlineSignedInputAllowed)
                    offer.setField("inlineSignedInputV1", "optional");
                if (protectedOffer.compactKeyEnvelopeAllowed)
                    offer.setField("compactKeyEnvelopeV1", "1");
                prepareRequestScopedRecipient(binding, serviceName, requestId, provider, pending, offer);
                auto capabilities = pending.requestMessage.getRequestCapabilities();
                const auto envelope = binding.getSelectionKeyEnvelope();
                const std::string encoded = (protectedOffer.compactKeyEnvelopeAllowed || protectedOffer.binaryKeyEnvelopeAllowed) ?
                    std::string(reinterpret_cast<const char*>(envelope.data()), envelope.size()) :
                    selectionGatedHex(ndn::span<const uint8_t>(envelope.data(), envelope.size()));
                if (encoded.size() > DeploymentControlMessage::MAX_FIELD_VALUE)
                    throw std::length_error("Protected Targeted key carrier exceeds capability bounds");
                capabilities.setField(protectedOffer.compactKeyEnvelopeAllowed ? detail::TARGETED_COMPACT_KEY_ENVELOPE_FIELD :
                    protectedOffer.binaryKeyEnvelopeAllowed ?
                    detail::TARGETED_BINARY_KEY_ENVELOPE_FIELD : detail::TARGETED_KEY_ENVELOPE_FIELD, encoded);
                pending.requestMessage.setRequestCapabilities(capabilities);
                if (pending.requestMessage.WireEncode().size() >
                        detail::TARGETED_INLINE_REQUEST_MAX_BYTES &&
                    detail::hasOptionalInlineTargetedInput(pending.requestMessage)) {
                    // The same signed external object is already published;
                    // optional mode and its authenticated binding remain valid.
                    ndn::Buffer empty;
                    pending.requestMessage.setPayload(empty, 0);
                }
                NDN_LOG_WARN("NDNSF_PROTECTED_TARGETED_REQUEST_READY requestId=" << requestId.toUri()
                    << " provider=" << provider.toUri() << " envelopeBytes=" << envelope.size());
            }
            catch (...) {
                erasePendingCallWithTrace(requestId, m_pendingCalls.find(requestId), "targeted_key_prepare_failed");
                throw; // The spent pair is discarded, never returned to its pool.
            }
        }

        updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=TARGETED_REQUEST_CREATED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << provider.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " fastPath=" << hasCachedTargetedToken);
        if (m_timelineTrace) {
            logTimelineTrace("user", "targeted_request_created", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"providerName", provider.toUri()}});
        }

        try {
            admitOrQueuePendingCall(requestId,
                                    requestMessage.getRequestMode() !=
                                        ndn_service_framework::tlv::TargetedRequest,
                                    false);
        }
        catch (...) {
            if (refillOwner) {
                clearTargetedTokenRefill(provider, serviceName);
            }
            if (protectedFastPath)
                erasePendingCallWithTrace(requestId, m_pendingCalls.find(requestId), "targeted_publish_failed");
            throw;
        }
        if (requestUsesTargetedFastPath && !protectedFastPath) {
            maybeRefillTargetedTokenPool(provider, serviceName);
        }
        return requestId;
    }

    ndn::Name ServiceUser::RequestService(const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        return RequestService({},
                          serviceName,
                          std::move(requestMessage),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponseHandler),
                          strategy);
    }

    ndn::Name ServiceUser::RequestService(const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AcksHandler onAcksHandler,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler)
    {
        if (!hasUserPermissionForRequest({}, serviceName)) {
            NDN_LOG_ERROR("Reject request without user permission serviceName="
                          << serviceName.toUri());
            return ndn::Name();
        }
        const ndn::Name requestId = makeRequestId();
        if (!prepareRequestControllerVersion(requestMessage, serviceName, requestId)) {
            return ndn::Name();
        }
        ndn::Buffer requestScopedPlaintext;
        const bool requestScopedConfidentiality = prepareRequestScopedRequest(
            requestMessage, serviceName, requestId, requestScopedPlaintext);

        PendingCall pendingCall;
        pendingCall.serviceName = serviceName;
        pendingCall.selectionGatedInputKey = requestScopedConfidentiality ?
            ndn::Buffer() : prepareSelectionGatedInput(requestMessage, serviceName, requestId);
        pendingCall.requestMessage = requestMessage;
        pendingCall.requestScopedPlaintext = std::move(requestScopedPlaintext);
        pendingCall.requestScopedConfidentiality = requestScopedConfidentiality;
        pendingCall.strategy = ndn_service_framework::tlv::FirstResponding;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.ackTimeoutMs = ackTimeoutMs;
        pendingCall.createdAtUs = nowMicroseconds();
        pendingCall.acksHandler = std::move(onAcksHandler);
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        m_pendingCalls[requestId] = std::move(pendingCall);
        updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri());
        if (m_timelineTrace) {
            logTimelineTrace("user", "request_created", requestId,
                             {{"serviceName", serviceName.toUri()}});
        }

        admitOrQueuePendingCall(requestId, true, false);
        return requestId;
    }

    ndn::Name ServiceUser::RequestService(const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AckCandidatesHandler onAcksHandler,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler)
    {
        if (!hasUserPermissionForRequest({}, serviceName)) {
            NDN_LOG_ERROR("Reject request without user permission serviceName="
                          << serviceName.toUri());
            return ndn::Name();
        }
        const ndn::Name requestId = makeRequestId();
        if (!prepareRequestControllerVersion(requestMessage, serviceName, requestId)) {
            return ndn::Name();
        }
        ndn::Buffer requestScopedPlaintext;
        const bool requestScopedConfidentiality = prepareRequestScopedRequest(
            requestMessage, serviceName, requestId, requestScopedPlaintext);

        PendingCall pendingCall;
        pendingCall.serviceName = serviceName;
        pendingCall.selectionGatedInputKey = requestScopedConfidentiality ?
            ndn::Buffer() : prepareSelectionGatedInput(requestMessage, serviceName, requestId);
        pendingCall.requestMessage = requestMessage;
        pendingCall.requestScopedPlaintext = std::move(requestScopedPlaintext);
        pendingCall.requestScopedConfidentiality = requestScopedConfidentiality;
        pendingCall.strategy = ndn_service_framework::tlv::RandomSelection;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.ackTimeoutMs = ackTimeoutMs;
        pendingCall.createdAtUs = nowMicroseconds();
        pendingCall.ackCandidatesHandler = std::move(onAcksHandler);
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        m_pendingCalls[requestId] = std::move(pendingCall);
        updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri());
        if (m_timelineTrace) {
            logTimelineTrace("user", "request_created", requestId,
                             {{"serviceName", serviceName.toUri()}});
        }

        admitOrQueuePendingCall(requestId, true, true);
        return requestId;
    }

    ndn::Name ServiceUser::RequestService(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AckCandidatesHandler onAcksHandler,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t requestStrategy,
                                      const RequestId& requestedRequestId)
    {
        if (!hasUserPermissionForRequest(providers, serviceName)) {
            NDN_LOG_ERROR("Reject request without user permission serviceName="
                          << serviceName.toUri());
            return ndn::Name();
        }
        const ndn::Name requestId = requestedRequestId.empty() ?
            makeRequestId() : requestedRequestId;
        if (m_pendingCalls.count(requestId) != 0) {
            throw std::invalid_argument("request ID is already pending");
        }
        if (!prepareRequestControllerVersion(requestMessage, serviceName, requestId)) {
            return ndn::Name();
        }
        ndn::Buffer requestScopedPlaintext;
        const bool requestScopedConfidentiality = prepareRequestScopedRequest(
            requestMessage, serviceName, requestId, requestScopedPlaintext);

        PendingCall pendingCall;
        pendingCall.providers = providers;
        pendingCall.serviceName = serviceName;
        pendingCall.selectionGatedInputKey = requestScopedConfidentiality ?
            ndn::Buffer() : prepareSelectionGatedInput(requestMessage, serviceName, requestId);
        pendingCall.requestMessage = requestMessage;
        pendingCall.requestScopedPlaintext = std::move(requestScopedPlaintext);
        pendingCall.requestScopedConfidentiality = requestScopedConfidentiality;
        pendingCall.strategy = requestStrategy;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.ackTimeoutMs = ackTimeoutMs;
        pendingCall.createdAtUs = nowMicroseconds();
        pendingCall.ackCandidatesHandler = std::move(onAcksHandler);
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        m_pendingCalls[requestId] = std::move(pendingCall);
        updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri());
        if (m_timelineTrace) {
            logTimelineTrace("user", "request_created", requestId,
                             {{"serviceName", serviceName.toUri()}});
        }

        admitOrQueuePendingCall(requestId, true, true);
        return requestId;
    }

    ndn::Name ServiceUser::RequestService(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AckSelectionStrategy selectionStrategy,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      const RequestId& requestedRequestId)
    {
        if (selectionStrategy == AckSelectionStrategy::FirstRespondingSelection) {
            if (!hasUserPermissionForRequest(providers, serviceName)) {
                NDN_LOG_ERROR("Reject request without user permission serviceName="
                              << serviceName.toUri());
                return ndn::Name();
            }
            const ndn::Name requestId = requestedRequestId.empty() ?
                makeRequestId() : requestedRequestId;
            if (m_pendingCalls.count(requestId) != 0) {
                throw std::invalid_argument("request ID is already pending");
            }
            if (!prepareRequestControllerVersion(requestMessage, serviceName, requestId)) {
                return ndn::Name();
            }
            ndn::Buffer requestScopedPlaintext;
            const bool requestScopedConfidentiality = prepareRequestScopedRequest(
                requestMessage, serviceName, requestId, requestScopedPlaintext);

            PendingCall pendingCall;
            pendingCall.providers = providers;
            pendingCall.serviceName = serviceName;
            pendingCall.selectionGatedInputKey = requestScopedConfidentiality ?
                ndn::Buffer() : prepareSelectionGatedInput(requestMessage, serviceName, requestId);
            pendingCall.requestMessage = requestMessage;
            pendingCall.requestScopedPlaintext = std::move(requestScopedPlaintext);
            pendingCall.requestScopedConfidentiality = requestScopedConfidentiality;
            pendingCall.strategy = ndn_service_framework::tlv::FirstResponding;
            pendingCall.timeoutMs = timeoutMs;
            pendingCall.ackTimeoutMs = ackTimeoutMs;
            pendingCall.createdAtUs = nowMicroseconds();
            pendingCall.timeoutHandler = std::move(onTimeout);
            pendingCall.responseHandler = std::move(onResponseHandler);
            const bool r1ReservationSelection =
                usesR1ReservationSelection(pendingCall);
            m_pendingCalls[requestId] = std::move(pendingCall);
            updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " serviceName=" << serviceName.toUri());
            if (m_timelineTrace) {
                logTimelineTrace("user", "request_created", requestId,
                                 {{"serviceName", serviceName.toUri()}});
            }

            admitOrQueuePendingCall(requestId,
                                    r1ReservationSelection,
                                    r1ReservationSelection);
            return requestId;
        }

        auto handler = makeAckSelectionHandler(selectionStrategy);
        if (!handler) {
            return ndn::Name();
        }

        const size_t requestStrategy =
            selectionStrategy == AckSelectionStrategy::AllSelected ?
            ndn_service_framework::tlv::AllSelected :
            ndn_service_framework::tlv::FirstResponding;

        return RequestService(providers,
                          serviceName,
                          std::move(requestMessage),
                          ackTimeoutMs,
                          std::move(handler),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponseHandler),
                          requestStrategy,
                          requestedRequestId);
    }

    ndn::Name ServiceUser::RequestService(
        const ServiceName& service,
        const RequestPayload& request,
        int ackCollectionTimeMs,
        std::shared_ptr<const AckSelectionPolicy> selectionPolicy,
        int timeoutMs,
        ResponseHandler onResponse,
        TimeoutHandler onTimeout,
        const RequestId& requestedRequestId)
    {
        if (!selectionPolicy) {
            selectionPolicy = strategy::FirstResponding;
        }

        if (selectionPolicy.get() == strategy::FirstResponding.get()) {
            ndn_service_framework::RequestMessage requestMessage;
            auto payload = request;
            requestMessage.setPayload(payload, payload.size());
            requestMessage.setStrategy(ndn_service_framework::tlv::FirstResponding);

            return RequestService({},
                              service,
                              std::move(requestMessage),
                              ackCollectionTimeMs,
                              AckSelectionStrategy::FirstRespondingSelection,
                              timeoutMs,
                              std::move(onTimeout),
                              std::move(onResponse),
                              requestedRequestId);
        }

        const size_t requestStrategy = selectionPolicy->requestStrategy();

        ndn_service_framework::RequestMessage requestMessage;
        auto payload = request;
        requestMessage.setPayload(payload, payload.size());
        requestMessage.setStrategy(requestStrategy);

        AckCandidatesHandler handler =
            [selectionPolicy = std::move(selectionPolicy)](
                const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates) {
                std::vector<ndn_service_framework::AckSelectionCandidate> selectedCandidates;
                const auto selectedProviders = selectionPolicy->select(candidates);
                for (const auto& provider : selectedProviders) {
                    for (const auto& candidate : candidates) {
                        if (candidate.providerName.equals(provider) &&
                            candidate.ack.getStatus()) {
                            selectedCandidates.push_back(candidate);
                            break;
                        }
                    }
                }
                return selectedCandidates;
            };

        return RequestService({},
                          service,
                          std::move(requestMessage),
                          ackCollectionTimeMs,
                          std::move(handler),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponse),
                          requestStrategy,
                          requestedRequestId);
    }

    ndn::Name ServiceUser::RequestCollaboration(
        const ServiceName& service,
        const RequestPayload& initialRequest,
        CollaborationPlan plan,
        ResponseHandler onFinalResponse,
        TimeoutHandler onTimeout,
        const RequestId& requestedRequestId)
    {
        if (!plan.participantSelector) {
            return ndn::Name();
        }

        const ndn::Name requestId = requestedRequestId.empty() ?
            makeRequestId() : requestedRequestId;
        if (m_pendingCalls.count(requestId) != 0) {
            throw std::invalid_argument(
                "collaboration request ID is already pending");
        }

        ndn_service_framework::RequestMessage requestMessage;
        auto payload = initialRequest;
        requestMessage.setPayload(payload, payload.size());
        // Collaboration uses an explicit participantSelector after the ACK
        // collection window. Do not mark the request as RandomSelection here:
        // the legacy RandomSelection path installs a hard-coded 100 ms timer
        // and can race the collaboration selector before all required roles
        // have returned ACKs.
        requestMessage.setStrategy(ndn_service_framework::tlv::AllSelected);
        if (!prepareRequestControllerVersion(requestMessage, service, requestId)) {
            return ndn::Name();
        }
        if (!hasUserPermissionForRequest({}, service)) {
            NDN_LOG_ERROR("Reject collaboration request without user permission serviceName="
                          << service.toUri());
            return ndn::Name();
        }

        PendingCall pendingCall;
        pendingCall.serviceName = service;
        pendingCall.requestMessage = std::move(requestMessage);
        pendingCall.strategy = ndn_service_framework::tlv::AllSelected;
        pendingCall.timeoutMs = plan.timeoutMs;
        pendingCall.ackTimeoutMs = plan.ackCollectionTimeMs;
        pendingCall.createdAtUs = nowMicroseconds();
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onFinalResponse);
        pendingCall.isCollaboration = true;
        pendingCall.collaborationPlan = std::move(plan);
        pendingCall.trackSelectionStatus = true;
        pendingCall.selectionStatusOptions = SelectionStatusOptions();
        m_pendingCalls[requestId] = std::move(pendingCall);

        updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=COLLAB_REQUEST_CREATED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << service.toUri());

        admitOrQueuePendingCall(requestId, true, true);
        return requestId;
    }

    void ServiceUser::postToIo(std::function<void()> task) const
    {
        if (!task) {
            throw std::invalid_argument("ServiceUser I/O task must not be empty");
        }
        // No raw ServiceUser capture: the queued task owns exactly the
        // captures supplied by its caller, including any required owner.
        boost::asio::post(m_face.getIoContext(), std::move(task));
    }

    bool ServiceUser::isOnIoThread() const
    {
        return m_face.getIoContext().get_executor().running_in_this_thread();
    }

    ndn::Name ServiceUser::BeginCollaboration(
        const ServiceName& service,
        const RequestPayload& initialRequest,
        int ackCollectionTimeMs,
        int timeoutMs,
        CollaborationAckClosedHandler onAckClosed,
        ResponseHandler onFinalResponse,
        TimeoutHandler onTimeout,
        const RequestId& requestedRequestId)
    {
        return BeginCollaboration(
            service, initialRequest, ackCollectionTimeMs, timeoutMs,
            std::move(onAckClosed), std::move(onFinalResponse),
            std::move(onTimeout), requestedRequestId,
            CollaborationAckCoverageHandler());
    }

    ndn::Name ServiceUser::BeginCollaborationWithProviders(
        const ServiceName& service,
        const RequestPayload& initialRequest,
        int ackCollectionTimeMs,
        int timeoutMs,
        CollaborationAckClosedHandler onAckClosed,
        ResponseHandler onFinalResponse,
        TimeoutHandler onTimeout,
        const RequestId& requestedRequestId,
        CollaborationAckCoverageHandler onAckCoverage,
        const RequestCapabilities& requestCapabilities,
        const std::optional<StreamRequestOptions>& requestedStreamOptions,
        std::function<void(const ndn::Buffer&)> onStreamEvent,
        std::function<void(const ndn::Buffer&)> onStreamComplete,
        std::function<void(const StreamedInvocationError&)> onStreamError,
        const std::vector<ndn::Name>& providerNames)
    {
        if (!onAckClosed || ackCollectionTimeMs <= 0 ||
            timeoutMs <= ackCollectionTimeMs) {
            throw std::invalid_argument(
                "deferred collaboration requires valid ACK and request deadlines");
        }
        if (std::any_of(providerNames.begin(), providerNames.end(),
                        [](const ndn::Name& provider) { return provider.empty(); })) {
            throw std::invalid_argument(
                "deferred collaboration Provider candidates must be non-empty");
        }
        const ndn::Name requestId = requestedRequestId.empty() ?
            makeRequestId() : requestedRequestId;
        if (m_pendingCalls.count(requestId) != 0) {
            throw std::invalid_argument(
                "collaboration request ID is already pending");
        }

        ndn_service_framework::RequestMessage requestMessage;
        auto payload = initialRequest;
        requestMessage.setPayload(payload, payload.size());
        requestMessage.setStrategy(ndn_service_framework::tlv::AllSelected);
        if (!requestCapabilities.getFields().empty()) {
            requestMessage.setRequestCapabilities(requestCapabilities);
        }

        // A streamed collaboration carries the same request-scoped stream
        // options as the ordinary streaming API.  Allocate its event-key
        // commitment before the one Request enters admission so the deferred
        // planner can commit a Selection without creating a second request.
        std::optional<StreamRequestOptions> streamOptions = requestedStreamOptions;
        ndn::Buffer streamEventKey;
        if (streamOptions) {
            if (streamOptions->mode != InvocationMode::Normal) {
                throw std::invalid_argument(
                    "deferred collaboration streaming currently requires Normal mode");
            }
            if (streamOptions->generationId == StreamGenerationId{}) {
                ndn::Buffer generation(sizeof(streamOptions->generationId));
                ndn::random::generateSecureBytes(
                    ndn::span<uint8_t>(generation.data(), generation.size()));
                std::copy(generation.begin(), generation.end(),
                          streamOptions->generationId.begin());
            }
            if (streamOptions->streamEpoch == 0) streamOptions->streamEpoch = 1;
            if (streamOptions->deadlineEpochMs == 0) {
                streamOptions->deadlineEpochMs = nowMilliseconds() +
                    static_cast<uint64_t>(timeoutMs);
            }
            if (!streamOptions->controllerVersion) {
                streamOptions->controllerVersion =
                    getControllerVersion(service);
            }
            streamEventKey.resize(32);
            ndn::random::generateSecureBytes(
                ndn::span<uint8_t>(streamEventKey.data(), streamEventKey.size()));
            streamOptions->eventKeyCommitment = computeStreamSha256(
                ndn::span<const uint8_t>(streamEventKey.data(), streamEventKey.size()));
            streamOptions->validate();
            requestMessage.setStreamRequestOptions(*streamOptions);
        }

        // Deferred collaboration publishes its initial Request only after
        // admission. Bind the current ControllerVersion before that publish,
        // just as the ordinary RequestService paths do; otherwise a Provider
        // correctly rejects the decrypted request as versionless.
        if (!prepareRequestControllerVersion(requestMessage, service, requestId)) {
            throw std::runtime_error(
                "deferred collaboration ControllerVersion is not ready");
        }
        ndn::Buffer requestScopedPlaintext;
        const bool requestScopedConfidentiality = prepareRequestScopedRequest(
            requestMessage, service, requestId, requestScopedPlaintext);

        PendingCall pendingCall;
        pendingCall.providers = providerNames;
        pendingCall.serviceName = service;
        pendingCall.requestMessage = std::move(requestMessage);
        pendingCall.requestScopedPlaintext = std::move(requestScopedPlaintext);
        pendingCall.requestScopedConfidentiality = requestScopedConfidentiality;
        pendingCall.strategy = ndn_service_framework::tlv::AllSelected;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.ackTimeoutMs = ackCollectionTimeMs;
        pendingCall.createdAtUs = nowMicroseconds();
        pendingCall.requestDeadlineUs =
            pendingCall.createdAtUs + static_cast<uint64_t>(timeoutMs) * 1000;
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onFinalResponse);
        pendingCall.isCollaboration = true;
        pendingCall.collaborationDeferred = true;
        pendingCall.collaborationPlan.ackCollectionTimeMs = ackCollectionTimeMs;
        pendingCall.collaborationPlan.timeoutMs = timeoutMs;
        pendingCall.collaborationAckClosedHandler = std::move(onAckClosed);
        pendingCall.collaborationAckCoverageHandler = std::move(onAckCoverage);
        pendingCall.trackSelectionStatus = true;
        pendingCall.selectionStatusOptions = SelectionStatusOptions();
        if (streamOptions) {
            pendingCall.streamOptions = *streamOptions;
            pendingCall.streamEventKey = streamEventKey;
        }
        m_pendingCalls[requestId] = std::move(pendingCall);

        if (streamOptions) {
            auto lifecycle = attachStreamLifecycle(requestId, true);
            (void) lifecycle;
            auto state = std::make_shared<StreamedInvocationSharedState>();
            state->requestId = requestId;
            state->status = StreamedInvocationStatus::Requesting;
            state->onEvent = std::move(onStreamEvent);
            state->onComplete = std::move(onStreamComplete);
            state->onError = std::move(onStreamError);
            state->cancel = [this, requestId] {
                boost::asio::post(m_face.getIoContext(), [this, requestId] {
                    cancelStreamRequest(requestId);
                });
            };
            m_streamStates[requestId] = std::move(state);
        }

        updateRequestLifecycleState(
            requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=COLLAB_DEFERRED_REQUEST_CREATED"
                  << " timestamp_us=" << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << service.toUri());
        admitOrQueuePendingCall(requestId, true, true);
        return requestId;
    }

    ndn::Name ServiceUser::BeginCollaboration(
        const ServiceName& service,
        const RequestPayload& initialRequest,
        int ackCollectionTimeMs,
        int timeoutMs,
        CollaborationAckClosedHandler onAckClosed,
        ResponseHandler onFinalResponse,
        TimeoutHandler onTimeout,
        const RequestId& requestedRequestId,
        CollaborationAckCoverageHandler onAckCoverage,
        const RequestCapabilities& requestCapabilities,
        const std::optional<StreamRequestOptions>& requestedStreamOptions,
        std::function<void(const ndn::Buffer&)> onStreamEvent,
        std::function<void(const ndn::Buffer&)> onStreamComplete,
        std::function<void(const StreamedInvocationError&)> onStreamError)
    {
        return BeginCollaborationWithProviders(
            service, initialRequest, ackCollectionTimeMs, timeoutMs,
            std::move(onAckClosed), std::move(onFinalResponse),
            std::move(onTimeout), requestedRequestId,
            std::move(onAckCoverage), requestCapabilities,
            requestedStreamOptions, std::move(onStreamEvent),
            std::move(onStreamComplete), std::move(onStreamError), {});
    }

    bool ServiceUser::CancelCollaboration(const RequestId& requestId)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end() || !pending->second.isCollaboration) {
            return false;
        }
        cancelStreamRequest(requestId);
        for (auto& entry : pending->second.collaborationScopeKeys) {
            if (!entry.second.empty()) OPENSSL_cleanse(entry.second.data(), entry.second.size());
        }
        if (!pending->second.streamEventKey.empty()) {
            OPENSSL_cleanse(pending->second.streamEventKey.data(), pending->second.streamEventKey.size());
        }
        {
            std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
            m_verifiedCollaborationData.erase(requestId);
            auto keys = m_userCollaborationScopeKeys.find(requestId);
            if (keys != m_userCollaborationScopeKeys.end()) {
                for (auto& entry : keys->second) {
                    if (!entry.second.empty()) OPENSSL_cleanse(entry.second.data(), entry.second.size());
                }
                m_userCollaborationScopeKeys.erase(keys);
            }
        }
        // Use the same cleanup as normal completion: release an admission
        // slot, cancel pending timers and invalidate request-scoped nonces.
        erasePendingCallWithTrace(requestId, pending, "caller_cancelled");
        return true;
    }

    bool ServiceUser::CommitCollaborationPlan(
        const RequestId& requestId,
        const std::string& ackClosedDigest,
        CollaborationPlan plan)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end() ||
            !pending->second.isCollaboration ||
            !pending->second.collaborationDeferred) {
            throw std::invalid_argument(
                "deferred collaboration invocation is unavailable");
        }
        auto& call = pending->second;
        if (!call.collaborationAcksClosed) {
            throw std::logic_error(
                "collaboration plan cannot commit before ACK_CLOSED");
        }
        if (ackClosedDigest.empty() ||
            ackClosedDigest != call.collaborationAckClosedDigest) {
            throw std::invalid_argument(
                "collaboration ACK_CLOSED digest mismatch");
        }
        if (call.requestDeadlineUs > 0 &&
            nowMicroseconds() >= call.requestDeadlineUs) {
            throw std::runtime_error(
                "collaboration plan commit is expired");
        }
        if (!plan.participantSelector || plan.roles.empty()) {
            throw std::invalid_argument(
                "collaboration plan is incomplete");
        }
        if (call.streamOptions) {
            const auto terminalRoleCount = static_cast<size_t>(std::count_if(
                plan.roles.begin(), plan.roles.end(),
                [](const CollaborationRoleSpec& role) {
                    return role.terminalResponseOwner;
                }));
            if (terminalRoleCount != 1) {
                throw std::invalid_argument(
                    "streamed collaboration requires exactly one terminal response owner");
            }
        }
        if (plan.ackCollectionTimeMs != call.ackTimeoutMs ||
            plan.timeoutMs != call.timeoutMs) {
            throw std::invalid_argument(
                "collaboration plan changed invocation deadlines");
        }

        std::vector<AckCandidate> candidates;
        for (const auto& storedAck : call.collaborationClosedAcks) {
            candidates.push_back(makeAckSelectionCandidate(storedAck));
        }
        const auto selected =
            plan.participantSelector->select(candidates, plan.roles);
        std::string validationError;
        if (!validateCollaborationSelection(
                plan, selected, validationError)) {
            throw std::invalid_argument(
                "invalid collaboration plan: " + validationError);
        }
        for (const auto& participant : selected) {
            const bool matched = std::any_of(
                call.collaborationClosedAcks.begin(),
                call.collaborationClosedAcks.end(),
                [&participant](const StoredAck& storedAck) {
                    return storedAck.message.getStatus() &&
                           storedAck.providerName.equals(participant.provider) &&
                           storedAck.serviceName.equals(participant.service) &&
                           storedAck.requestId.equals(participant.ack.requestId) &&
                           ackEquals(storedAck.message, participant.ack.ack);
                });
            if (!matched) {
                throw std::invalid_argument(
                    "collaboration plan selected outside ACK_CLOSED");
            }
            // Validate the complete selection before creating any external
            // assignment objects or marking this deferred plan committed.
            if (!authorizeControllerTransition(participant.service,
                                               ProtectedTransition::SELECTION)) {
                throw std::runtime_error("collaboration Selection authorization rejected");
            }
            if (participant.assignmentPayload.empty() ||
                participant.assignmentPayload.size() > 4 * 1024 * 1024) {
                throw std::length_error("collaboration assignment outside external Data bounds");
            }
        }

        const auto commitDigest = deferredPlanCommitDigest(
            ackClosedDigest, plan, selected);
        if (call.collaborationPlanCommitted) {
            if (commitDigest == call.collaborationCommittedPlanDigest) {
                return true;
            }
            throw std::logic_error(
                "conflicting second collaboration plan commit");
        }

        // Collaboration large-data primitives are encrypted with one key per
        // dependency scope.  Generate these keys exactly once at commit time,
        // after ACK_CLOSED has fixed the selected participants, so a
        // retransmitted Selection reuses the same material and cannot create
        // a second cryptographic state for one durable invocation.
        if (call.collaborationScopeKeys.empty()) {
            std::set<std::string> scopeNames;
            for (const auto& scope : plan.keyScopes) {
                if (!scope.name.empty()) {
                    scopeNames.insert(scope.name);
                }
            }
            for (const auto& dependency : plan.dependencies) {
                if (!dependency.keyScope.empty()) {
                    scopeNames.insert(dependency.keyScope);
                }
            }
            for (const auto& scope : scopeNames) {
                const auto key = decodeSecureStatusKeyHex(
                    generateSecureStatusKeyHex());
                if (key.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                    throw std::runtime_error(
                        "failed to generate collaboration scope key");
                }
                call.collaborationScopeKeys.emplace(scope, key);
            }
        }

        call.collaborationPlan = std::move(plan);
        call.collaborationCommittedParticipants = selected;
        call.collaborationCommittedPlanDigest = commitDigest;
        call.collaborationPlanCommitted = true;
        {
            std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
            m_userCollaborationScopeKeys[requestId] =
                call.collaborationScopeKeys;
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=COLLAB_PLAN_COMMITTED"
                  << " timestamp_us=" << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " ackClosedDigest=" << ackClosedDigest
                  << " planDigest=" << commitDigest
                  << " selectedCount=" << selected.size());
        try {
            if (!evaluateAckSelection(requestId)) {
                throw std::runtime_error(
                    "committed collaboration plan produced no Selection");
            }
        }
        catch (...) {
            // Publication may have partially reached the network. Fail closed
            // instead of leaving selected state that makes a retry look like
            // an idempotently successful commit. A new invocation is required.
            CancelCollaboration(requestId);
            throw;
        }
        return true;
    }

    void ServiceUser::handleResponse(const ndn::Name& requestId,
                                     const ndn::Name& providerName,
                                     const ndn_service_framework::ResponseMessage& responseMessage)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            ++m_runtimeDiagnostics.callbackSkippedNoPending;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=CALLBACK_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " userToken=" << responseMessage.getUserToken());
            return;
        }
        if (isSpec192StatusService(pendingCall->second.serviceName)) {
            traceSpec192StatusResponse("application-callback-entered");
        }

        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=CALLBACK_ATTEMPT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " pendingCall=present"
                  << " timedOut=" << pendingCall->second.timedOut);
        if (pendingCall->second.timedOut) {
            ++m_runtimeDiagnostics.callbackSkippedTimeout;
            ++m_runtimeDiagnostics.responseAfterPendingTimeout;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=CALLBACK_SKIPPED_TIMEOUT timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
            return;
        }
        // A collaboration selects every pipeline participant, but only the
        // final role publishes the user-facing response.  Multi-response
        // completion applies to ordinary AllSelected calls, not collaborations.
        const bool expectMultipleResponses =
            !pendingCall->second.isCollaboration &&
            pendingCall->second.expectedResponseProviders.size() > 1;
        if (pendingCall->second.hasResponse && !expectMultipleResponses) {
            return;
        }
        if (expectMultipleResponses &&
            !containsName(pendingCall->second.expectedResponseProviders, providerName)) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_REJECTED_UNSELECTED_PROVIDER timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri());
            return;
        }
        if (expectMultipleResponses &&
            containsName(pendingCall->second.responseProviders, providerName)) {
            return;
        }
        pendingCall->second.hasResponse = true;
        if (!providerName.empty()) {
            addUniqueName(pendingCall->second.responseProviders, providerName);
        }

        auto responseHandler = pendingCall->second.responseHandler;
        updateRequestLifecycleState(requestId, RequestLifecycleState::CALLBACK_FIRED);
        const bool allExpectedResponsesReceived =
            !expectMultipleResponses ||
            pendingCall->second.responseProviders.size() >=
                pendingCall->second.expectedResponseProviders.size();
        if (allExpectedResponsesReceived) {
            erasePendingCallWithTrace(requestId, pendingCall, "response_callback");
        }

        dispatchResponseHandler(std::move(responseHandler), requestId, responseMessage);
    }

    void ServiceUser::dispatchResponseHandler(ResponseHandler responseHandler,
                                              const ndn::Name& requestId,
                                              ResponseMessage responseMessage)
    {
        if (!responseHandler) {
            return;
        }

        NDN_LOG_TRACE("[ServiceUser] RESPONSE accepted requestId="
                  << requestId.toUri()
                  << " userToken=" << responseMessage.getUserToken());
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=CALLBACK_FIRED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " handlerQueueDepth=" << m_handlerPool.getQueueSize());
        if (m_timelineTrace) {
            logTimelineTrace("user", "callback_start", requestId,
                             {{"handlerQueueDepth",
                               std::to_string(m_handlerPool.getQueueSize())}});
        }

        if (m_handlerPool.getThreadCount() == 0) {
            responseHandler(responseMessage);
            if (m_timelineTrace) {
                logTimelineTrace("user", "callback_done", requestId);
            }
            return;
        }

        const bool queued = m_handlerPool.post(
            [responseHandler = std::move(responseHandler),
             responseMessage = std::move(responseMessage),
             requestId,
             traceEnabled = m_timelineTrace]() mutable {
                responseHandler(responseMessage);
                if (traceEnabled) {
                    logTimelineTrace("user", "callback_done", requestId);
                }
            });
        if (!queued) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=CALLBACK_QUEUE_FULL timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
        }
    }

    bool ServiceUser::handleDecryptedResponse(
        const ndn::Name& requestId,
        const ndn::Name& providerName,
        const ndn_service_framework::ResponseMessage& responseMessage)
    {
        return handleDecryptedResponse(requestId, providerName, responseMessage, false);
    }

    bool ServiceUser::handleDecryptedResponse(
        const ndn::Name& requestId,
        const ndn::Name& providerName,
        const ndn_service_framework::ResponseMessage& responseMessage,
        bool resolvedLargeResponse)
    {
        if (m_pendingCalls.find(requestId) == m_pendingCalls.end()) {
            ++m_runtimeDiagnostics.callbackSkippedNoPending;
            ++m_runtimeDiagnostics.responseAfterPendingTimeout;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPTED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=CALLBACK_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " reason=response_after_pending_erased");
            return false;
        }

        auto pendingCall = m_pendingCalls.find(requestId);
        if (responseMessage.hasProviderSecurityProfile() &&
            !responseMessage.hasResolvedProviderSecurityProfile()) {
            removeName(pendingCall->second.responseDecryptProvidersInFlight, providerName);
            NDN_LOG_ERROR("Reject unresolved Response security profile requestId=" << requestId);
            return false;
        }
        auto releaseResponseDecryptInFlight = [&] {
            removeName(pendingCall->second.responseDecryptProvidersInFlight,
                       providerName);
        };
        const auto scopedStateIt =
            pendingCall->second.requestScopedProviderStates.find(
                providerName.toUri());
        const RequestKeyBundle* scopedKeys = nullptr;
        const RequestSecurityBinding* scopedBinding = nullptr;
        if (scopedStateIt !=
            pendingCall->second.requestScopedProviderStates.end()) {
            scopedKeys = &scopedStateIt->second.keys;
            scopedBinding = &scopedStateIt->second.binding;
        }
        else if (pendingCall->second.requestScopedKeys &&
                 pendingCall->second.requestScopedBinding) {
            // Compatibility for invocations created before the per-Provider
            // state was introduced (and for LocalMock tests that inject the
            // legacy single-provider state directly).
            scopedKeys = &*pendingCall->second.requestScopedKeys;
            scopedBinding = &*pendingCall->second.requestScopedBinding;
        }
        // A protected message carries the ControllerVersion as a hint.  Queue
        // an exact status fetch before enforcing the current transition so a
        // newly started/revoked runtime can converge instead of rejecting the
        // only message that advertises the newer controller state.
        const auto responseVersion = responseMessage.hasControllerVersion() ?
            std::optional<ControllerVersion>(responseMessage.getControllerVersion()) :
            std::nullopt;
        maybeRefreshControllerVersionHint(pendingCall->second.serviceName,
                                          responseVersion);
        if (!authorizeControllerTransition(pendingCall->second.serviceName,
                                           ProtectedTransition::RESPONSE_DELIVERY)) {
            ++m_runtimeDiagnostics.callbackSkippedNoPending;
            NDN_LOG_ERROR("Reject response under revoked Controller status requestId="
                          << requestId.toUri());
            releaseResponseDecryptInFlight();
            return false;
        }
        const auto& expectedUserToken = pendingCall->second.requestMessage.getUserToken();
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_START timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " pendingCall=present");
        if (m_useTokens &&
            (expectedUserToken.empty() ||
             responseMessage.getUserToken() != expectedUserToken)) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_FAILED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " reason=user_token_mismatch"
                      << " expectedTokenPresent=" << !expectedUserToken.empty());
            NDN_LOG_ERROR("Reject response with mismatched UserToken for requestId="
                          << requestId.toUri());
            releaseResponseDecryptInFlight();
            return false;
        }
        if (!isAcceptablePolicyEpoch(pendingCall->second.serviceName,
                                     responseMessage.getPolicyEpoch())) {
            NDN_LOG_ERROR("Reject response with stale policy epoch for requestId="
                          << requestId.toUri()
                          << " receivedEpoch=" << responseMessage.getPolicyEpoch()
                          << " currentEpoch=" << m_currentPolicyEpoch);
            releaseResponseDecryptInFlight();
            return false;
        }
        if (!isAcceptableControllerVersion(pendingCall->second.serviceName,
                                           responseVersion)) {
            NDN_LOG_ERROR("Reject response with stale ControllerVersion for requestId="
                          << requestId.toUri());
            releaseResponseDecryptInFlight();
            return false;
        }
        if (pendingCall->second.requestScopedConfidentiality &&
            scopedBinding &&
            (!responseVersion ||
             *responseVersion != scopedBinding->controllerVersion)) {
            // The response version is also authenticated through the request
            // binding/AAD.  A packet carrying a currently acceptable status
            // must not be allowed to relabel an invocation created under a
            // different ControllerVersion.
            NDN_LOG_ERROR("Reject request-scoped response whose ControllerVersion "
                          "does not match its invocation binding requestId="
                          << requestId.toUri());
            releaseResponseDecryptInFlight();
            return false;
        }
        if (pendingCall->second.targetedMode &&
            !pendingCall->second.expectedResponseProviders.empty() &&
            !containsName(pendingCall->second.expectedResponseProviders, providerName)) {
            NDN_LOG_ERROR("Reject targeted response from unexpected provider requestId="
                          << requestId.toUri()
                          << " provider=" << providerName.toUri());
            releaseResponseDecryptInFlight();
            return false;
        }
        if (!hasUserPermissionForProvider(providerName,
                                          pendingCall->second.serviceName)) {
            NDN_LOG_ERROR("Reject response from provider without user permission requestId="
                          << requestId.toUri()
                          << " provider=" << providerName.toUri()
                          << " serviceName=" << pendingCall->second.serviceName.toUri());
            releaseResponseDecryptInFlight();
            return false;
        }

        // On the wire path the response has already passed the configured
        // Data validator, but the request-scoped binding still needs an
        // explicit transport-owner check.  A response can be delivered by a
        // cache or an application callback after validation; do not allow a
        // valid AEAD ciphertext to be attributed to a different Provider.
        // Direct LocalMock callers intentionally leave transport evidence
        // empty, so retain that test-only compatibility while enforcing the
        // check whenever authenticated Data evidence is present.
        if (pendingCall->second.requestScopedConfidentiality &&
            !responseMessage.getDataName().empty()) {
            const auto& signerCertificate = responseMessage.getSignerCertificate();
            if (signerCertificate.empty()) {
                NDN_LOG_ERROR("Reject request-scoped response without authenticated signer "
                              << "requestId=" << requestId.toUri());
                releaseResponseDecryptInFlight();
                return false;
            }
            try {
                const auto signerIdentity = ndn::security::extractIdentityFromCertName(
                    ndn::Name(signerCertificate));
                if (signerIdentity != providerName) {
                    NDN_LOG_ERROR("Reject request-scoped response signer/provider mismatch "
                                  << "requestId=" << requestId.toUri()
                                  << " provider=" << providerName.toUri());
                    releaseResponseDecryptInFlight();
                    return false;
                }
            }
            catch (const std::exception&) {
                NDN_LOG_ERROR("Reject request-scoped response with invalid signer certificate "
                              << "requestId=" << requestId.toUri());
                releaseResponseDecryptInFlight();
                return false;
            }
        }

        ResponseMessage deliveredResponse(responseMessage);
        if (pendingCall->second.requestScopedConfidentiality) {
            if (resolvedLargeResponse) {
                // The reference-resolution path already authenticated every
                // segment against the invocation binding, the nonce registry,
                // and the reference digest; the delivered payload is the
                // reconstructed plaintext.  Nothing further to decrypt.
            }
            else {
            if (!scopedKeys || !scopedBinding ||
                providerName.empty() ||
                !responseMessage.hasAeadEnvelope()) {
                NDN_LOG_ERROR("Reject request-scoped response without key state or AEAD envelope requestId="
                              << requestId.toUri());
                releaseResponseDecryptInFlight();
                return false;
            }
            const auto& binding = *scopedBinding;
            if (!providerName.empty() && !binding.providerEncryptionCertName.empty()) {
                try {
                    if (ndn::security::extractIdentityFromCertName(
                            binding.providerEncryptionCertName) != providerName) {
                        NDN_LOG_ERROR("Reject request-scoped response from provider not bound by Selection requestId="
                                      << requestId.toUri());
                        releaseResponseDecryptInFlight();
                        return false;
                    }
                }
                catch (const std::exception&) {
                    releaseResponseDecryptInFlight();
                    return false;
                }
            }
            auto responseBinding = binding;
            responseBinding.segmentOrEventId = "response";
            const auto nowMs = nowMilliseconds();
            if (!scopedKeys->isValid(nowMs)) {
                NDN_LOG_ERROR("Reject expired request-scoped response key requestId="
                              << requestId.toUri());
                releaseResponseDecryptInFlight();
                return false;
            }
            AeadEnvelope envelope;
            if (!envelope.wireDecode(responseMessage.getAeadEnvelope())) {
                NDN_LOG_ERROR("Reject malformed request-scoped response envelope requestId="
                              << requestId.toUri());
                releaseResponseDecryptInFlight();
                return false;
            }
            const auto lifetimeMs = std::chrono::milliseconds(
                std::max<uint64_t>(1, scopedKeys->expiresAtMs - nowMs));
            ndn::Buffer plaintext;
            RequestCryptoFailure failure = RequestCryptoFailure::NONE;
            if (!decryptRequestContent(
                    scopedKeys->responseKey,
                    scopedKeys->keyId,
                    responseBinding, envelope, plaintext, &failure)) {
                NDN_LOG_ERROR("Reject request-scoped response AEAD requestId="
                              << requestId.toUri()
                              << " reason=" << requestCryptoFailureName(failure));
                releaseResponseDecryptInFlight();
                return false;
            }
            // Authenticate before consuming the nonce.  Otherwise a forged
            // ciphertext/tag could poison the replay registry and make the
            // subsequent valid response look like a replay.
            if (!m_requestScopedNonceRegistry.reserve(
                    scopedKeys->keyId,
                    ndn::span<const uint8_t>(envelope.nonce.data(), envelope.nonce.size()),
                    lifetimeMs)) {
                NDN_LOG_ERROR("Reject replayed request-scoped response nonce requestId="
                              << requestId.toUri());
                releaseResponseDecryptInFlight();
                return false;
            }
            deliveredResponse.setPayload(plaintext, plaintext.size());
            deliveredResponse.clearAeadEnvelope();
            NDN_LOG_INFO("NDNSF_REQUEST_SCOPED_RESPONSE_DECRYPTED requestId="
                         << requestId.toUri()
                         << " providerName=" << providerName.toUri()
                         << " plaintextBytes=" << plaintext.size());
            }
        }
        const auto& effectiveResponse = deliveredResponse;

        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_DONE timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri());
        storeTargetedTokenPairs(providerName,
                                pendingCall->second.serviceName,
                                effectiveResponse);
        pendingCall->second.responseValidatedAtUs = nowMicroseconds();
        if (pendingCall->second.streamOptions) {
            if (!effectiveResponse.getStatus()) {
                // A provider-side stream failure is still a valid, encrypted
                // Response.  Route it through the streaming error callback
                // instead of treating it as a unary response (the latter
                // would leave the stream handle in Requesting/Streaming).
                const auto stateIt = m_streamStates.find(requestId);
                if (stateIt != m_streamStates.end()) {
                    const auto state = stateIt->second;
                    std::function<void(const StreamedInvocationError&)> onError;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        if (state->status != StreamedInvocationStatus::Completed &&
                            state->status != StreamedInvocationStatus::Cancelled) {
                            state->status = StreamedInvocationStatus::Failed;
                            onError = state->onError;
                        }
                    }
                    try {
                        if (pendingCall->second.streamLifecycle) {
                            pendingCall->second.streamLifecycle->user().fail();
                        }
                    }
                    catch (const std::logic_error&) {
                    }
                    m_streamConsumers.erase(requestId);
                    if (onError) {
                        onError(StreamedInvocationError{
                            StreamedInvocationErrorCode::ProviderFailure,
                            effectiveResponse.getErrorInfo().empty() ?
                                "provider failed streamed invocation" :
                                effectiveResponse.getErrorInfo(),
                            requestId, 0, providerName});
                    }
                }
                releaseResponseDecryptInFlight();
                handleResponse(requestId, providerName, effectiveResponse);
                return true;
            }

            const auto consumer = m_streamConsumers.find(requestId);
            if (consumer == m_streamConsumers.end() ||
                !consumer->second->acceptResponse(effectiveResponse)) {
                NDN_LOG_ERROR("Reject streamed Response before End closure requestId="
                              << requestId.toUri());
                releaseResponseDecryptInFlight();
                return false;
            }
        }
        handleResponse(requestId, providerName, effectiveResponse);
        return true;
    }

    bool ServiceUser::handleDecryptedResponse(const ndn::Name& requestId,
                                              const ndn_service_framework::ResponseMessage& responseMessage)
    {
        return handleDecryptedResponse(requestId, ndn::Name(), responseMessage);
    }

    bool ServiceUser::handleDecryptedResponse(const ndn::Name& requestId,
                                              const ndn::Block& responseBlock)
    {
        ndn_service_framework::ResponseMessage responseMessage;
        if (!responseMessage.WireDecode(responseBlock)) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_FAILED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " reason=wire_decode_failed");
            return false;
        }

        return handleDecryptedResponse(requestId, ndn::Name(), responseMessage);
    }

    std::optional<ResponseNameV2>
    ServiceUser::resolvePendingReplyContext(const ndn::Name& name, const std::string& category,
                                           const ndn::Name& expectedProducer,
                                           bool allowTransport) const
    {
      if (category != "ACK" && category != "RESPONSE") {
        return std::nullopt;
      }
      std::optional<ResponseNameV2> result;
      bool ambiguous = false;
      const auto inspect = [&](const ndn::Name& candidateName) {
        // Producer, NDNSF, category, recipient and service are all nonempty.
        // Pending hits fix the service length and hence the Producer boundary.
        for (size_t start = 5; start < candidateName.size(); ++start) {
          const auto requestId = candidateName.getSubName(start);
          const auto pending = m_pendingCalls.find(requestId);
          if (pending == m_pendingCalls.end()) {
            continue;
          }
          const auto& service = pending->second.serviceName;
          if (start <= service.size() + 3) {
            continue;
          }
          const auto producer = candidateName.getPrefix(start - service.size() - 3);
          if (!expectedProducer.empty() && producer != expectedProducer) {
            continue;
          }
          std::optional<ResponseNameV2> candidate;
          if (category == "ACK") {
            const auto ack = parseRequestAckNameV2(candidateName, producer, service);
            if (ack) {
              candidate = ResponseNameV2{ack->providerName, ack->requesterName,
                                         ack->serviceName, ack->requestId};
            }
          }
          else {
            candidate = parseResponseNameV2(candidateName, producer, service);
          }
          if (!candidate || candidate->requestId != requestId ||
              candidate->requesterName != identity) {
            continue;
          }
          if (result && (result->providerName != candidate->providerName ||
                         result->serviceName != candidate->serviceName ||
                         result->requestId != candidate->requestId)) {
            ambiguous = true;
            return;
          }
          result = std::move(candidate);
        }
      };
      inspect(name);
      if (!ambiguous && allowTransport && name.size() >= 2 &&
          name[-2].isVersion() && name[-1].isSegment()) {
        // Keep both interpretations: typed suffixes may also belong to a
        // caller-supplied request ID. Never prefer one ambiguous invocation.
        inspect(name.getPrefix(-2));
      }
      return ambiguous ? std::nullopt : result;
    }

    std::optional<RequestAckNameV2> ServiceUser::parsePendingAckName(const ndn::Name& name) const
    {
      const auto result = resolvePendingReplyContext(name, "ACK");
      if (!result) {
        return std::nullopt;
      }
      return RequestAckNameV2{result->providerName, result->requesterName,
                               result->serviceName, result->requestId};
    }

    std::optional<ResponseNameV2> ServiceUser::parsePendingResponseName(const ndn::Name& name) const
    {
      return resolvePendingReplyContext(name, "RESPONSE");
    }

    bool ServiceUser::handleDecryptedResponseByName(
        const ndn::Name& responseName,
        const ndn_service_framework::ResponseMessage& responseMessage)
    {
        auto parsedV2 = parsePendingResponseName(responseName);
        if (parsedV2) {
            // The requester component is part of the V2 response name and
            // is the wire-level recipient binding.  Do not route a valid
            // response ciphertext into this User merely because its request
            // ID happens to collide with one of our pending calls.
            if (parsedV2->requesterName != identity) {
                NDN_LOG_ERROR("handleDecryptedResponseByName: response requester "
                              << parsedV2->requesterName
                              << " does not match User identity " << identity);
                return false;
            }
            return handleDecryptedResponse(parsedV2->requestId,
                                           parsedV2->providerName,
                                           responseMessage);
        }

        NDN_LOG_ERROR("handleDecryptedResponseByName: non-V2 response name rejected: "
                      << responseName);
        return false;
    }

    bool ServiceUser::handleDecryptedResponseByName(const ndn::Name& responseName,
                                                    const ndn::Block& responseBlock)
    {
        ndn_service_framework::ResponseMessage responseMessage;
        if (!responseMessage.WireDecode(responseBlock)) {
            return false;
        }

        return handleDecryptedResponseByName(responseName, responseMessage);
    }

    std::optional<ResponseMessage>
    ServiceUser::resolveLargeResponseReferencePayload(
        const ResponseMessage& responseMessage,
        const ndn::Name& responseName,
        const ndn::Name& serviceName,
        std::string& errorMessage)
    {
        const auto reference = parseLargeDataReferencePayload(responseMessage.getPayload());
        if (!reference) {
            return responseMessage;
        }
        if (!reference->encrypted) {
            errorMessage = "large response reference is not encrypted";
            return std::nullopt;
        }
        if (reference->dataName.empty()) {
            errorMessage = "large response reference has empty Data name";
            return std::nullopt;
        }

        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto mutex = std::make_shared<std::mutex>();
        auto cv = std::make_shared<std::condition_variable>();
        auto error = std::make_shared<std::string>();
        auto encodedEnvelope = std::make_shared<ndn::Buffer>();
        const int interestLifetimeMs =
            std::max(50, intEnvOrDefault("NDNSF_RESPONSE_LARGE_INTEREST_LIFETIME_MS", 4000));
        const double fetchInitCwnd = static_cast<double>(
            std::max(1, intEnvOrDefault("NDNSF_RESPONSE_LARGE_FETCH_INIT_CWND", 8)));
        const bool fetchTimingEnabled = isTruthyEnv("NDNSF_RESPONSE_LARGE_FETCH_TIMING");
        const auto fetchStart = std::chrono::steady_clock::now();

        boost::asio::post(m_face.getIoContext(),
            [this,
             dataName = reference->dataName,
             completed,
             mutex,
             cv,
             error,
             encodedEnvelope,
             interestLifetimeMs,
             fetchInitCwnd,
             fetchTimingEnabled,
             fetchStart] {
                ndn::Interest interest(dataName);
                interest.setCanBePrefix(true);
                interest.setMustBeFresh(true);
                interest.setInterestLifetime(ndn::time::milliseconds(interestLifetimeMs));

                try {
                    ndn::SegmentFetcher::Options options;
                    options.probeLatestVersion = false;
                    options.useConstantCwnd = true;
                    options.initCwnd = fetchInitCwnd;
                    options.maxTimeout = ndn::time::seconds(10);
                    options.interestLifetime = ndn::time::milliseconds(interestLifetimeMs);
                    if (fetchTimingEnabled) {
                        NDN_LOG_INFO("NDNSF_RESPONSE_LARGE_FETCH_TIMING event=start"
                                     << " dataName=" << dataName.toUri()
                                     << " interest_lifetime_ms=" << interestLifetimeMs
                                     << " init_cwnd=" << fetchInitCwnd);
                    }
                    // Large response segments are Provider-signed Data.  Use
                    // the same configured trust-schema validator as the
                    // normal SVS/Data path; ValidatorNull here would allow a
                    // forged segment to reach request-scoped decryption.
                    auto transportValidator = validator;
                    auto fetcher = ndn::SegmentFetcher::start(m_face,
                                                               interest,
                                                               transportValidator->getConfiguredValidatorForSegmentFetcher(),
                                                               options);
                    fetcher->onComplete.connect(
                        [completed,
                         mutex,
                         cv,
                         encodedEnvelope,
                         transportValidator,
                         dataName,
                         fetchTimingEnabled,
                         fetchStart,
                         interestLifetimeMs,
                         fetchInitCwnd](ndn::ConstBufferPtr buffer) {
                            {
                                std::lock_guard<std::mutex> lock(*mutex);
                                encodedEnvelope->assign(buffer->begin(), buffer->end());
                                completed->store(true);
                            }
                            if (fetchTimingEnabled) {
                                const auto elapsedMs =
                                    std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - fetchStart).count() /
                                    1000.0;
                                NDN_LOG_INFO("NDNSF_RESPONSE_LARGE_FETCH_TIMING event=complete"
                                             << " dataName=" << dataName.toUri()
                                             << " elapsed_ms=" << elapsedMs
                                             << " encoded_bytes=" << buffer->size()
                                             << " interest_lifetime_ms=" << interestLifetimeMs
                                             << " init_cwnd=" << fetchInitCwnd);
                            }
                            cv->notify_one();
                        });
                    fetcher->onError.connect(
                        [completed,
                         mutex,
                         cv,
                         error,
                         transportValidator,
                         dataName,
                         fetchTimingEnabled,
                         fetchStart,
                         interestLifetimeMs,
                         fetchInitCwnd](uint32_t code, const std::string& msg) {
                            {
                                std::lock_guard<std::mutex> lock(*mutex);
                                *error = "SegmentFetcher error " + std::to_string(code) +
                                         ": " + msg;
                                completed->store(true);
                            }
                            if (fetchTimingEnabled) {
                                const auto elapsedMs =
                                    std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - fetchStart).count() /
                                    1000.0;
                                NDN_LOG_INFO("NDNSF_RESPONSE_LARGE_FETCH_TIMING event=error"
                                             << " dataName=" << dataName.toUri()
                                             << " elapsed_ms=" << elapsedMs
                                             << " code=" << code
                                             << " message=" << msg
                                             << " interest_lifetime_ms=" << interestLifetimeMs
                                             << " init_cwnd=" << fetchInitCwnd);
                            }
                            cv->notify_one();
                        });
                }
                catch (const std::exception& e) {
                    {
                        std::lock_guard<std::mutex> lock(*mutex);
                        *error = std::string("large response fetch/decrypt failed: ") + e.what();
                        completed->store(true);
                    }
                    cv->notify_one();
                }
            });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::unique_lock<std::mutex> lock(*mutex);
        cv->wait_until(lock, deadline, [&completed] { return completed->load(); });
        if (!completed->load()) {
            errorMessage = "large response fetch timed out for " + reference->dataName.toUri();
            return std::nullopt;
        }
        if (!error->empty()) {
            errorMessage = "large response fetch failure for " +
                           reference->dataName.toUri() + ": " + *error;
            return std::nullopt;
        }

        ndn::Name providerName;
        ndn::Name requestId;
        if (auto parsedV2 = parsePendingResponseName(responseName)) {
            providerName = parsedV2->providerName;
            requestId = parsedV2->requestId;
        }
        else {
            errorMessage = "large response reference requires a V2 response name";
            return std::nullopt;
        }

        // Request-scoped large responses are a sequence of independently
        // authenticated AeadEnvelope TLVs.  Do not route them through the
        // legacy service-wide HybridMessageCrypto key carrier: the pending
        // invocation's response key and binding are the only decryption
        // authority for this branch.
        if (reference->keyScope == "request") {
            const auto pendingIt = m_pendingCalls.find(requestId);
            if (pendingIt == m_pendingCalls.end() ||
                !pendingIt->second.requestScopedConfidentiality) {
                errorMessage =
                    "request-scoped large response has no pending invocation key state";
                return std::nullopt;
            }
            const auto scopedStateIt =
                pendingIt->second.requestScopedProviderStates.find(
                    providerName.toUri());
            const RequestKeyBundle* scopedKeys = nullptr;
            const RequestSecurityBinding* scopedBinding = nullptr;
            if (scopedStateIt !=
                pendingIt->second.requestScopedProviderStates.end()) {
                scopedKeys = &scopedStateIt->second.keys;
                scopedBinding = &scopedStateIt->second.binding;
            }
            else if (pendingIt->second.requestScopedKeys &&
                     pendingIt->second.requestScopedBinding) {
                scopedKeys = &*pendingIt->second.requestScopedKeys;
                scopedBinding = &*pendingIt->second.requestScopedBinding;
            }
            if (!scopedKeys || !scopedBinding) {
                errorMessage =
                    "request-scoped large response has no Provider key state";
                return std::nullopt;
            }
            if (!responseMessage.hasControllerVersion() ||
                responseMessage.getControllerVersion() !=
                    scopedBinding->controllerVersion) {
                errorMessage =
                    "request-scoped large response ControllerVersion mismatch";
                return std::nullopt;
            }
            try {
                if (ndn::security::extractIdentityFromCertName(
                        scopedBinding->providerEncryptionCertName) !=
                    providerName) {
                    errorMessage =
                        "request-scoped large response provider certificate mismatch";
                    return std::nullopt;
                }
            }
            catch (const std::exception&) {
                errorMessage =
                    "request-scoped large response provider certificate is invalid";
                return std::nullopt;
            }
            if (scopedBinding->serviceName != serviceName ||
                scopedBinding->requestId != requestId ||
                reference->plaintextSize == 0) {
                errorMessage = "request-scoped large response binding or size is invalid";
                return std::nullopt;
            }

            const auto nowMs = nowMilliseconds();
            if (!scopedKeys->isValid(nowMs)) {
                errorMessage = "request-scoped large response key is expired";
                return std::nullopt;
            }
            const auto lifetimeMs = std::chrono::milliseconds(std::max<uint64_t>(
                1, scopedKeys->expiresAtMs - nowMs));
            ndn::Buffer plaintext;
            std::set<std::string> segmentIds;
            std::vector<ndn::Buffer> authenticatedNonces;
            size_t offset = 0;
            size_t segmentIndex = 0;
            while (offset < encodedEnvelope->size()) {
                auto [parsed, block] = ndn::Block::fromBuffer(ndn::span<const uint8_t>(
                    encodedEnvelope->data() + offset,
                    encodedEnvelope->size() - offset));
                if (!parsed || block.type() != AeadEnvelope::TYPE || block.size() == 0) {
                    errorMessage =
                        "request-scoped large response contains malformed segment envelope";
                    return std::nullopt;
                }
                offset += block.size();
                AeadEnvelope envelope;
                if (!envelope.wireDecode(block)) {
                    errorMessage =
                        "request-scoped large response segment envelope decode failed";
                    return std::nullopt;
                }
                const auto expectedSegment =
                    "response/" + std::to_string(segmentIndex);
                if (envelope.segmentOrEventId != expectedSegment ||
                    !segmentIds.insert(envelope.segmentOrEventId).second) {
                    errorMessage =
                        "request-scoped large response segment identity is invalid";
                    return std::nullopt;
                }
                auto binding = *scopedBinding;
                binding.segmentOrEventId = envelope.segmentOrEventId;
                ndn::Buffer segmentPlaintext;
                RequestCryptoFailure failure = RequestCryptoFailure::NONE;
                if (!decryptRequestContent(
                        scopedKeys->responseKey,
                        scopedKeys->keyId,
                        binding,
                        envelope,
                        segmentPlaintext,
                        &failure)) {
                    errorMessage = std::string(
                        "request-scoped large response segment decryption failed: ") +
                        requestCryptoFailureName(failure);
                    return std::nullopt;
                }
                // Decrypt/authenticate every segment before changing replay
                // state.  A malformed later segment must not consume the
                // earlier nonces and poison a valid retry of the reference.
                authenticatedNonces.push_back(envelope.nonce);
                plaintext.insert(plaintext.end(),
                                 segmentPlaintext.begin(), segmentPlaintext.end());
                ++segmentIndex;
            }
            if (segmentIndex == 0 ||
                plaintext.size() != reference->plaintextSize ||
                (!reference->digest.empty() &&
                 sha256DigestString(plaintext) != reference->digest)) {
                errorMessage = "request-scoped large response size or digest mismatch";
                return std::nullopt;
            }
            if (!m_requestScopedNonceRegistry.reserveBatch(
                    scopedKeys->keyId,
                    authenticatedNonces, lifetimeMs)) {
                errorMessage = "request-scoped large response segment nonce was replayed";
                return std::nullopt;
            }
            ResponseMessage resolved(responseMessage);
            resolved.setPayload(plaintext, plaintext.size());
            resolved.clearAeadEnvelope();
            NDN_LOG_INFO("NDNSF_REQUEST_SCOPED_LARGE_RESPONSE_RESOLVED"
                         << " name=" << reference->dataName.toUri()
                         << " requestId=" << requestId.toUri()
                         << " serviceName=" << serviceName.toUri()
                         << " plaintextBytes=" << plaintext.size()
                         << " segments=" << segmentIndex);
            return resolved;
        }

        // Spec179 migration (T012): the legacy service-wide response-key
        // carrier was removed after the request-scoped default passed its
        // MiniNDN gate.  A large-response reference that does not declare
        // the authenticated request-scoped key scope cannot be resolved any
        // more; fail closed with a typed error instead of consuming a
        // service-wide ABE-wrapped key.
        errorMessage = "large response reference without request-scoped key scope "
                       "(legacy service-wide response-key carrier removed)";
        return std::nullopt;
    }

    void ServiceUser::dispatchDecryptedResponseByName(const ndn::Name& responseName,
                                                      const ndn::Name& requestId,
                                                      const ndn::Buffer& buffer,
                                                      const std::string& dataName,
                                                      const std::string& signerCertificate,
                                                      const std::string& wireDigest,
                                                      const std::string& providerIdentity)
    {
        auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());
        auto decodeAndFinish = [this, responseName, requestId, raw, dataName,
                                signerCertificate, wireDigest,
                                providerIdentity]() mutable {
            ndn_service_framework::ResponseMessage responseMessage;
            try {
                ndn::Block block(ndn::span<const uint8_t>(raw->data(), raw->size()));
                if (!responseMessage.WireDecode(block)) {
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_FAILED timestamp_us="
                              << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " reason=wire_decode_failed");
                    return;
                }
            }
            catch (const std::exception& e) {
                NDN_LOG_ERROR("ResponseMessage decode failed: " << e.what());
                return;
            }
            responseMessage.setAuthenticatedTransportEvidence(
                dataName, signerCertificate, wireDigest, providerIdentity);

            boost::asio::post(m_face.getIoContext(),
                [this, responseName, requestId,
                 raw,
                 responseMessage = std::move(responseMessage)]() mutable {
                    finishDecryptedResponseByName(responseName, requestId,
                                                  std::move(responseMessage));
                });
        };

        if (buffer.size() <= HybridMessageCrypto::INLINE_CRYPTO_MAX_BYTES) {
            decodeAndFinish();
            return;
        }
        if (m_handlerPool.getThreadCount() == 0 ||
            !m_handlerPool.post(std::move(decodeAndFinish))) {
            // Zero-worker runtimes deliberately decode on the Face thread, but
            // they must still pass through finishDecryptedResponseByName().
            // Calling handleDecryptedResponseByName() directly would bypass
            // transparent large-response reference resolution and deliver the
            // compact reference payload to the application.
            ResponseMessage responseMessage;
            try {
                ndn::Block block(buffer);
                if (!responseMessage.WireDecode(block)) {
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_FAILED"
                                  << " timestamp_us=" << nowMicroseconds()
                                  << " requestId=" << requestId.toUri()
                                  << " reason=wire_decode_failed_zero_worker");
                    return;
                }
            }
            catch (const std::exception& e) {
                NDN_LOG_ERROR("ResponseMessage decode failed on zero-worker path: "
                              << e.what());
                return;
            }
            responseMessage.setAuthenticatedTransportEvidence(
                dataName, signerCertificate, wireDigest, providerIdentity);
            finishDecryptedResponseByName(responseName, requestId,
                                          std::move(responseMessage));
        }
    }

    void ServiceUser::finishDecryptedResponseByName(
        const ndn::Name& responseName,
        const ndn::Name& expectedRequestId,
        ndn_service_framework::ResponseMessage responseMessage)
    {
        // Rejection only: never recover an execution route from a terminal
        // name. The caller's ID suffices to skip a stale large reference.
        if (!expectedRequestId.empty() &&
            m_pendingCalls.find(expectedRequestId) == m_pendingCalls.end() &&
            parseLargeDataReferencePayload(responseMessage.getPayload())) {
            ++m_runtimeDiagnostics.responseLargeReferenceSkippedNoPending;
            ++m_runtimeDiagnostics.callbackSkippedNoPending;
            ++m_runtimeDiagnostics.responseAfterPendingTimeout;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_LARGE_REFERENCE_SKIPPED"
                          << " timestamp_us=" << nowMicroseconds()
                          << " requestId=" << expectedRequestId.toUri()
                          << " reason=no_pending_call"
                          << " responseName=" << responseName.toUri());
            return;
        }
        ndn::Name serviceName;
        ndn::Name requestId;
        ndn::Name providerName;
        auto parsedV2 = parsePendingResponseName(responseName);
        if (!parsedV2) {
            NDN_LOG_WARN("Reject unmatched or inactive response: " << responseName);
            return;
        }
        serviceName = parsedV2->serviceName;
        requestId = parsedV2->requestId;
        providerName = parsedV2->providerName;
        if (responseMessage.hasProviderSecurityProfile() &&
            !responseMessage.hasResolvedProviderSecurityProfile()) {
            const auto pending = m_pendingCalls.find(requestId);
            if (pending == m_pendingCalls.end()) return;
            const auto reject = [this, requestId, providerName](const std::string& error) {
                const auto call = m_pendingCalls.find(requestId);
                if (call != m_pendingCalls.end())
                    removeName(call->second.responseDecryptProvidersInFlight, providerName);
                NDN_LOG_ERROR("Reject Response security profile requestId=" << requestId
                              << " reason=" << error);
            };
            const auto& request = pending->second.requestMessage;
            const bool reader = request.hasUserSecurityProfile() ||
                (request.hasRequestCapabilities() &&
                 request.getRequestCapabilities().hasField("SecurityProfileReferenceV1") &&
                 request.getRequestCapabilities().getField("SecurityProfileReferenceV1") == "1");
            if (!reader) { reject("unnegotiated profile reference"); return; }
            const auto now = nowMicroseconds();
            const auto deadline = pending->second.requestDeadlineUs != 0 ?
                pending->second.requestDeadlineUs :
                (pending->second.timeoutMs > 0 ? pending->second.createdAtUs +
                 static_cast<uint64_t>(pending->second.timeoutMs) * 1000 : 0);
            if (deadline != 0 && deadline <= now) {
                reject("invocation deadline expired");
                return;
            }
            maybeRefreshControllerVersionHint(serviceName,
                responseMessage.hasControllerVersion() ?
                    std::optional<ControllerVersion>(responseMessage.getControllerVersion()) : std::nullopt);
            if (!authorizeControllerTransition(serviceName, ProtectedTransition::RESPONSE_DELIVERY)) {
                reject("controller forbids response delivery");
                return;
            }
            try {
                if (!m_securityProfiles)
                    m_securityProfiles = std::make_unique<SecurityProfileStore>(m_face, validator);
                const auto remaining = deadline == 0 ? ndn::time::milliseconds(5000) :
                    ndn::time::milliseconds(std::max<uint64_t>(1, (deadline - now + 999) / 1000));
                const auto reference = responseMessage.getProviderSecurityProfile();
                m_securityProfiles->resolve(reference, providerName, remaining,
                    [this, responseName, requestId, deadline, reject, message = std::move(responseMessage)]
                    (auto record, const auto& error) mutable {
                        if (!record) { reject(error); return; }
                        if (m_pendingCalls.find(requestId) == m_pendingCalls.end()) return;
                        if (deadline != 0 && deadline <= nowMicroseconds()) {
                            reject("invocation deadline expired during profile resolution");
                            return;
                        }
                        try {
                            const auto flags = record->profile.capabilities;
                            const std::map<std::string, std::string> stable{
                                {"targeted.recipientPublicKey", record->publicKeyHex},
                                {"targeted.recipientCertName", record->profile.certificate.getName().toUri()},
                                {"targeted.recipientCertDigest", record->publicKeyDigest},
                                {"targeted.binaryKeyEnvelopeV1", flags & ProducerSecurityProfile::BINARY_TARGETED_ENVELOPE ? "1" : "0"},
                                {"targeted.inlineSignedInputV1", flags & ProducerSecurityProfile::INLINE_SIGNED_INPUT ? "1" : "0"},
                                {"targeted.compactKeyEnvelopeV1", flags & ProducerSecurityProfile::COMPACT_REQUEST ? "1" : "0"},
                                {"targeted.securityProfileReferenceV1", "1"}};
                            message.resolveProviderSecurityProfile(
                                std::shared_ptr<const EncryptionCertificateAdvertisement>(record, &record->advertisement), stable);
                        }
                        catch (const std::exception& failure) { reject(failure.what()); return; }
                        finishDecryptedResponseByName(responseName, requestId, std::move(message));
                    });
            }
            catch (const std::exception& failure) { reject(failure.what()); }
            return;
        }
        if (parseLargeDataReferencePayload(responseMessage.getPayload())) {
            auto pendingIt = m_pendingCalls.find(requestId);
            // Enforce the protected transition before fetching or decrypting
            // any segment.  A revoked User must not spend network fetches or
            // use the request-scoped response key on segments whose delivery
            // would be rejected anyway.
            if (!authorizeControllerTransition(serviceName,
                                               ProtectedTransition::RESPONSE_DELIVERY)) {
                NDN_LOG_ERROR("Reject large response reference under revoked Controller status "
                              << "requestId=" << requestId.toUri()
                              << " serviceName=" << serviceName.toUri());
                return;
            }

            const bool expectMultipleResponses =
                !pendingIt->second.isCollaboration &&
                pendingIt->second.expectedResponseProviders.size() > 1;
            if ((pendingIt->second.hasResponse && !expectMultipleResponses) ||
                (expectMultipleResponses &&
                containsName(pendingIt->second.responseProviders, providerName)) ||
                containsName(pendingIt->second.largeResponseReferenceProvidersInFlight,
                             providerName)) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_LARGE_REFERENCE_SKIPPED"
                              << " timestamp_us=" << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " providerName=" << providerName.toUri()
                              << " reason=duplicate_or_completed"
                              << " responseName=" << responseName.toUri());
                return;
            }
            if (expectMultipleResponses &&
                !containsName(pendingIt->second.expectedResponseProviders, providerName)) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_LARGE_REFERENCE_SKIPPED"
                              << " timestamp_us=" << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " providerName=" << providerName.toUri()
                              << " reason=unexpected_provider"
                              << " responseName=" << responseName.toUri());
                return;
            }
            addUniqueName(pendingIt->second.largeResponseReferenceProvidersInFlight,
                          providerName);
            auto resolveAndFinish =
                [this, responseName, serviceName, requestId, providerName,
                 responseMessage = std::move(responseMessage)]() mutable {
                    std::string error;
                    auto resolved =
                        resolveLargeResponseReferencePayload(responseMessage,
                                                             responseName,
                                                             serviceName,
                                                             error);
                    boost::asio::post(m_face.getIoContext(),
                        [this, responseName, requestId, providerName,
                         resolved = std::move(resolved),
                         responseMessage = std::move(responseMessage),
                         error = std::move(error)]() mutable {
                            if (!resolved) {
                                auto pending = m_pendingCalls.find(requestId);
                                if (pending != m_pendingCalls.end()) {
                                    removeName(
                                        pending->second.largeResponseReferenceProvidersInFlight,
                                        providerName);
                                }
                                NDN_LOG_ERROR("Failed to resolve large response reference: "
                                              << error);
                                ResponseMessage failure(responseMessage);
                                failure.setStatus(false);
                                failure.setErrorInfo("large response reference fetch failed: " +
                                                     error);
                                if (!handleDecryptedResponse(requestId, providerName,
                                                             failure, true)) {
                                    NDN_LOG_DEBUG("OnResponse: no pending async callback for "
                                                  << responseName);
                                }
                                return;
                            }
                            if (!handleDecryptedResponse(requestId, providerName,
                                                         *resolved, true)) {
                                NDN_LOG_DEBUG("OnResponse: no pending async callback for "
                                              << responseName);
                            }
                        });
                };
            if (m_handlerPool.getThreadCount() > 0 &&
                m_handlerPool.post(resolveAndFinish)) {
                return;
            }
            std::thread(resolveAndFinish).detach();
            return;
        }
        if (!handleDecryptedResponseByName(responseName, responseMessage)) {
            NDN_LOG_DEBUG("OnResponse: no pending async callback for " << responseName);
        }
    }

    bool ServiceUser::handleRequestAckByName(
        const ndn::Name& ackName,
        const ndn_service_framework::RequestAckMessage& ackMessage,
        AckAuthenticationEvidence authenticationEvidence)
    {
        auto parsedV2 = parsePendingAckName(ackName);
        if (parsedV2) {
            const auto ackReceiveUs = nowMicroseconds();
            auto completeAckDecrypt = [this, requestId = parsedV2->requestId]() {
                auto pending = m_pendingCalls.find(requestId);
                if (pending == m_pendingCalls.end()) {
                    return false;
                }
                if (shouldTrackAckDecrypt(pending->second) &&
                    pending->second.ackDecryptsInFlight > 0) {
                    --pending->second.ackDecryptsInFlight;
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_DECRYPT_IN_FLIGHT_DONE timestamp_us="
                              << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " inFlight=" << pending->second.ackDecryptsInFlight
                              << " ackWindowExpired=" << pending->second.ackWindowExpired);
                }
                return shouldTrackAckDecrypt(pending->second) &&
                       pending->second.ackWindowExpired &&
                       pending->second.ackDecryptsInFlight == 0 &&
                       !pending->second.providerSelected &&
                       pending->second.selectedProvider.empty();
            };
            logAckMatchAttempt(parsedV2->requestId,
                               ackName,
                               parsedV2->providerName,
                               ackReceiveUs,
                               "decoded_ack");
            auto pendingCall = m_pendingCalls.find(parsedV2->requestId);
            if (pendingCall == m_pendingCalls.end()) {
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-no-pending-after-decode");
                logAckNoPending(parsedV2->requestId,
                                ackName,
                                parsedV2->providerName,
                                ackReceiveUs);
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_IGNORED_NO_PENDING timestamp_us="
                          << ackReceiveUs
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " status=" << ackMessage.getStatus());
                return false;
            }

            // Controller permission allows use of a provider, but does not
            // override the caller's explicit candidate set for this request.
            if (!pendingCall->second.providers.empty() &&
                !containsName(pendingCall->second.providers, parsedV2->providerName)) {
                // An authorized but unselected reader still receives this
                // solicitation. Learn only authenticated, current, request-bound
                // capabilities; it remains excluded from selection below.
                const auto& expectedToken = pendingCall->second.requestMessage.getUserToken();
                if (!m_useTokens || (!expectedToken.empty() &&
                    ackMessage.getUserToken() == expectedToken &&
                    (!ackMessage.getStatus() || !ackMessage.getProviderToken().empty()))) {
                    rememberBasicProfileReader(parsedV2->providerName, parsedV2->serviceName,
                                               ackMessage, authenticationEvidence);
                }
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-provider-not-requested");
                NDN_LOG_WARN("Reject ACK outside requested provider set requestId="
                             << parsedV2->requestId.toUri()
                             << " provider=" << parsedV2->providerName.toUri());
                if (completeAckDecrypt()) {
                    evaluateAckSelection(parsedV2->requestId);
                }
                return false;
            }

            const auto ackVersion = ackMessage.hasControllerVersion() ?
                std::optional<ControllerVersion>(ackMessage.getControllerVersion()) :
                std::nullopt;
            maybeRefreshControllerVersionHint(parsedV2->serviceName, ackVersion);
            if (!authorizeControllerTransition(pendingCall->second.serviceName,
                                               ProtectedTransition::ACK_COLLECTION)) {
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-controller-transition");
                NDN_LOG_ERROR("Reject ACK under revoked Controller status requestId="
                              << parsedV2->requestId.toUri());
                if (completeAckDecrypt()) {
                    evaluateAckSelection(parsedV2->requestId);
                }
                return false;
            }

            if (!hasUserPermissionForProvider(parsedV2->providerName,
                                              pendingCall->second.serviceName)) {
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-provider-permission");
                NDN_LOG_ERROR("Reject ACK from provider without user permission requestId="
                              << parsedV2->requestId.toUri()
                              << " provider=" << parsedV2->providerName.toUri()
                              << " serviceName=" << pendingCall->second.serviceName.toUri());
                if (completeAckDecrypt()) {
                    evaluateAckSelection(parsedV2->requestId);
                }
                return false;
            }

            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_RECEIVED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << parsedV2->requestId.toUri()
                      << " providerName=" << parsedV2->providerName.toUri()
                      << " pendingCall=present"
                      << " status=" << ackMessage.getStatus());
            if (pendingCall->second.firstAckAtUs == 0) {
                pendingCall->second.firstAckAtUs = nowMicroseconds();
                if (pendingCall->second.createdAtUs > 0 &&
                    pendingCall->second.firstAckAtUs >= pendingCall->second.createdAtUs) {
                    m_runtimeDiagnostics.ackLatenciesMs.push_back(
                        static_cast<double>(pendingCall->second.firstAckAtUs -
                                            pendingCall->second.createdAtUs) / 1000.0);
                }
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=FIRST_ACK_OBSERVED timestamp_us="
                          << pendingCall->second.firstAckAtUs
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " status=" << ackMessage.getStatus());
            }

            const auto& expectedUserToken =
                pendingCall->second.requestMessage.getUserToken();
            if (m_useTokens &&
                (expectedUserToken.empty() ||
                 ackMessage.getUserToken() != expectedUserToken)) {
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-user-token");
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_REJECTED_USER_TOKEN timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " expectedTokenPresent=" << !expectedUserToken.empty());
                NDN_LOG_ERROR("Reject ACK with mismatched UserToken for requestId="
                              << parsedV2->requestId.toUri()
                              << " provider=" << parsedV2->providerName.toUri());
                if (completeAckDecrypt()) {
                    evaluateAckSelection(parsedV2->requestId);
                }
                return false;
            }
            if (!isAcceptablePolicyEpoch(pendingCall->second.serviceName,
                                         ackMessage.getPolicyEpoch())) {
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-policy-epoch");
                NDN_LOG_ERROR("Reject ACK with stale policy epoch for requestId="
                              << parsedV2->requestId.toUri()
                              << " provider=" << parsedV2->providerName.toUri()
                              << " receivedEpoch=" << ackMessage.getPolicyEpoch()
                              << " currentEpoch=" << m_currentPolicyEpoch);
                if (completeAckDecrypt()) {
                    evaluateAckSelection(parsedV2->requestId);
                }
                return false;
            }
            if (!isAcceptableControllerVersion(pendingCall->second.serviceName,
                                               ackVersion)) {
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-controller-version");
                NDN_LOG_ERROR("Reject ACK with stale ControllerVersion for requestId="
                              << parsedV2->requestId.toUri()
                              << " provider=" << parsedV2->providerName.toUri());
                if (completeAckDecrypt()) {
                    evaluateAckSelection(parsedV2->requestId);
                }
                return false;
            }

            if (m_useTokens &&
                ackMessage.getStatus() && ackMessage.getProviderToken().empty()) {
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-provider-token");
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_REJECTED_PROVIDER_TOKEN timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri());
                NDN_LOG_ERROR("Reject ACK missing ProviderToken for requestId="
                              << parsedV2->requestId.toUri()
                              << " provider=" << parsedV2->providerName.toUri());
                if (completeAckDecrypt()) {
                    evaluateAckSelection(parsedV2->requestId);
                }
                return false;
            }

            rememberBasicProfileReader(parsedV2->providerName, parsedV2->serviceName,
                                       ackMessage, authenticationEvidence);
            const bool collectResponseRetryCandidate =
                pendingCall->second.responseRetryEnabled &&
                pendingCall->second.strategy == ndn_service_framework::tlv::FirstResponding &&
                !pendingCall->second.targetedMode &&
                !pendingCall->second.isCollaboration &&
                !pendingCall->second.acksHandler &&
                !pendingCall->second.ackCandidatesHandler &&
                pendingCall->second.providerSelected &&
                !pendingCall->second.selectedProvider.empty() &&
                !pendingCall->second.selectedProvider.equals(parsedV2->providerName);
            if ((pendingCall->second.providerSelected ||
                 !pendingCall->second.selectedProvider.empty()) &&
                !collectResponseRetryCandidate) {
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-provider-already-selected");
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_IGNORED_PROVIDER_SELECTED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " selectedProvider="
                          << (pendingCall->second.selectedProvider.empty() ?
                              "-" : pendingCall->second.selectedProvider.toUri())
                          << " status=" << ackMessage.getStatus());
                return false;
            }

            const bool duplicateAck =
                std::any_of(pendingCall->second.requestAcks.begin(),
                            pendingCall->second.requestAcks.end(),
                            [&] (const StoredAck& storedAck) {
                                return storedAck.providerName.equals(parsedV2->providerName) &&
                                       storedAck.serviceName.equals(parsedV2->serviceName) &&
                                       storedAck.requestId.equals(parsedV2->requestId);
                            });
            if (duplicateAck) {
                traceSpec192StatusAck(parsedV2->serviceName,
                                      "ack-reject-reason-duplicate");
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_REPLAY_IGNORED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " serviceName=" << parsedV2->serviceName.toUri());
                if (completeAckDecrypt()) {
                    evaluateAckSelection(parsedV2->requestId);
                }
                return false;
            }

            const auto phaseAttempt = pendingCall->second.streamOptions &&
                pendingCall->second.streamOptions->attemptEpoch != 0 ?
                std::to_string(pendingCall->second.streamOptions->attemptEpoch) :
                std::string("request");
            // All authenticated ACK fields and replay/provider checks have
            // passed, but the ACK has not yet been inserted or allowed to
            // trigger ACK_CLOSED/Selection.  This is the actual verification
            // boundary for the phase timeline.
            logPhaseTiming(
                "user", "ackVerified", parsedV2->requestId,
                {{"attempt", phaseAttempt},
                 {"providerName", parsedV2->providerName.toUri()},
                 {"executionRole", "user"}});

            pendingCall->second.providerTokens[parsedV2->providerName.toUri()] =
                ackMessage.getProviderToken();
            const uint64_t ackStartUs = pendingCall->second.publishedAtUs != 0 ?
                pendingCall->second.publishedAtUs : pendingCall->second.createdAtUs;
            std::optional<double> measuredAckRttMs;
            uint64_t ackObservedAtMs = 0;
            if (ackStartUs != 0 && ackReceiveUs >= ackStartUs) {
                measuredAckRttMs =
                    static_cast<double>(ackReceiveUs - ackStartUs) / 1000.0;
                ackObservedAtMs = ackReceiveUs / 1000;
                m_networkTelemetry.updateAckRtt(
                    parsedV2->providerName,
                    parsedV2->serviceName,
                    *measuredAckRttMs);
            }
            pendingCall->second.requestAcks.push_back(
                StoredAck{parsedV2->providerName,
                          parsedV2->serviceName,
                          parsedV2->requestId,
                          ackMessage,
                          std::move(authenticationEvidence),
                          measuredAckRttMs,
                          ackObservedAtMs});
            if (collectResponseRetryCandidate && ackMessage.getStatus()) {
                addUniqueName(pendingCall->second.successfulAckProviders,
                              parsedV2->providerName);
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_RETRY_CANDIDATE_STORED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " selectedProvider="
                          << pendingCall->second.selectedProvider.toUri()
                          << " candidateCount="
                          << pendingCall->second.successfulAckProviders.size());
            }
            updateRequestLifecycleState(parsedV2->requestId, RequestLifecycleState::ACK_MATCHED);
            auto& traceRecord = m_pendingCallTraceHistory[parsedV2->requestId];
            if (traceRecord.createdAtUs == 0) {
                traceRecord.createdAtUs = pendingCall->second.createdAtUs;
            }
            traceRecord.matchedAck = true;
            traceRecord.requestName = pendingCall->second.requestName;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_MATCHED_PENDING_CALL timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << parsedV2->requestId.toUri()
                      << " callId=" << parsedV2->requestId.toUri()
                      << " ackName=" << ackName.toUri()
                      << " providerName=" << parsedV2->providerName.toUri()
                      << " serviceName=" << parsedV2->serviceName.toUri()
                      << " ackCount=" << pendingCall->second.requestAcks.size()
                      << " status=" << ackMessage.getStatus()
                      << " providerTokenPresent="
                      << !ackMessage.getProviderToken().empty()
                      << " userTokenMatched=1");
            if (!ackMessage.getStatus()) {
                recordNegativeAck(pendingCall->second,
                                  parsedV2->requestId,
                                  parsedV2->providerName,
                                  ackMessage);
                completeAckDecrypt();
                if (maybeEarlyStopAllKnownProvidersNegative(parsedV2->requestId)) {
                    return true;
                }
                return true;
            }
            const auto& storedAck = pendingCall->second.requestAcks.back();
            if (collectResponseRetryCandidate) {
                if (!pendingCall->second.responseRetryTimerArmed) {
                    retryResponseWithNextProvider(parsedV2->requestId,
                                                  "late_ack_after_attempt_timeout");
                }
                return true;
            }
            if (shouldTrackAckDecrypt(pendingCall->second)) {
                if (pendingCall->second.ackWindowExpired) {
                    if (completeAckDecrypt()) {
                        if (!pendingCall->second.collaborationDeferred) {
                            evaluateAckSelection(parsedV2->requestId);
                        }
                    }
                    return true;
                }
                completeAckDecrypt();
                std::set<ndn::Name> ackProviders;
                for (const auto& ack : pendingCall->second.requestAcks) {
                    ackProviders.insert(ack.providerName);
                }
                const size_t learnedProviderCount =
                    pendingCall->second.learnedAckProviderCountAtPublish;
                bool applicationCoverageSatisfied = false;
                if (pendingCall->second.isCollaboration &&
                    pendingCall->second.collaborationAckCoverageHandler) {
                    std::vector<AckCandidate> coverageCandidates;
                    coverageCandidates.reserve(
                        pendingCall->second.requestAcks.size());
                    for (const auto& ack : pendingCall->second.requestAcks) {
                        coverageCandidates.push_back(
                            makeAckSelectionCandidate(ack));
                    }
                    try {
                        applicationCoverageSatisfied =
                            pendingCall->second.collaborationAckCoverageHandler(
                                coverageCandidates);
                    }
                    catch (const std::exception& error) {
                        // Coverage is an optimization hint.  A failed hook
                        // must preserve the normal ACK deadline path rather
                        // than closing a partial candidate set.
                        NDN_LOG_ERROR(
                            "Collaboration ACK coverage hook failed: "
                            << error.what());
                    }
                    catch (...) {
                        NDN_LOG_ERROR(
                            "Collaboration ACK coverage hook failed");
                    }
                }
                if (!usesR1ReservationSelection(pendingCall->second) &&
                    pendingCall->second.isCollaboration &&
                    (applicationCoverageSatisfied ||
                     collaborationAckRoleCoverageSatisfied(
                         parsedV2->requestId, pendingCall->second))) {
                    pendingCall->second.ackWindowExpired = true;
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_EARLY_COLLAB_ROLE_COVERAGE timestamp_us="
                              << nowMicroseconds()
                              << " requestId=" << parsedV2->requestId.toUri()
                              << " ackProviderCount=" << ackProviders.size()
                              << " roleCount="
                              << pendingCall->second.collaborationPlan.roles.size()
                              << " applicationHook="
                              << applicationCoverageSatisfied);
                    if (pendingCall->second.collaborationDeferred) {
                        // Deferred callers must receive the immutable
                        // ACK_CLOSED snapshot before DI can inspect the
                        // graph, split the model, and commit the plan.
                        handleAckCollectionTimeout(parsedV2->requestId, false);
                    }
                    else {
                        evaluateAckSelection(parsedV2->requestId);
                    }
                }
                else if (!usesR1ReservationSelection(pendingCall->second) &&
                         !pendingCall->second.isCollaboration &&
                         learnedProviderCount > 0 &&
                         ackProviders.size() >= learnedProviderCount) {
                    pendingCall->second.ackWindowExpired = true;
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_EARLY_LEARNED_PROVIDERS timestamp_us="
                              << nowMicroseconds()
                              << " requestId=" << parsedV2->requestId.toUri()
                              << " ackProviderCount=" << ackProviders.size()
                              << " learnedProviderCount="
                              << learnedProviderCount);
                    evaluateAckSelection(parsedV2->requestId);
                }
                return true;
            }
            if (pendingCall->second.ackWindowExpired) {
                if (pendingCall->second.isCollaboration) {
                    evaluateAckSelection(parsedV2->requestId);
                    return true;
                }
                selectLateAckAfterAckTimeout(pendingCall->second, storedAck);
                return true;
            }
            if (usesR1ReservationSelection(pendingCall->second)) {
                // R1 eligibility is frozen only by the one ACK deadline.
                // Never select early merely because a valid ACK arrived.
                completeAckDecrypt();
                return true;
            }
            const bool shouldSelectFirstAck =
                pendingCall->second.strategy == ndn_service_framework::tlv::FirstResponding &&
                !pendingCall->second.isCollaboration &&
                pendingCall->second.selectedProvider.empty() &&
                ackMessage.getStatus();
            evaluateAckSelection(parsedV2->requestId);
            pendingCall = m_pendingCalls.find(parsedV2->requestId);
            if (shouldSelectFirstAck &&
                pendingCall != m_pendingCalls.end() &&
                pendingCall->second.selectedProvider.equals(parsedV2->providerName)) {
                const auto scheduleAtUs = nowMicroseconds();
                pendingCall->second.selectionScheduledAtUs = scheduleAtUs;
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_ELIGIBILITY_CHECK timestamp_us="
                          << scheduleAtUs
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " serviceName=" << parsedV2->serviceName.toUri()
                          << " eligible=1"
                          << " reason=first_ack_selected"
                          << " providerTokenPresent="
                          << (pendingCall->second.providerTokens.find(parsedV2->providerName.toUri()) !=
                              pendingCall->second.providerTokens.end()));
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_SCHEDULE_ATTEMPT timestamp_us="
                          << scheduleAtUs
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " serviceName=" << parsedV2->serviceName.toUri());
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_FAST_PATH timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " serviceName=" << parsedV2->serviceName.toUri());
                PublishServiceSelectionMessageV2(parsedV2->providerName,
                                                    parsedV2->serviceName,
                                                    parsedV2->requestId);
            }
            else if (shouldSelectFirstAck) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_SKIPPED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " serviceName=" << parsedV2->serviceName.toUri()
                          << " reason="
                          << (pendingCall == m_pendingCalls.end() ?
                              "pending_missing_after_selection" : "selected_provider_mismatch"));
            }

            return true;
        }

        NDN_LOG_WARN("handleRequestAckByName: non-V2 ACK name rejected: " << ackName);
        traceSpec192SelectedRequestAckStage("ack-reject-reason-non-v2-name");
        return false;
    }

    bool ServiceUser::handleRequestAckByName(const ndn::Name& ackName,
                                             const ndn::Block& ackBlock)
    {
        ndn_service_framework::RequestAckMessage ackMessage;
        if (!ackMessage.WireDecode(ackBlock)) {
            traceSpec192SelectedRequestAckStage("ack-reject-reason-wire-decode");
            return false;
        }

        return handleRequestAckByName(ackName, ackMessage);
    }

    std::string ServiceUser::sanitizeLargeDataObjectId(const std::string& objectLabel)
    {
        std::string sanitized;
        sanitized.reserve(objectLabel.size());
        for (const char ch : objectLabel) {
            if ((ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.') {
                sanitized.push_back(ch);
            }
            else {
                sanitized.push_back('-');
            }
        }
        return sanitized;
    }

    ndn::Name ServiceUser::makeRequestId()
    {
        std::ostringstream os;
        os << ndn::time::toIsoString(ndn::time::system_clock::now())
           << "-"
           << std::hex
           << ndn::random::generateSecureWord64();
        return ndn::Name(os.str());
    }

    void ServiceUser::recordObservedAckProvider(const ndn::Name& serviceName,
                                                const ndn::Name& providerName,
                                                uint64_t timestampUs)
    {
        if (serviceName.empty() || providerName.empty()) {
            return;
        }

        auto& providers = m_recentAckProvidersByService[serviceName];
        providers[providerName.toUri()] = timestampUs;
        constexpr uint64_t OBSERVATION_WINDOW_US = 60ULL * 1000ULL * 1000ULL;
        for (auto it = providers.begin(); it != providers.end();) {
            if (timestampUs >= it->second &&
                timestampUs - it->second > OBSERVATION_WINDOW_US) {
                it = providers.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    size_t ServiceUser::getRecentAckProviderCount(const ndn::Name& serviceName,
                                                  uint64_t nowUs)
    {
        auto providers = m_recentAckProvidersByService.find(serviceName);
        if (providers == m_recentAckProvidersByService.end()) {
            return 0;
        }

        constexpr uint64_t OBSERVATION_WINDOW_US = 60ULL * 1000ULL * 1000ULL;
        for (auto it = providers->second.begin(); it != providers->second.end();) {
            if (nowUs >= it->second &&
                nowUs - it->second > OBSERVATION_WINDOW_US) {
                it = providers->second.erase(it);
            }
            else {
                ++it;
            }
        }
        return providers->second.size();
    }

    ndn_service_framework::AckSelectionCandidate
    ServiceUser::makeAckSelectionCandidate(const StoredAck& storedAck) const
    {
        ndn_service_framework::AckSelectionCandidate candidate;
        candidate.providerName = storedAck.providerName;
        candidate.serviceName = storedAck.serviceName;
        candidate.requestId = storedAck.requestId;
        candidate.ack = storedAck.message;
        candidate.telemetry =
            m_networkTelemetry.getServicePath(storedAck.providerName,
                                              storedAck.serviceName);
        candidate.authenticationEvidence = storedAck.authenticationEvidence;
        candidate.ackRttMs = storedAck.ackRttMs;
        candidate.ackObservedAtMs = storedAck.ackObservedAtMs;
        return candidate;
    }

    void ServiceUser::recordNegativeAck(
        PendingCall& pendingCall,
        const ndn::Name& requestId,
        const ndn::Name& providerName,
        const ndn_service_framework::RequestAckMessage& ackMessage)
    {
        if (ackMessage.getStatus()) {
            return;
        }

        addUniqueName(pendingCall.negativeAckProviders, providerName);
        pendingCall.negativeAckReasons[providerName.toUri()] = ackMessage.getMessage();
        if (!ackMessage.getMessage().empty() &&
            !negative_ack_reason::isRecommended(ackMessage.getMessage())) {
            NDN_LOG_WARN("Negative ACK uses non-recommended reason code provider="
                         << providerName.toUri()
                         << " requestId=" << requestId.toUri()
                         << " reason=" << ackMessage.getMessage());
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=NEGATIVE_ACK_RECORDED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " reason="
                  << (ackMessage.getMessage().empty() ? "-" : ackMessage.getMessage())
                  << " negativeAckCount=" << pendingCall.negativeAckProviders.size()
                  << " knownProviderCount=" << pendingCall.providers.size());
    }

    bool ServiceUser::maybeEarlyStopAllKnownProvidersNegative(const ndn::Name& requestId)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return false;
        }

        auto& call = pendingCall->second;
        if (call.collaborationDeferred ||
            call.providers.empty() ||
            call.hasResponse ||
            call.timedOut ||
            call.providerSelected ||
            !call.selectedProvider.empty()) {
            return false;
        }

        for (const auto& storedAck : call.requestAcks) {
            if (storedAck.message.getStatus()) {
                return false;
            }
        }

        for (const auto& provider : call.providers) {
            if (!containsName(call.negativeAckProviders, provider)) {
                return false;
            }
        }

        std::string reasons;
        for (const auto& provider : call.providers) {
            if (!reasons.empty()) {
                reasons += ",";
            }
            const auto providerUri = provider.toUri();
            const auto reason = call.negativeAckReasons.find(providerUri);
            reasons += providerUri + "=" +
                       (reason == call.negativeAckReasons.end() ||
                        reason->second.empty() ? "-" : reason->second);
        }

        call.ackWindowExpired = true;
        call.timedOut = true;
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=NEGATIVE_ACK_EARLY_STOP_ALL_KNOWN_PROVIDERS timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " knownProviderCount=" << call.providers.size()
                  << " negativeAckCount=" << call.negativeAckProviders.size()
                  << " reasons=" << reasons);
        NDN_LOG_INFO("[ServiceUser] all known providers rejected requestId="
                  << requestId.toUri()
                  << " reasons=" << reasons);
        finalizeTimedOutPendingCall(requestId);
        return true;
    }

    bool ServiceUser::collaborationAckRoleCoverageSatisfied(
        const ndn::Name& requestId,
        const PendingCall& pendingCall) const
    {
        if (!pendingCall.isCollaboration ||
            pendingCall.collaborationPlan.roles.empty()) {
            return false;
        }

        if (!pendingCall.collaborationPlan.participantSelector) {
            return false;
        }

        std::vector<ndn_service_framework::AckSelectionCandidate> candidates;
        for (const auto& storedAck : pendingCall.requestAcks) {
            candidates.push_back(makeAckSelectionCandidate(storedAck));
        }
        const auto selected =
            pendingCall.collaborationPlan.participantSelector->select(
                candidates,
                pendingCall.collaborationPlan.roles);
        std::string validationError;
        const bool valid =
            validateCollaborationSelection(pendingCall.collaborationPlan,
                                           selected,
                                           validationError);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_COLLAB_ROLE_COVERAGE_CHECK timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " candidateCount=" << candidates.size()
                  << " selectedCount=" << selected.size()
                  << " roleCount="
                  << pendingCall.collaborationPlan.roles.size()
                  << " valid=" << valid
                  << " reason="
                  << (validationError.empty() ? "-" : validationError));
        return valid;
    }

    bool ServiceUser::usesR1ReservationSelection(const PendingCall& pendingCall) const
    {
        return pendingCall.requestMessage.hasRequestCapabilities() &&
               pendingCall.requestMessage.getRequestCapabilities().hasField(
                   "DIReservationSelectionV1") &&
               pendingCall.requestMessage.getRequestCapabilities().getField(
                   "DIReservationSelectionV1") == "required";
    }

    void ServiceUser::PublishR1SelectionDecision(const StoredAck& ack, bool selected)
    {
        auto pending = m_pendingCalls.find(ack.requestId);
        if (pending == m_pendingCalls.end() || !ack.message.hasReservationLease()) return;
        const auto& lease = ack.message.getReservationLease();
        if (!lease.hasField("reservationId")) return;
        const auto reservationId = lease.getField("reservationId");
        if (pending->second.r1DecisionDeliveries.find(reservationId) !=
            pending->second.r1DecisionDeliveries.end()) {
            return;
        }

        ServiceSelectionMessage message;
        message.setRequestIDs({ack.requestId.toUri()});
        message.setPolicyEpoch(getCurrentPolicyEpoch(ack.serviceName));
        SelectionDecision decision;
        decision.setField("schemaVersion", "1");
        decision.setField("decision", selected ? "SELECTED" : "NOT_SELECTED");
        decision.setField("requester", identity.toUri());
        decision.setField("requestId", ack.requestId.toUri());
        decision.setField("attempt", "1");
        decision.setField("targetProvider", ack.providerName.toUri());
        decision.setField("reservationId", lease.getField("reservationId"));
        decision.setField("reservationDigest", lease.computeDigest());
        decision.setField("decisionSequence", "1");
        decision.setField("issuedAtUs", std::to_string(nowMicroseconds()));
        if (lease.hasField("providerBootEpoch"))
            decision.setField("providerBootEpoch", lease.getField("providerBootEpoch"));
        if (lease.hasField("expiresAtMs"))
            decision.setField("expiresAtMs", lease.getField("expiresAtMs"));
        if (selected) {
            if (!pending->second.deploymentPlan)
                throw std::runtime_error("selected R1 decision missing global plan");
            decision.setField("globalPlanDigest",
                              pending->second.deploymentPlan->computeDigest());
            message.setDeploymentPlan(*pending->second.deploymentPlan);
        }
        message.setSelectionDecision(decision);

        if (selected && !pending->second.selectionGatedInputKey.empty()) {
            if (!ack.message.hasSelectionInputKeyOffer())
                throw std::runtime_error(
                    "SelectionGatedInputV1 positive ACK missing key offer");
            const auto& offer = ack.message.getSelectionInputKeyOffer();
            if (!offer.hasField("recipient") ||
                offer.getField("recipient") != ack.providerName.toUri() ||
                !offer.hasField("recipientCertName") ||
                !offer.hasField("recipientPublicKey") ||
                !offer.hasField("recipientCertDigest"))
                throw std::runtime_error("invalid Selection input key offer binding");
            const auto publicKey = selectionGatedUnhex(
                offer.getField("recipientPublicKey"));
            const auto wrapped = wrapSelectionGatedInputKey(
                pending->second.selectionGatedInputKey, publicKey);
            SelectionInputKeyGrant grant;
            grant.setField("schemaVersion", "1");
            grant.setField("recipient", ack.providerName.toUri());
            grant.setField("recipientCertName", offer.getField("recipientCertName"));
            grant.setField("recipientCertDigest", offer.getField("recipientCertDigest"));
            grant.setField("wrappedInputKey", selectionGatedHex(wrapped));
            grant.setField("encryptedInputDigest",
                           pending->second.requestMessage.getEncryptedRequestInput().computeDigest());
            grant.setField("requestId", ack.requestId.toUri());
            grant.setField("attempt", "1");
            grant.setField("reservationId", lease.getField("reservationId"));
            message.setSelectionInputKeyGrant(grant);
            NDN_LOG_INFO("R1_SELECTION_INPUT_GRANT requestId="
                         << ack.requestId.toUri() << " provider="
                         << ack.providerName.toUri() << " attached=true");
        }

        SelectionProviderEntry entry;
        entry.providerName = ack.providerName;
        const auto token = pending->second.providerTokens.find(ack.providerName.toUri());
        if (m_useTokens && token != pending->second.providerTokens.end()) {
            entry.providerTokenHash = computeSelectionProviderTokenProofHash(
                identity, ack.providerName, ack.serviceName, token->second);
        }
        if (selected) {
            const auto assignment = pending->second.collaborationAssignments.find(
                ack.providerName.toUri());
            ndn::Buffer projection;
            if (assignment != pending->second.collaborationAssignments.end()) {
                projection = assignment->second;
            }
            else {
                const std::string minimal =
                    "role=primary;planDigest=" +
                    pending->second.deploymentPlan->computeDigest() +
                    ";reservationId=" + lease.getField("reservationId") + ";";
                projection = ndn::Buffer(
                    reinterpret_cast<const uint8_t*>(minimal.data()), minimal.size());
            }
            if (!ack.message.hasSelectionInputKeyOffer())
                throw std::runtime_error(
                    "selected DI assignment missing recipient key offer");
            const auto& offer = ack.message.getSelectionInputKeyOffer();
            if (!offer.hasField("recipientPublicKey") ||
                !offer.hasField("recipientCertName"))
                throw std::runtime_error("invalid DI assignment recipient offer");
            const auto aad = recipientAssignmentAssociatedData(
                identity, ack.providerName, ack.serviceName, ack.requestId,
                lease.getField("reservationId"),
                pending->second.deploymentPlan->computeDigest());
            message.setRecipientEncryptedAssignment(
                encryptRecipientAssignment(
                    projection,
                    selectionGatedUnhex(offer.getField("recipientPublicKey")),
                    ack.providerName,
                    ndn::Name(offer.getField("recipientCertName")), aad));
        }
        message.addProviderEntry(entry);
        const auto name = makeServiceSelectionDecisionNameV2(
            identity, ack.providerName, ack.serviceName, ack.requestId, 1);
        const auto suffix = makeServiceSelectionDecisionNameWithoutPrefixV2(
            ack.providerName, ack.serviceName, ack.requestId, 1);
        const auto selectionDigest = computeSelectionDigest(message);
        PublishMessage(name, suffix, message);
        addUniqueName(pending->second.selectionPublishedProviders, ack.providerName);
        uint64_t expiresAtMs = 0;
        if (lease.hasField("expiresAtMs")) {
            try { expiresAtMs = std::stoull(lease.getField("expiresAtMs")); }
            catch (const std::exception&) { expiresAtMs = 0; }
        }
        PendingCall::R1DecisionDelivery delivery;
        delivery.providerName = ack.providerName;
        delivery.serviceName = ack.serviceName;
        delivery.messageName = name;
        delivery.messageSuffix = suffix;
        delivery.message = message;
        delivery.selectionDigest = selectionDigest;
        delivery.decisionDigest = decision.computeDigest();
        delivery.expiresAtMs = expiresAtMs;
        delivery.transmissions = 1;
        pending->second.r1DecisionDeliveries.emplace(reservationId,
                                                     std::move(delivery));
        m_scheduler.schedule(ndn::time::milliseconds(50),
          [this, requestId = ack.requestId, reservationId] {
            pollR1DecisionReceipt(requestId, reservationId);
          });
    }

    void ServiceUser::pollR1DecisionReceipt(const ndn::Name& requestId,
                                            const std::string& reservationId)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end()) return;
        auto delivery = pending->second.r1DecisionDeliveries.find(reservationId);
        if (delivery == pending->second.r1DecisionDeliveries.end() ||
            delivery->second.receiptAccepted) return;
        const auto provider = delivery->second.providerName;
        const auto service = delivery->second.serviceName;
        const auto digest = delivery->second.selectionDigest;
        QuerySelectionStatus(
          provider, service, digest,
          [this, requestId, reservationId](const SelectionExecutionStatus& status) {
            auto pendingNow = m_pendingCalls.find(requestId);
            if (pendingNow == m_pendingCalls.end()) return;
            auto current = pendingNow->second.r1DecisionDeliveries.find(reservationId);
            if (current == pendingNow->second.r1DecisionDeliveries.end()) return;
            bool accepted = false;
            if (!status.decisionReceipt.empty()) {
                auto [ok, block] = ndn::Block::fromBuffer(
                  ndn::span<const uint8_t>(status.decisionReceipt.data(),
                                           status.decisionReceipt.size()));
                if (ok) {
                    SelectionDecisionReceipt receipt;
                    accepted = receipt.WireDecode(block) &&
                      receipt.hasField("reservationId") &&
                      receipt.getField("reservationId") == reservationId &&
                      receipt.hasField("decisionDigest") &&
                      receipt.getField("decisionDigest") == current->second.decisionDigest;
                }
            }
            if (accepted) {
                current->second.receiptAccepted = true;
                return;
            }
            retryR1Decision(requestId, reservationId);
          },
          [this, requestId, reservationId](const ndn::Name&) {
            retryR1Decision(requestId, reservationId);
          }, 100);
    }

    void ServiceUser::retryR1Decision(const ndn::Name& requestId,
                                      const std::string& reservationId)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end()) return;
        auto delivery = pending->second.r1DecisionDeliveries.find(reservationId);
        if (delivery == pending->second.r1DecisionDeliveries.end() ||
            delivery->second.receiptAccepted) return;
        const auto nowMs = static_cast<uint64_t>(nowMilliseconds());
        if (delivery->second.transmissions >= 3 ||
            (delivery->second.expiresAtMs > 0 && nowMs >= delivery->second.expiresAtMs)) {
            return;
        }
        PublishMessage(delivery->second.messageName,
                       delivery->second.messageSuffix,
                       delivery->second.message);
        ++delivery->second.transmissions;
        m_scheduler.schedule(ndn::time::milliseconds(50),
          [this, requestId, reservationId] {
            pollR1DecisionReceipt(requestId, reservationId);
          });
    }

    void ServiceUser::closeR1ReservationDecisions(PendingCall& pendingCall)
    {
        if (!usesR1ReservationSelection(pendingCall)) return;
        DeploymentPlan plan;
        plan.setField("schemaVersion", "1");
        plan.setField("requesterIdentity", identity.toUri());
        if (!pendingCall.requestAcks.empty())
            plan.setField("requestId", pendingCall.requestAcks.front().requestId.toUri());
        plan.setField("attempt", "1");
        if (pendingCall.requestMessage.hasDeploymentIntent())
            plan.setField("intentDigest",
                          pendingCall.requestMessage.getDeploymentIntent().computeDigest());
        size_t member = 0;
        for (const auto& selectedAck : pendingCall.customSelectedAcks) {
            if (!selectedAck.message.hasReservationLease()) continue;
            const auto prefix = "member." + std::to_string(member++) + ".";
            plan.setField(prefix + "provider", selectedAck.providerName.toUri());
            plan.setField(prefix + "reservationId",
                          selectedAck.message.getReservationLease().getField("reservationId"));
            const auto assignment = pendingCall.collaborationAssignments.find(
                selectedAck.providerName.toUri());
            const auto fields = assignment == pendingCall.collaborationAssignments.end() ?
                std::map<std::string, std::string>() :
                parseSemicolonFields(assignment->second);
            const auto role = fields.find("role");
            plan.setField(prefix + "role",
                          role == fields.end() ? "primary" : role->second);
        }
        if (member == 0 && !pendingCall.selectedProvider.empty()) {
            const auto selected = std::find_if(
                pendingCall.requestAcks.begin(), pendingCall.requestAcks.end(),
                [&pendingCall] (const StoredAck& ack) {
                    return ack.providerName.equals(pendingCall.selectedProvider) &&
                           ack.message.hasReservationLease();
                });
            if (selected != pendingCall.requestAcks.end()) {
                plan.setField("member.0.provider", selected->providerName.toUri());
                plan.setField("member.0.reservationId",
                              selected->message.getReservationLease().getField("reservationId"));
                plan.setField("member.0.role", "primary");
                member = 1;
            }
        }
        plan.setField("memberCount", std::to_string(member));
        if (member > 0) pendingCall.deploymentPlan = plan;
        for (const auto& ack : pendingCall.requestAcks) {
            if (!ack.message.getStatus() || !ack.message.hasReservationLease()) continue;
            bool selected = std::any_of(
                pendingCall.customSelectedAcks.begin(), pendingCall.customSelectedAcks.end(),
                [&ack] (const StoredAck& candidate) {
                    return candidate.providerName.equals(ack.providerName) &&
                           candidate.message.getReservationLease().computeDigest() ==
                             ack.message.getReservationLease().computeDigest();
                });
            if (pendingCall.customSelectedAcks.empty() &&
                !pendingCall.selectedProvider.empty())
                selected = pendingCall.selectedProvider.equals(ack.providerName);
            PublishR1SelectionDecision(ack, selected);
        }
    }

    bool ServiceUser::evaluateAckSelection(const ndn::Name& requestId)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
            return false;
        }

        // A failed application selector is terminal for this local attempt.
        // Keep its original request deadline for the timeout callback, but
        // never invoke the selector again on a late ACK.
        if (pendingCall->second.timedOut) {
            return false;
        }

        if (pendingCall->second.providerSelected ||
            !pendingCall->second.selectedProvider.empty()) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_SKIPPED_ALREADY_SELECTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " selectedProvider="
                      << (pendingCall->second.selectedProvider.empty() ?
                          "-" : pendingCall->second.selectedProvider.toUri()));
            return true;
        }
        if (pendingCall->second.collaborationDeferred &&
            !pendingCall->second.collaborationPlanCommitted) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_CLOSED_AWAITING_PLAN"
                      << " timestamp_us=" << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
            return false;
        }

        pendingCall->second.ackSelectionAtUs = nowMicroseconds();
        size_t successfulAckCount = 0;
        for (const auto& storedAck : pendingCall->second.requestAcks) {
            if (storedAck.message.getStatus()) {
                ++successfulAckCount;
            }
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_BEGIN timestamp_us="
                  << pendingCall->second.ackSelectionAtUs
                  << " requestId=" << requestId.toUri()
                  << " ackCount=" << pendingCall->second.requestAcks.size()
                  << " successfulAckCount=" << successfulAckCount
                  << " ackTimeoutMs=" << pendingCall->second.ackTimeoutMs
                  << " timeoutMs=" << pendingCall->second.timeoutMs
                  << " customHandler="
                  << static_cast<bool>(pendingCall->second.isCollaboration ||
                                       pendingCall->second.acksHandler ||
                                       pendingCall->second.ackCandidatesHandler));
        if (m_timelineTrace) {
            logTimelineTrace("user", "ack_selection_start", requestId,
                             {{"ackCount", std::to_string(
                                  pendingCall->second.requestAcks.size())},
                              {"successfulAckCount", std::to_string(
                                  successfulAckCount)},
                              {"attempt", pendingCall->second.streamOptions &&
                                  pendingCall->second.streamOptions->attemptEpoch != 0 ?
                                std::to_string(pendingCall->second.streamOptions->attemptEpoch) :
                                "request"}});
        }

        bool selected = false;
        if (pendingCall->second.isCollaboration ||
            pendingCall->second.acksHandler ||
            pendingCall->second.ackCandidatesHandler) {
            selected = evaluateCustomAckSelection(pendingCall->second);
        }
        else {
            selected = evaluateBuiltInAckSelection(pendingCall->second);
        }

        const bool hasSelectedCandidate =
            !pendingCall->second.selectedProvider.empty() ||
            !pendingCall->second.customSelectedAcks.empty() ||
            (pendingCall->second.strategy == ndn_service_framework::tlv::AllSelected &&
             !pendingCall->second.successfulAckProviders.empty());
        selected = selected && hasSelectedCandidate && !pendingCall->second.timedOut;

        pendingCall->second.ackSelectionCompletedAtUs = nowMicroseconds();
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_END timestamp_us="
                  << pendingCall->second.ackSelectionCompletedAtUs
                  << " requestId=" << requestId.toUri()
                  << " result=" << selected
                  << " selectedProvider="
                  << (pendingCall->second.selectedProvider.empty() ?
                      "-" : pendingCall->second.selectedProvider.toUri())
                  << " customSelectedCount="
                  << pendingCall->second.customSelectedAcks.size()
                  << " successfulProviderCount="
                  << pendingCall->second.successfulAckProviders.size());
        if (m_timelineTrace) {
            logTimelineTrace("user", "ack_selection_done", requestId,
                             {{"selected", selected ? "true" : "false"},
                              {"successfulProviderCount", std::to_string(
                                  pendingCall->second.successfulAckProviders.size())},
                              {"attempt", pendingCall->second.streamOptions &&
                                  pendingCall->second.streamOptions->attemptEpoch != 0 ?
                                std::to_string(pendingCall->second.streamOptions->attemptEpoch) :
                                "request"}});
        }
        if (selected && hasSelectedCandidate) {
            pendingCall->second.providerSelected = true;
            updateRequestLifecycleState(requestId, RequestLifecycleState::PROVIDER_SELECTED);
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=PROVIDER_SELECTED timestamp_us="
                      << pendingCall->second.ackSelectionCompletedAtUs
                      << " requestId=" << requestId.toUri()
                      << " selectedProvider="
                      << (pendingCall->second.selectedProvider.empty() ?
                          "-" : pendingCall->second.selectedProvider.toUri())
                      << " customSelectedCount="
                      << pendingCall->second.customSelectedAcks.size()
                      << " successfulProviderCount="
                      << pendingCall->second.successfulAckProviders.size());
            if (m_timelineTrace) {
                logTimelineTrace("user", "provider_selected", requestId,
                                 {{"selectedProvider",
                                   pendingCall->second.selectedProvider.empty() ?
                                   "-" : pendingCall->second.selectedProvider.toUri()}});
            }
        }
        else {
            // ACK selection without a provider is not terminal: late ACKs may
            // still arrive and trigger selection. Keep the admission slot
            // until the call completes or times out so overload is reflected
            // in the user-side controller instead of admitting more work.
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_NO_PROVIDER_PENDING"
                      << " timestamp_us=" << pendingCall->second.ackSelectionCompletedAtUs
                      << " requestId=" << requestId.toUri()
                      << " ackCount=" << pendingCall->second.requestAcks.size()
                      << " timeoutMs=" << pendingCall->second.timeoutMs);
        }
        if (pendingCall->second.ackWindowExpired)
            closeR1ReservationDecisions(pendingCall->second);
        return selected;
    }

    bool ServiceUser::shouldTrackAckDecrypt(const PendingCall& pendingCall)
    {
        // Deferred collaborations freeze an immutable ACK_CLOSED snapshot.
        // An ACK observed before the deadline must keep that snapshot open
        // until asynchronous decrypt/authentication finishes, just as custom
        // ACK handlers already do. Otherwise a Provider can accept a Request
        // while the User closes an empty candidate set.
        return pendingCall.isCollaboration ||
               pendingCall.strategy == ndn_service_framework::tlv::AllSelected ||
               static_cast<bool>(pendingCall.acksHandler) ||
               static_cast<bool>(pendingCall.ackCandidatesHandler);
    }

    bool ServiceUser::closeDeferredCollaborationAcks(
        const ndn::Name& requestId,
        PendingCall& pendingCall)
    {
        if (!pendingCall.collaborationDeferred) {
            return false;
        }
        if (pendingCall.collaborationAcksClosed) {
            return true;
        }
        std::vector<AckCandidate> candidates;
        for (const auto& storedAck : pendingCall.requestAcks) {
            candidates.push_back(makeAckSelectionCandidate(storedAck));
        }
        pendingCall.collaborationClosedAcks = pendingCall.requestAcks;
        pendingCall.collaborationAcksClosedAtUs = nowMicroseconds();
        pendingCall.collaborationAckClosedDigest = deferredAckClosureDigest(
            requestId, pendingCall.requestDeadlineUs, candidates);
        pendingCall.collaborationAcksClosed = true;

        CollaborationAckClosure closure;
        closure.requestId = requestId;
        closure.candidates = std::move(candidates);
        closure.digest = pendingCall.collaborationAckClosedDigest;
        closure.closedAtUs = pendingCall.collaborationAcksClosedAtUs;
        closure.requestDeadlineUs = pendingCall.requestDeadlineUs;
        logPhaseTiming(
            "user", "ackClosed", requestId,
            {{"attempt", pendingCall.streamOptions ?
                            std::to_string(pendingCall.streamOptions->attemptEpoch) :
                            "request"},
             {"candidateCount", std::to_string(closure.candidates.size())}});
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=COLLAB_ACK_CLOSED"
                  << " timestamp_us=" << closure.closedAtUs
                  << " requestId=" << requestId.toUri()
                  << " candidateCount=" << closure.candidates.size()
                  << " digest=" << closure.digest);
        try {
            // A synchronous commit failure may cancel and erase PendingCall.
            // Keep the executing callback alive independently of its owner.
            const auto handler = pendingCall.collaborationAckClosedHandler;
            handler(closure);
        }
        catch (const std::exception& error) {
            NDN_LOG_ERROR("Deferred collaboration ACK_CLOSED callback failed: "
                          << error.what());
        }
        catch (...) {
            NDN_LOG_ERROR(
                "Deferred collaboration ACK_CLOSED callback failed");
        }
        return true;
    }

    bool ServiceUser::handleAckCollectionTimeout(const ndn::Name& requestId,
                                                bool timerFired)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
            return false;
        }

        // A scheduler callback can be observed slightly before its nominal
        // deadline under load. Keep the ACK window open until the Core-owned
        // deadline is reached; the test-only clock uses this same guard for
        // deterministic 999/1000/1001ms boundary checks.
        const auto nowUs = nowMicroseconds();
        if (timerFired && pendingCall->second.ackWindowDeadlineUs != 0 &&
            nowUs < pendingCall->second.ackWindowDeadlineUs) {
            const auto remainingUs = pendingCall->second.ackWindowDeadlineUs - nowUs;
            const auto remainingMs = std::max<uint64_t>(
                1, (remainingUs + 999) / 1000);
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_EARLY_TIMER_IGNORED"
                      << " timestamp_us=" << nowUs
                      << " requestId=" << requestId.toUri()
                      << " deadlineUs=" << pendingCall->second.ackWindowDeadlineUs);
            m_scheduler.schedule(ndn::time::milliseconds(
                static_cast<int64_t>(std::min<uint64_t>(
                    remainingMs, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))),
              [this, requestId] {
                handleAckCollectionTimeout(requestId);
              });
            return false;
        }

        if (pendingCall->second.strategy == ndn_service_framework::tlv::FirstResponding &&
            !usesR1ReservationSelection(pendingCall->second) &&
            !pendingCall->second.isCollaboration &&
            !pendingCall->second.acksHandler &&
            !pendingCall->second.ackCandidatesHandler) {
            return false;
        }

        pendingCall->second.ackWindowExpired = true;
        if (shouldTrackAckDecrypt(pendingCall->second) &&
            !usesR1ReservationSelection(pendingCall->second) &&
            pendingCall->second.ackDecryptsInFlight > 0) {
            if (pendingCall->second.requestDeadlineUs != 0 &&
                nowUs >= pendingCall->second.requestDeadlineUs) {
                return false;
            }
            ++pendingCall->second.ackSelectionDeferrals;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_DEFERRED_IN_FLIGHT timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " inFlight=" << pendingCall->second.ackDecryptsInFlight
                      << " deferrals=" << pendingCall->second.ackSelectionDeferrals);
            m_scheduler.schedule(ndn::time::milliseconds(20),
              [this, requestId, timerFired]() {
                handleAckCollectionTimeout(requestId, timerFired);
            });
            return false;
        }
        if (pendingCall->second.collaborationDeferred) {
            return closeDeferredCollaborationAcks(
                requestId, pendingCall->second);
        }
        return evaluateAckSelection(requestId);
    }

    bool ServiceUser::selectLateAckAfterAckTimeout(PendingCall& pendingCall,
                                                   const StoredAck& storedAck)
    {
        if (pendingCall.timedOut ||
            pendingCall.providerSelected ||
            !pendingCall.selectedProvider.empty() ||
            !storedAck.message.getStatus()) {
            return false;
        }

        if (usesR1ReservationSelection(pendingCall)) {
            PublishR1SelectionDecision(storedAck, false);
            return true;
        }

        pendingCall.selectedProvider = storedAck.providerName;
        pendingCall.providerSelected = true;
        pendingCall.customSelectedAcks.clear();
        pendingCall.customSelectedAcks.push_back(storedAck);
        pendingCall.successfulAckProviders.clear();
        addUniqueName(pendingCall.successfulAckProviders, storedAck.providerName);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=LATE_ACK_SELECTED_AFTER_ACK_TIMEOUT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << storedAck.requestId.toUri()
                  << " providerName=" << storedAck.providerName.toUri()
                  << " serviceName=" << storedAck.serviceName.toUri()
                  << " providerTokenPresent="
                  << !storedAck.message.getProviderToken().empty());
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=CUSTOM_ACK_SELECTED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << storedAck.requestId.toUri()
                  << " providerName=" << storedAck.providerName.toUri()
                  << " serviceName=" << storedAck.serviceName.toUri()
                  << " providerTokenPresent="
                  << !storedAck.message.getProviderToken().empty());
        PublishServiceSelectionMessageV2(storedAck.providerName,
                                            storedAck.serviceName,
                                            storedAck.requestId);
        return true;
    }

    // This helper must also run before the first envelope encoding: the wire
    // envelope is bounded to 1 MiB, while external opaque assignments allow
    // up to 4 MiB. Both Selection paths share the same publication contract.
    static bool externalizeCollaborationEnvelope(
        ServiceUser& user,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        CollaborationAssignmentEnvelope& envelope,
        std::vector<LargeDataPublishResult>* transaction = nullptr)
    {
        if (envelope.opaquePayload.size() > 4 * 1024 * 1024) {
            throw std::length_error(
                "collaboration assignment exceeds external Data bound");
        }
        if (parseLargeDataReferencePayload(envelope.opaquePayload) ||
            envelope.opaquePayload.size() <= 4096) {
            return false;
        }
        PreparedServiceRequest context;
        context.serviceName = serviceName;
        context.requestId = requestId;
        const std::string digest = sha256DigestString(envelope.opaquePayload);
        std::vector<uint8_t> plaintext(envelope.opaquePayload.begin(),
                                       envelope.opaquePayload.end());
        const auto published = user.publishEncryptedLargeData(
            context, plaintext,
            "selection-assignment-" + providerName.toUri() + "-" +
                envelope.role + "-" + digest.substr(7, 16));
        if (!published.success) {
            throw std::runtime_error(
                "failed to externalize collaboration assignment: " + published.errorMessage);
        }
        if (transaction) transaction->push_back(published);
        LargeDataReference reference;
        reference.dataName = published.encryptedDataName;
        reference.objectType = "application/vnd.ndnsf.collaboration-assignment-v1";
        reference.objectId = published.objectId;
        reference.plaintextSize = envelope.opaquePayload.size();
        reference.encrypted = true;
        reference.digest = digest;
        envelope.opaquePayload = encodeLargeDataReferencePayload(reference);
        NDN_LOG_INFO("NDNSF_COLLAB_ASSIGNMENT_EXTERNALIZED requestId="
                     << requestId.toUri()
                     << " providerName=" << providerName.toUri()
                     << " serviceName=" << serviceName.toUri()
                     << " role=" << envelope.role
                     << " plaintextBytes=" << reference.plaintextSize
                     << " referenceBytes=" << envelope.opaquePayload.size()
                     << " dataName=" << reference.dataName.toUri());
        return true;
    }

    ndn::Buffer ServiceUser::externalizeLargeCollaborationAssignment(
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const ndn::Buffer& assignmentPayload)
    {
        // A Selection is one SVS publication and ultimately one NDN Data
        // packet. Keep room for the token, stream grant, shared role metadata,
        // and hybrid envelope. Large opaque projections use the existing
        // segmented, service-authorized Data path; the SVS piggyback limit is
        // intentionally not a correctness or performance repair mechanism.
        static constexpr size_t INLINE_OPAQUE_LIMIT = 4096;

        std::vector<ndn::Buffer> assignmentItems;
        try {
            assignmentItems = decodeOpaqueAssignmentSet(assignmentPayload);
        }
        catch (const std::exception& error) {
            throw std::runtime_error(
                std::string("cannot inspect collaboration assignment set: ") +
                error.what());
        }

        bool externalized = false;
        for (auto& item : assignmentItems) {
            CollaborationAssignmentEnvelope envelope;
            bool structured = false;
            try {
                structured = decodeCollaborationAssignmentEnvelope(item, envelope);
            }
            catch (const std::exception& error) {
                throw std::runtime_error(
                    std::string("invalid collaboration assignment envelope: ") +
                    error.what());
            }
            if (!structured) {
                if (item.size() > INLINE_OPAQUE_LIMIT) {
                    throw std::length_error(
                        "large collaboration assignment lacks a structured envelope");
                }
                continue;
            }
            if (!externalizeCollaborationEnvelope(
                    *this, providerName, serviceName, requestId, envelope)) {
                continue;
            }
            item = encodeCollaborationAssignmentEnvelope(envelope);
            externalized = true;
        }

        return externalized ? encodeOpaqueAssignmentSet(assignmentItems) :
                              assignmentPayload;
    }

    void ServiceUser::addCustomSelectedAck(PendingCall& pendingCall,
                                           const StoredAck& storedAck)
    {
        const auto alreadySelected = std::any_of(
            pendingCall.customSelectedAcks.begin(), pendingCall.customSelectedAcks.end(),
            [&storedAck](const StoredAck& selected) {
                return selected.providerName.equals(storedAck.providerName) &&
                       selected.serviceName.equals(storedAck.serviceName) &&
                       selected.requestId.equals(storedAck.requestId);
            });
        if (!alreadySelected) {
            pendingCall.customSelectedAcks.push_back(storedAck);
        }
    }

    bool ServiceUser::evaluateCustomAckSelection(PendingCall& pendingCall)
    {
        // Only newly externalized objects belong to this local transaction.
        // On failure remove staging; already queued packets expire normally.
        struct AssignmentPublicationGuard {
            ServiceUser& user;
            std::vector<LargeDataPublishResult> objects;
            bool complete = false;
            ~AssignmentPublicationGuard() {
                if (!complete) user.abortLargeDataPublications(objects);
            }
        } publications{*this, {}, false};
        pendingCall.customSelectedAcks.clear();
        pendingCall.successfulAckProviders.clear();
        pendingCall.selectedProvider = ndn::Name();
        pendingCall.expectedResponseProviders.clear();
        pendingCall.responseProviders.clear();
        pendingCall.collaborationAssignments.clear();

        if (pendingCall.isCollaboration &&
            pendingCall.collaborationPlan.participantSelector) {
            std::vector<ndn_service_framework::AckSelectionCandidate> candidates;
            const auto& candidateAcks = pendingCall.collaborationDeferred ?
                pendingCall.collaborationClosedAcks : pendingCall.requestAcks;
            for (const auto& storedAck : candidateAcks) {
                candidates.push_back(makeAckSelectionCandidate(storedAck));
            }

            std::vector<SelectedParticipant> selectedParticipants;
            if (pendingCall.collaborationDeferred) {
                selectedParticipants = pendingCall.collaborationCommittedParticipants;
            }
            else {
                try {
                    selectedParticipants = pendingCall.collaborationPlan.participantSelector->select(
                        candidates, pendingCall.collaborationPlan.roles);
                }
                catch (const std::exception& error) {
                    NDN_LOG_ERROR("Application participant selector failed: " << error.what());
                    pendingCall.timedOut = true;
                    return false;
                }
                catch (...) {
                    NDN_LOG_ERROR("Application participant selector failed");
                    pendingCall.timedOut = true;
                    return false;
                }
            }
            std::map<std::string, std::vector<ndn::Buffer>>
                assignmentsByProvider;
            std::string validationError;
            if (!validateCollaborationSelection(pendingCall.collaborationPlan,
                                                selectedParticipants,
                                                validationError)) {
                NDN_LOG_ERROR("Reject collaboration selection for request "
                              << pendingCall.requestMessage.getUserToken()
                              << ": " << validationError);
                return false;
            }
            for (const auto& participant : selectedParticipants) {
                if (!authorizeControllerTransition(participant.service,
                                                   ProtectedTransition::SELECTION)) {
                    throw std::runtime_error("collaboration Selection authorization rejected");
                }
                if (participant.assignmentPayload.size() > 4 * 1024 * 1024) {
                    throw std::length_error("collaboration assignment exceeds external Data bound");
                }
            }
            for (const auto& participant : selectedParticipants) {
                for (const auto& storedAck : candidateAcks) {
                    if (!storedAck.providerName.equals(participant.provider) ||
                        !storedAck.serviceName.equals(participant.service) ||
                        !storedAck.requestId.equals(participant.ack.requestId) ||
                        !ackEquals(storedAck.message, participant.ack.ack) ||
                        !storedAck.message.getStatus()) {
                        continue;
                    }

                    addCustomSelectedAck(pendingCall, storedAck);
                    addUniqueName(pendingCall.successfulAckProviders, storedAck.providerName);
                    addUniqueName(pendingCall.expectedResponseProviders, storedAck.providerName);
                    if (pendingCall.selectedProvider.empty()) {
                        pendingCall.selectedProvider = storedAck.providerName;
                    }

        ndn::Buffer assignment = participant.assignmentPayload;
                    if (assignment.empty()) {
                        if (pendingCall.collaborationDeferred) {
                            NDN_LOG_ERROR(
                                "Reject deferred collaboration plan with empty "
                                "opaque participant assignment");
                            return false;
                        }
                        // Preplanned V1 compatibility only. Deferred planning
                        // requires the external participant to provide its
                        // exact opaque assignment bytes.
                        const std::string text =
                            "role=" + participant.role +
                            ";artifact=" + participant.assignedArtifact.toUri() +
                            ";requiresProvisioning=" +
                            (participant.requiresProvisioning ? "1" : "0") +
                            ";provisioningTimeoutMs=" +
                            std::to_string(participant.provisioningTimeoutMs) +
                            ";";
                        assignment = ndn::Buffer(
                            reinterpret_cast<const uint8_t*>(text.data()),
                            text.size());
                    }
                    if (pendingCall.isCollaboration) {
                        CollaborationAssignmentEnvelope envelope;
                        envelope.role = participant.role;
                        envelope.assignedArtifact = participant.assignedArtifact;
                        envelope.artifactDataName = participant.artifactDataName;
                        envelope.requiresProvisioning =
                            participant.requiresProvisioning;
                        envelope.provisioningTimeoutMs =
                            participant.provisioningTimeoutMs;
                        const auto roleUsesScope =
                            [&pendingCall, &participant](const std::string& scope) {
                                for (const auto& keyScope :
                                     pendingCall.collaborationPlan.keyScopes) {
                                    if (keyScope.name != scope) {
                                        continue;
                                    }
                                    return std::any_of(
                                        keyScope.roles.begin(), keyScope.roles.end(),
                                        [&participant](const CollaborationRole& role) {
                                            return role == participant.role;
                                        });
                                }
                                for (const auto& dependency :
                                     pendingCall.collaborationPlan.dependencies) {
                                    if (dependency.keyScope != scope) {
                                        continue;
                                    }
                                    return std::any_of(
                                               dependency.producers.begin(),
                                               dependency.producers.end(),
                                               [&participant](const CollaborationRole& role) {
                                                   return role == participant.role;
                                               }) ||
                                           std::any_of(
                                               dependency.consumers.begin(),
                                               dependency.consumers.end(),
                                               [&participant](const CollaborationRole& role) {
                                                   return role == participant.role;
                                               });
                                }
                                return false;
                            };
                        // Carry encrypted scope-key Data references in the
                        // per-Provider envelope as well as the shared
                        // Selection metadata.  Preparation runs before the
                        // application handler and therefore must receive the
                        // references on the exact assignment it prepares.
                        for (const auto& field :
                             parseSemicolonFields(
                                 pendingCall.collaborationPlan.sharedAssignmentMetadata)) {
                            static const std::string prefix = "scopeKeyData.";
                            if (field.first.rfind(prefix, 0) != 0 ||
                                field.first.substr(prefix.size()).empty() ||
                                field.second.empty()) {
                                continue;
                            }
                            const auto scope = field.first.substr(prefix.size());
                            if (roleUsesScope(scope)) {
                                envelope.scopeKeyDataNames.emplace(
                                    scope, ndn::Name(field.second));
                            }
                        }
                        for (const auto& [scope, key] :
                             pendingCall.collaborationScopeKeys) {
                            if (roleUsesScope(scope)) {
                                envelope.scopeKeys.emplace(scope, key);
                            }
                        }
                        // The envelope is framework metadata; the application
                        // assignment remains opaque and is carried unchanged.
                        envelope.opaquePayload = std::move(assignment);
                        externalizeCollaborationEnvelope(
                            *this, storedAck.providerName, storedAck.serviceName,
                            storedAck.requestId, envelope, &publications.objects);
                        assignment =
                            encodeCollaborationAssignmentEnvelope(envelope);
                    }
                    else {
                        assignment = mergeSelectionAssignmentPayloads(
                            assignment,
                            genericAdmissionLeaseSelectionPayloadFromAck(
                                storedAck.message));
                    }
                    auto& providerAssignments = assignmentsByProvider[
                        storedAck.providerName.toUri()];
                    const bool duplicateOpaqueTuple = std::any_of(
                        providerAssignments.begin(),
                        providerAssignments.end(),
                        [&assignment] (const ndn::Buffer& existing) {
                            return existing.size() == assignment.size() &&
                                std::equal(existing.begin(), existing.end(),
                                           assignment.begin());
                        });
                    if (!duplicateOpaqueTuple) {
                        providerAssignments.push_back(std::move(assignment));
                    }
                    // This is qualification evidence, not verbose diagnostics:
                    // the ACK-driven role map must remain visible under the
                    // routine *=WARN filter without enabling all ServiceUser
                    // INFO records.
                    NDN_LOG_WARN("NDNSF_COLLAB_ASSIGNMENT_SELECTED requestId="
                                 << storedAck.requestId.toUri()
                                 << " providerName=" << storedAck.providerName.toUri()
                                 << " serviceName=" << storedAck.serviceName.toUri()
                                 << " role=" << participant.role
                                 << " assignmentPayloadBytes="
                                 << providerAssignments.back().size()
                                 << " ackPayloadBytes="
                                 << storedAck.message.getPayload().size());
                    break;
                }
            }
            for (auto& [provider, assignments] : assignmentsByProvider) {
                pendingCall.collaborationAssignments[provider] =
                    encodeOpaqueAssignmentSet(assignments);
            }
        }
        else if (pendingCall.ackCandidatesHandler) {

            std::vector<ndn_service_framework::AckSelectionCandidate> candidates;
            for (const auto& storedAck : pendingCall.requestAcks) {
                candidates.push_back(makeAckSelectionCandidate(storedAck));
            }

            std::vector<AckSelectionCandidate> selectedCandidates;
            try {
                selectedCandidates = pendingCall.ackCandidatesHandler(candidates);
            }
            catch (const std::exception& error) {
                NDN_LOG_ERROR("Application ACK candidate selector failed: " << error.what());
                pendingCall.timedOut = true;
                return false;
            }
            catch (...) {
                NDN_LOG_ERROR("Application ACK candidate selector failed");
                pendingCall.timedOut = true;
                return false;
            }
            for (const auto& selectedCandidate : selectedCandidates) {
                for (const auto& storedAck : pendingCall.requestAcks) {
                    if (!storedAck.providerName.equals(selectedCandidate.providerName) ||
                        !storedAck.serviceName.equals(selectedCandidate.serviceName) ||
                        !storedAck.requestId.equals(selectedCandidate.requestId)) {
                        continue;
                    }
                    if (!ackEquals(storedAck.message, selectedCandidate.ack)) {
                        continue;
                    }

                    if (!storedAck.message.getStatus()) {
                        break;
                    }

                    addCustomSelectedAck(pendingCall, storedAck);
                    addUniqueName(pendingCall.successfulAckProviders, storedAck.providerName);
                    addUniqueName(pendingCall.expectedResponseProviders, storedAck.providerName);
                    if (pendingCall.selectedProvider.empty()) {
                        pendingCall.selectedProvider = storedAck.providerName;
                    }
                    break;
                }
            }
        }
        else {
            std::vector<ndn_service_framework::RequestAckMessage> ackMessages;
            for (const auto& storedAck : pendingCall.requestAcks) {
                ackMessages.push_back(storedAck.message);
            }

            std::vector<RequestAckMessage> selectedMessages;
            try {
                selectedMessages = pendingCall.acksHandler(ackMessages);
            }
            catch (const std::exception& error) {
                NDN_LOG_ERROR("Application ACK selector failed: " << error.what());
                pendingCall.timedOut = true;
                return false;
            }
            catch (...) {
                NDN_LOG_ERROR("Application ACK selector failed");
                pendingCall.timedOut = true;
                return false;
            }
            for (const auto& selectedMessage : selectedMessages) {
                const auto* storedAck = findStoredAck(pendingCall, selectedMessage);
                if (storedAck == nullptr || !storedAck->message.getStatus()) {
                    continue;
                }

                addCustomSelectedAck(pendingCall, *storedAck);
                addUniqueName(pendingCall.successfulAckProviders, storedAck->providerName);
                addUniqueName(pendingCall.expectedResponseProviders, storedAck->providerName);
                if (pendingCall.selectedProvider.empty()) {
                    pendingCall.selectedProvider = storedAck->providerName;
                }
            }
        }

        // Once Selection publication can start, references may escape. Keep
        // their bounded serving lifetime even if a later publication fails;
        // the commit caller terminates the invocation rather than retrying it.
        publications.complete = true;
        if (usesR1ReservationSelection(pendingCall)) {
            // The timeout-closure path publishes one exact-target decision
            // for every reservation-bearing positive ACK.
        }
        else if (pendingCall.customSelectedAcks.size() > 1 &&
            (pendingCall.isCollaboration ||
             !pendingCall.selectionAssignmentPayloads.empty())) {
            // A collaboration assignment is an independently authorized
            // provider projection.  Combining every opaque assignment into
            // one hybrid/SVS publication can exceed NDN's single-packet wire
            // budget after encryption, key wrapping, and signatures.  Keep
            // one durable request and one Selection phase, but publish one
            // bounded provider-specific projection per selected participant.
            std::set<std::string> publishedProviderServices;
            for (const auto& selectedAck : pendingCall.customSelectedAcks) {
                const auto providerServiceKey =
                    selectedAck.providerName.toUri() + "|" +
                    selectedAck.serviceName.toUri();
                // A single Provider may own several collaboration roles.  Its
                // assignments are already grouped into one opaque set, so
                // publish one provider-scoped Selection and one digest/status
                // stream instead of letting later role entries replace the
                // terminal binding.
                if (!publishedProviderServices.insert(providerServiceKey).second) {
                    continue;
                }
                NDN_LOG_INFO("NDNSF_SELECTION_PROVIDER_PROJECTION requestId="
                             << selectedAck.requestId.toUri()
                             << " providerName=" << selectedAck.providerName.toUri()
                             << " serviceName=" << selectedAck.serviceName.toUri()
                             << " selectedCount="
                             << pendingCall.customSelectedAcks.size());
                PublishServiceSelectionMessageV2(selectedAck.providerName,
                                                 selectedAck.serviceName,
                                                 selectedAck.requestId);
            }
        }
        else if (pendingCall.customSelectedAcks.size() > 1) {
            PublishCompactServiceSelectionMessageV2(pendingCall.customSelectedAcks);
        }
        else {
            for (const auto& selectedAck : pendingCall.customSelectedAcks) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=CUSTOM_ACK_SELECTED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << selectedAck.requestId.toUri()
                          << " providerName=" << selectedAck.providerName.toUri()
                          << " serviceName=" << selectedAck.serviceName.toUri()
                          << " providerTokenPresent="
                          << !selectedAck.message.getProviderToken().empty());
                PublishServiceSelectionMessageV2(selectedAck.providerName,
                                                    selectedAck.serviceName,
                                                    selectedAck.requestId);
            }
        }

        return !pendingCall.timedOut && !pendingCall.customSelectedAcks.empty();
    }

    bool ServiceUser::evaluateBuiltInAckSelection(PendingCall& pendingCall)
    {
        pendingCall.successfulAckProviders.clear();
        for (const auto& storedAck : pendingCall.requestAcks) {
            if (storedAck.message.getStatus()) {
                addUniqueName(pendingCall.successfulAckProviders, storedAck.providerName);
            }
        }

        if (pendingCall.strategy == ndn_service_framework::tlv::FirstResponding) {
            if (pendingCall.selectedProvider.empty() && !pendingCall.successfulAckProviders.empty()) {
                pendingCall.selectedProvider = pendingCall.successfulAckProviders.front();
            }
            pendingCall.expectedResponseProviders.clear();
            addUniqueName(pendingCall.expectedResponseProviders, pendingCall.selectedProvider);
            return !pendingCall.selectedProvider.empty();
        }

        if (pendingCall.strategy == ndn_service_framework::tlv::RandomSelection) {
            pendingCall.selectedProvider = selectRandomProvider(pendingCall.successfulAckProviders);
            pendingCall.expectedResponseProviders.clear();
            addUniqueName(pendingCall.expectedResponseProviders, pendingCall.selectedProvider);
            return !pendingCall.selectedProvider.empty();
        }

        if (pendingCall.strategy == ndn_service_framework::tlv::AllSelected) {
            pendingCall.selectedProvider = ndn::Name();
            pendingCall.customSelectedAcks.clear();
            pendingCall.expectedResponseProviders.clear();
            pendingCall.responseProviders.clear();
            for (const auto& storedAck : pendingCall.requestAcks) {
                if (!storedAck.message.getStatus()) {
                    continue;
                }
                pendingCall.customSelectedAcks.push_back(storedAck);
                addUniqueName(pendingCall.expectedResponseProviders, storedAck.providerName);
                if (pendingCall.selectedProvider.empty()) {
                    pendingCall.selectedProvider = storedAck.providerName;
                }
            }
            if (usesR1ReservationSelection(pendingCall)) {
                // Published atomically by closeR1ReservationDecisions().
            }
            else if (pendingCall.customSelectedAcks.size() > 1) {
                PublishCompactServiceSelectionMessageV2(pendingCall.customSelectedAcks);
            }
            else {
                for (const auto& selectedAck : pendingCall.customSelectedAcks) {
                    PublishServiceSelectionMessageV2(selectedAck.providerName,
                                                        selectedAck.serviceName,
                                                        selectedAck.requestId);
                }
            }
            return !pendingCall.timedOut && !pendingCall.expectedResponseProviders.empty();
        }

        return false;
    }

    bool ServiceUser::containsName(const std::vector<ndn::Name>& names,
                                   const ndn::Name& name)
    {
        for (const auto& item : names) {
            if (item.equals(name)) {
                return true;
            }
        }
        return false;
    }

    void ServiceUser::addUniqueName(std::vector<ndn::Name>& names,
                                    const ndn::Name& name)
    {
        if (!name.empty() && !containsName(names, name)) {
            names.push_back(name);
        }
    }

    void ServiceUser::removeName(std::vector<ndn::Name>& names,
                                 const ndn::Name& name)
    {
        names.erase(std::remove_if(names.begin(),
                                   names.end(),
                                   [&name](const ndn::Name& item) {
                                       return item.equals(name);
                                   }),
                    names.end());
    }

    ndn::Name ServiceUser::selectRandomProvider(
        const std::vector<ndn::Name>& providers)
    {
        if (providers.empty()) {
            return ndn::Name();
        }

        ndn::Name selected = providers.front();
        for (const auto& provider : providers) {
            if (provider.toUri() < selected.toUri()) {
                selected = provider;
            }
        }
        return selected;
    }

    const ServiceUser::StoredAck* ServiceUser::findStoredAck(
        const PendingCall& pendingCall,
        const ndn_service_framework::RequestAckMessage& ackMessage)
    {
        for (const auto& storedAck : pendingCall.requestAcks) {
            if (ackEquals(storedAck.message, ackMessage)) {
                return &storedAck;
            }
        }
        return nullptr;
    }

    void ServiceUser::processNDNSDServiceInfoCallback(const ndnsd::discovery::Details &details)
    {
        NDN_LOG_INFO("NDNSD service details received for " << details.serviceName);
    }

    void ServiceUser::onPermissionResponseData(const ndn::Interest& interest,
                                               const ndn::Data& data)
    {
        const auto expectedController = extractPermissionControllerIdentity(interest, currentControllerIdentityForControl());
        if (!expectedController || data.getName() != interest.getName()) return;
        validator->validateWithConfiguredTrustSchema(
            data,
            [this, expectedController](const ndn::Data& validatedData) {
                if (expectedController &&
                    !isSignedByIdentity(validatedData, *expectedController)) {
                    NDN_LOG_ERROR("PermissionResponse Data signer mismatch: "
                                  << validatedData.getName()
                                  << " expectedController=" << expectedController->toUri());
                    return;
                }
                EncryptedPermissionResponse encryptedResponse;
                if (decodeEncryptedPermissionResponseFromDataContent(validatedData, encryptedResponse)) {
                    try {
                        auto& responseKeyChain = m_testSigningKeyChain ?
                            *m_testSigningKeyChain : m_keyChain;
                        auto response =
                            decryptPermissionResponseWithKeyChain(encryptedResponse, responseKeyChain);
                        if (ndn::Name(response.getTargetIdentity()) != identity) {
                            NDN_LOG_ERROR("Ignoring PermissionResponse for unexpected targetIdentity="
                                          << response.getTargetIdentity()
                                          << " expected=" << identity.toUri());
                            return;
                        }
                        applyPermissionResponse(response);
                    }
                    catch (const std::exception& e) {
                        NDN_LOG_ERROR("Failed to install PermissionResponse epoch: "
                                      << e.what());
                    }
                }
            },
            [](const ndn::Data& badData, const ndn::security::ValidationError& error) {
                NDN_LOG_ERROR("PermissionResponse Data validation failed: "
                              << badData.getName() << " reason=" << error);
            });
    }

    void ServiceUser::onPermissionResponseTimeout(const ndn::Interest& interest,
                                                  int attempt)
    {
        const int maxAttempts = permissionFetchMaxAttempts();
        if (attempt >= maxAttempts) {
            NDN_LOG_ERROR("PermissionResponse timeout: " << interest.getName()
                          << " attempt=" << attempt
                          << "/" << maxAttempts
                          << " final=1");
            return;
        }

        const int nextAttempt = attempt + 1;
        const int backoffMs = permissionFetchRetryBackoffMs(attempt);
        NDN_LOG_WARN("PermissionResponse timeout: " << interest.getName()
                     << " attempt=" << attempt
                     << "/" << maxAttempts
                     << " retryAttempt=" << nextAttempt
                     << " backoffMs=" << backoffMs);
        m_scheduler.schedule(ndn::time::milliseconds(backoffMs),
            [this, interest, nextAttempt] {
                ndn::Interest retryInterest(interest);
                retryInterest.refreshNonce();
                retryInterest.setInterestLifetime(
                    ndn::time::milliseconds(permissionFetchLifetimeMs()));
                m_face.expressInterest(
                    retryInterest,
                    std::bind(&ServiceUser::onPermissionResponseData, this, _1, _2),
                    [this, nextAttempt](const ndn::Interest& interest, const ndn::lp::Nack&) {
                        onPermissionResponseTimeout(interest, nextAttempt);
                    },
                    [this, nextAttempt](const ndn::Interest& interest) {
                        onPermissionResponseTimeout(interest, nextAttempt);
                    });
            });
    }

    ndn::Name ServiceUser::currentControllerIdentityForControl() const
    {
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        return m_controllerPrefix;
    }

    std::optional<ndn::Interest> ServiceUser::prepareControllerControlQuery(
        const ndn::Name& producer, const std::string& operation,
        const ndn::Block& parameters, int attempt)
    {
        try {
            const auto owner = makeControlRpcName(producer, operation, parameters, "private-owner");
            const auto existing = m_preparedControlInterests.find(owner);
            if (attempt > 1) {
                if (existing != m_preparedControlInterests.end() &&
                    (operation != "POLICY-STATUS" ||
                     controlNonceExpiry(producer, existing->second))) {
                    auto retry = existing->second;
                    retry.refreshNonce();
                    return retry;
                }
                // Renew only the expired status query identity, not its attempt budget.
                if (operation != "POLICY-STATUS") return std::nullopt;
            }
            if (existing == m_preparedControlInterests.end() && m_preparedControlInterests.size() >= 8192)
                return std::nullopt;
            ndn::Interest interest(makeControlRpcName(producer, operation, parameters, makeControlQueryNonce()));
            interest.setApplicationParameters(parameters);
            interest.setCanBePrefix(false);
            interest.setMustBeFresh(true);
            interest.setInterestLifetime(ndn::time::milliseconds(permissionFetchLifetimeMs()));
            checkNamingNameSize(interest.getName());
            if (interest.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE) return std::nullopt;
            m_preparedControlInterests.insert_or_assign(owner, interest);
            m_scheduler.schedule(ndn::time::milliseconds(controlQueryLifetimeMs(operation)),
                [this, owner, finalName = interest.getName()] {
                    const auto current = m_preparedControlInterests.find(owner);
                    if (current != m_preparedControlInterests.end() &&
                        current->second.getName() == finalName) {
                        m_preparedControlInterests.erase(current);
                    }
                });
            return interest;
        }
        catch (const std::exception& e) {
            NDN_LOG_WARN("Control query preparation failed: " << e.what()); return std::nullopt;
        }
    }

    void ServiceUser::fetchPolicyManifestFromController(const ndn::Name& controllerPrefix,
                                                        int attempt)
    {
        const auto prepared = prepareControllerControlQuery(controllerPrefix, "POLICY-MANIFEST",
            encodeControlQueryParameters({}), attempt);
        if (!prepared) return;
        const auto interest = *prepared;
        const auto interestName = interest.getName();

        NDN_LOG_INFO("Fetch policy manifest: " << interestName
                     << " attempt=" << attempt
                     << "/" << permissionFetchMaxAttempts());
        m_face.expressInterest(
            interest,
            std::bind(&ServiceUser::onPolicyManifestData, this, _1, _2),
            [this, attempt](const ndn::Interest& interest, const ndn::lp::Nack&) {
                onPolicyManifestTimeout(interest, attempt);
            },
            [this, attempt](const ndn::Interest& interest) {
                onPolicyManifestTimeout(interest, attempt);
            });
    }

    void ServiceUser::onPolicyManifestData(const ndn::Interest& interest,
                                           const ndn::Data& data)
    {
        const auto expectedController = extractPermissionControllerIdentity(interest, currentControllerIdentityForControl());
        if (!expectedController || data.getName() != interest.getName()) return;
        validator->validateWithConfiguredTrustSchema(
            data,
            [this, expectedController](const ndn::Data& validatedData) {
                if (expectedController &&
                    !isSignedByIdentity(validatedData, *expectedController)) {
                    NDN_LOG_ERROR("PolicyManifest Data signer mismatch: "
                                  << validatedData.getName()
                                  << " expectedController=" << expectedController->toUri());
                    return;
                }
                PolicyManifest manifest;
                const auto& content = validatedData.getContent();
                bool ok = content.type() == tlv::PolicyManifestType ?
                    manifest.WireDecode(content) : false;
                if (!ok) {
                    auto [parsed, block] = ndn::Block::fromBuffer(
                        ndn::span<const uint8_t>(content.value(), content.value_size()));
                    ok = parsed && manifest.WireDecode(block);
                }
                if (!ok) {
                    NDN_LOG_ERROR("PolicyManifest decode failed: " << validatedData.getName());
                    return;
                }
                m_currentPolicyEpoch = manifest.getPolicyEpoch();
                m_requiredKeyEpoch = manifest.getRequiredKeyEpoch();
                m_policyGracePeriodMs = manifest.getGracePeriodMs();
                if (manifest.hasControllerVersion()) {
                    adoptControllerVersion(manifest.getControllerVersion());
                }
                NDN_LOG_INFO("Installed PolicyManifest " << manifest.toString());
            },
            [](const ndn::Data& badData, const ndn::security::ValidationError& error) {
                NDN_LOG_ERROR("PolicyManifest Data validation failed: "
                              << badData.getName() << " reason=" << error);
            });
    }

    void ServiceUser::onPolicyManifestTimeout(const ndn::Interest& interest,
                                              int attempt)
    {
        const int maxAttempts = permissionFetchMaxAttempts();
        if (attempt >= maxAttempts) {
            NDN_LOG_ERROR("PolicyManifest timeout: " << interest.getName()
                          << " attempt=" << attempt
                          << "/" << maxAttempts
                          << " final=1");
            return;
        }

        const int nextAttempt = attempt + 1;
        const int backoffMs = permissionFetchRetryBackoffMs(attempt);
        NDN_LOG_WARN("PolicyManifest timeout: " << interest.getName()
                     << " attempt=" << attempt
                     << "/" << maxAttempts
                     << " retryAttempt=" << nextAttempt
                     << " backoffMs=" << backoffMs);
        m_scheduler.schedule(ndn::time::milliseconds(backoffMs),
            [this, interest, nextAttempt] {
                ndn::Interest retryInterest(interest);
                retryInterest.refreshNonce();
                retryInterest.setInterestLifetime(
                    ndn::time::milliseconds(permissionFetchLifetimeMs()));
                m_face.expressInterest(
                    retryInterest,
                    std::bind(&ServiceUser::onPolicyManifestData, this, _1, _2),
                    [this, nextAttempt](const ndn::Interest& interest, const ndn::lp::Nack&) {
                        onPolicyManifestTimeout(interest, nextAttempt);
                    },
                    [this, nextAttempt](const ndn::Interest& interest) {
                        onPolicyManifestTimeout(interest, nextAttempt);
                    });
            });
    }

  void
  ServiceUser::fetchPolicyStatusFromController(const ndn::Name& controllerPrefix,
                                       const ndn::Name& serviceName, int attempt,
                                       std::optional<ControllerVersion> expectedVersion)
  {
    dispatchPolicyStatusFetch(controllerPrefix, serviceName, attempt, expectedVersion, false);
  }

  bool
  ServiceUser::isCurrentPolicyStatusFetch(const ndn::Name& serviceName, uint64_t ticket,
                                  const ndn::Name& queryName) const
  {
    std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
    const auto found = m_policyStatusFetchInFlight.find(serviceName.toUri());
    return ticket != 0 && found != m_policyStatusFetchInFlight.end() &&
      found->second->ticket == ticket && found->second->query &&
      found->second->query->getName() == queryName;
  }

  void
  ServiceUser::finishPolicyStatusFetch(const ndn::Name& serviceName, uint64_t ticket)
  {
    std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
    const auto key = serviceName.toUri();
    const auto found = m_policyStatusFetchInFlight.find(key);
    if (found == m_policyStatusFetchInFlight.end() || found->second->ticket != ticket) return;
    found->second->pending.cancel();
    found->second->retry.cancel();
    m_policyStatusFetchInFlight.erase(found);
    const auto coordinator = m_policyRefreshCoordinators.find(key);
    if (coordinator != m_policyRefreshCoordinators.end()) {
      coordinator->second.finishTransportFetch();
    }
  }

  std::optional<ndn::Name>
  ServiceUser::policyStatusController(const ndn::Interest& interest, uint64_t ticket) const
  {
    const auto parameters = decodeControlQueryParameters(interest.getApplicationParameters());
    if (!parameters || ticket == 0) return std::nullopt;
    std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
    const auto found = m_policyStatusFetchInFlight.find(parameters->service.toUri());
    if (found == m_policyStatusFetchInFlight.end() || found->second->ticket != ticket ||
        !found->second->query || found->second->query->getName() != interest.getName()) {
      return std::nullopt;
    }
    const auto& controller = found->second->controller;
    if (!m_controllerPrefix.empty() && m_controllerPrefix != controller) return std::nullopt;
    return controller;
  }

  void
  ServiceUser::dispatchPolicyStatusFetch(const ndn::Name& controllerPrefix,
                                  const ndn::Name& serviceName, int attempt,
                                  std::optional<ControllerVersion> expectedVersion, bool replace)
  {
    if (controllerPrefix.empty() || serviceName.empty() || attempt < 1) return;
    const auto key = serviceName.toUri();
    std::shared_ptr<detail::PolicyStatusFetch> owner;
    {
      std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
      auto found = m_policyStatusFetchInFlight.find(key);
      if (found != m_policyStatusFetchInFlight.end()) {
        if (attempt == 1 && !replace) return;
        owner = found->second;
      }
      else {
        if (attempt != 1 || m_policyStatusFetchInFlight.size() >= 8192) return;
        owner = std::make_shared<detail::PolicyStatusFetch>();
        m_policyStatusFetchInFlight.emplace(key, owner);
      }
      if (owner->attempts >= permissionFetchMaxAttempts() ||
          m_nextPolicyStatusFetchTicket == std::numeric_limits<uint64_t>::max()) {
        owner->pending.cancel();
        owner->retry.cancel();
        m_policyStatusFetchInFlight.erase(key);
        const auto coordinator = m_policyRefreshCoordinators.find(key);
        if (coordinator != m_policyRefreshCoordinators.end()) {
          coordinator->second.finishTransportFetch();
        }
        return;
      }
      ControlQueryParameters parameters;
      parameters.service = serviceName;
      if (expectedVersion) {
        parameters.generation = expectedVersion->controllerGenerationTimestamp;
        parameters.epoch = expectedVersion->controllerEpoch;
      }
      const auto now = ndn::time::steady_clock::now();
      const auto previous = owner->query ?
        parsePolicyStatusName(controllerPrefix, *owner->query) : std::optional<PolicyStatusName>();
      const bool renew = replace || !previous || previous->version != expectedVersion ||
                         now >= owner->renewAt;
      const auto prepared = prepareControllerControlQuery(controllerPrefix, "POLICY-STATUS",
        encodeControlQueryParameters(parameters), renew ? 1 : owner->attempts + 1);
      if (!prepared) {
        owner->pending.cancel();
        owner->retry.cancel();
        m_policyStatusFetchInFlight.erase(key);
        const auto coordinator = m_policyRefreshCoordinators.find(key);
        if (coordinator != m_policyRefreshCoordinators.end()) {
          coordinator->second.finishTransportFetch();
        }
        return;
      }
      if (!owner->query || owner->query->getName() != prepared->getName()) {
        owner->renewAt = now + ndn::time::milliseconds(controlQueryLifetimeMs("POLICY-STATUS"));
      }
      owner->pending.cancel();
      owner->retry.cancel();
      owner->controller = controllerPrefix;
      owner->query = *prepared;
      ++owner->attempts;
      owner->ticket = ++m_nextPolicyStatusFetchTicket;
    }
    const auto ticket = owner->ticket;
    const auto transportAttempt = owner->attempts;
    const std::weak_ptr<detail::PolicyStatusFetch> weakOwner = owner;
    NDN_LOG_INFO("Fetch policy status: " << owner->query->getName()
                 << " attempt=" << transportAttempt << "/" << permissionFetchMaxAttempts());
    try {
      owner->pending = m_face.expressInterest(*owner->query,
      [this, weakOwner, ticket, transportAttempt](const ndn::Interest& interest,
                                                const ndn::Data& data) {
        if (weakOwner.expired()) return;
        onPolicyStatusData(interest, data, transportAttempt, ticket);
      },
      [this, weakOwner, ticket, transportAttempt](const ndn::Interest& interest,
                                                const ndn::lp::Nack&) {
        if (weakOwner.expired()) return;
        onPolicyStatusTimeout(interest, transportAttempt, ticket);
      },
      [this, weakOwner, ticket, transportAttempt](const ndn::Interest& interest) {
        if (weakOwner.expired()) return;
        onPolicyStatusTimeout(interest, transportAttempt, ticket);
      });
    }
    catch (const std::exception& error) {
      NDN_LOG_ERROR("PolicyStatus dispatch failed: " << error.what());
      finishPolicyStatusFetch(serviceName, ticket);
    }
  }

    void ServiceUser::onPolicyStatusData(const ndn::Interest& interest,
                                         const ndn::Data& data,
                                         int attempt, uint64_t ticket)
    {
        const auto expectedController = policyStatusController(interest, ticket);
        if (!expectedController) {
            // Data delivery consumes the pending Interest, so no later timeout
            // can release an owner whose configured Controller has changed.
            const auto parameters = decodeControlQueryParameters(interest.getApplicationParameters());
            if (parameters && isCurrentPolicyStatusFetch(parameters->service, ticket,
                                                        interest.getName())) {
                finishPolicyStatusFetch(parameters->service, ticket);
            }
            return;
        }
        const auto parsedInterest = expectedController ?
            parsePolicyStatusName(*expectedController, interest) :
            std::optional<PolicyStatusName>();
        const ndn::Name serviceName = parsedInterest ?
            parsedInterest->serviceName : ndn::Name();
        if (!expectedController || !parsedInterest || data.getName() != interest.getName()) return;
        const auto requestedVersion = parsedInterest ? parsedInterest->version : std::nullopt;
        const auto serviceKey = serviceName.toUri();
        if (!isCurrentPolicyStatusFetch(serviceName, ticket, interest.getName())) return;
        std::weak_ptr<detail::PolicyStatusFetch> weakOwner;
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            const auto found = m_policyStatusFetchInFlight.find(serviceKey);
            if (found == m_policyStatusFetchInFlight.end() || found->second->ticket != ticket) return;
            weakOwner = found->second;
        }
        auto clearInFlight = [this, weakOwner, serviceName, ticket] {
            if (weakOwner.expired()) return;
            finishPolicyStatusFetch(serviceName, ticket);
        };
        auto retryValidationFailure = [this, weakOwner, interest, attempt, ticket] {
            if (weakOwner.expired()) return;
            onPolicyStatusTimeout(interest, attempt, ticket);
        };

        validator->validateWithConfiguredTrustSchema(
            data,
            [this, interest, expectedController, serviceName, requestedVersion,
             clearInFlight, retryValidationFailure, weakOwner, ticket](const ndn::Data& validatedData) {
                if (weakOwner.expired() ||
                    !isCurrentPolicyStatusFetch(serviceName, ticket, interest.getName())) return;
                if (policyStatusController(interest, ticket) != expectedController) {
                    clearInFlight();
                    return;
                }
                if (expectedController &&
                    !isSignedByIdentity(validatedData, *expectedController)) {
                    NDN_LOG_ERROR("PolicyStatus Data signer mismatch: "
                                  << validatedData.getName()
                                  << " expectedController=" << expectedController->toUri());
                    retryValidationFailure();
                    return;
                }
                PolicyStatusData status;
                const auto& content = validatedData.getContent();
                bool ok = content.type() == PolicyStatusData::TYPE &&
                    status.wireDecode(content);
                if (!ok && content.value_size() > 0) {
                    auto [parsed, block] = ndn::Block::fromBuffer(
                        ndn::span<const uint8_t>(content.value(), content.value_size()));
                    ok = parsed && status.wireDecode(block);
                }
                bool controllerCertificateMatches = !expectedController;
                if (expectedController && ok) {
                    try {
                        controllerCertificateMatches =
                            ndn::security::extractIdentityFromCertName(
                                status.getControllerCertificate()) == *expectedController;
                    }
                    catch (const std::exception&) {
                        controllerCertificateMatches = false;
                    }
                }
                const bool exactNameMatches = expectedController &&
                    validatedData.getName() == interest.getName() &&
                    (!requestedVersion || status.getControllerVersion() == *requestedVersion);
                bool meetsLatestTarget = true;
                {
                    std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
                    const auto coordinator = m_policyRefreshCoordinators.find(serviceName.toUri());
                    if (coordinator != m_policyRefreshCoordinators.end() &&
                        coordinator->second.highestCandidate()) {
                        meetsLatestTarget = status.getControllerVersion().compare(
                            *coordinator->second.highestCandidate()) >= 0;
                    }
                }
                if (!meetsLatestTarget || !ok || serviceName.empty() ||
                    status.getServiceName() != serviceName ||
                    !exactNameMatches ||
                    !status.validate(nowMilliseconds()) ||
                    !controllerCertificateMatches ||
                    !installControllerStatus(status, true)) {
                    NDN_LOG_ERROR("PolicyStatus rejected: " << validatedData.getName());
                    retryValidationFailure();
                    return;
                }
                NDN_LOG_INFO("Installed PolicyStatus service=" << serviceName
                             << " generation="
                             << status.getControllerVersion().controllerGenerationTimestamp
                             << " epoch=" << status.getControllerVersion().controllerEpoch);
                persistAcceptedControllerStatus(validatedData, status);
                clearInFlight();
            },
            [retryValidationFailure](const ndn::Data& badData,
                                     const ndn::security::ValidationError& error) {
                NDN_LOG_ERROR("PolicyStatus Data validation failed: "
                              << badData.getName() << " reason=" << error);
                retryValidationFailure();
            });
    }

  void
  ServiceUser::onPolicyStatusTimeout(const ndn::Interest& interest, int attempt, uint64_t ticket)
  {
    const auto controller = policyStatusController(interest, ticket);
    if (!controller) {
      // A changed Controller binding terminates only this exact transport owner.
      const auto parameters = decodeControlQueryParameters(interest.getApplicationParameters());
      if (parameters && isCurrentPolicyStatusFetch(parameters->service, ticket,
                                                  interest.getName())) {
        finishPolicyStatusFetch(parameters->service, ticket);
      }
      return;
    }
    const auto parsed = controller ? parsePolicyStatusName(*controller, interest) :
                                    std::optional<PolicyStatusName>();
    if (!parsed || !isCurrentPolicyStatusFetch(parsed->serviceName, ticket, interest.getName())) {
      return;
    }
    if (attempt >= permissionFetchMaxAttempts()) {
      NDN_LOG_ERROR("PolicyStatus timeout: " << interest.getName()
                   << " attempt=" << attempt << " final=1");
      finishPolicyStatusFetch(parsed->serviceName, ticket);
      return;
    }
    const auto backoff = permissionFetchRetryBackoffMs(attempt);
    std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
    const auto found = m_policyStatusFetchInFlight.find(parsed->serviceName.toUri());
    if (found == m_policyStatusFetchInFlight.end() || found->second->ticket != ticket) return;
    const auto& owner = found->second;
    owner->pending.cancel();
    const std::weak_ptr<detail::PolicyStatusFetch> weakOwner = owner;
    owner->retry = m_scheduler.schedule(ndn::time::milliseconds(backoff),
      [this, weakOwner, ticket, interest, controller = *controller,
       service = parsed->serviceName, version = parsed->version, nextAttempt = attempt + 1] {
        if (weakOwner.expired() || !isCurrentPolicyStatusFetch(service, ticket, interest.getName())) {
          return;
        }
        fetchPolicyStatusFromController(controller, service, nextAttempt, version);
      });
  }

    void ServiceUser::maybeRefreshControllerVersionHint(
        const ndn::Name& serviceName,
        const std::optional<ControllerVersion>& messageVersion)
    {
        if (serviceName.empty() || !messageVersion || !messageVersion->isValid()) {
            return;
        }
        ndn::Name controllerPrefix;
        std::optional<ControllerVersion> current;
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            controllerPrefix = m_controllerPrefix;
            const auto state = m_revocationStates.find(serviceName.toUri());
            if (state != m_revocationStates.end() &&
                state->second.hasCurrentStatus()) {
                current = state->second.currentVersion();
            }
        }
        if (controllerPrefix.empty() ||
            (current && messageVersion->compare(*current) <= 0)) {
            return;
        }
        bool shouldFetch = false;
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            auto [coordinator, inserted] = m_policyRefreshCoordinators.try_emplace(
                serviceName.toUri(), serviceName);
            (void) inserted;
            const auto previousCandidate = coordinator->second.highestCandidate();
            const auto result = coordinator->second.observeHint(
                *messageVersion, true, nowMilliseconds());
            shouldFetch = result.outcome ==
                PolicyRefreshCoordinator::Outcome::FETCH_STARTED ||
                (result.outcome == PolicyRefreshCoordinator::Outcome::FETCH_COALESCED &&
                 (!previousCandidate || messageVersion->compare(*previousCandidate) > 0));
        }
        if (shouldFetch) {
            dispatchPolicyStatusFetch(controllerPrefix, serviceName, 1, std::nullopt, true);
        }
    }

    void ServiceUser::OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        // log message
        NDN_LOG_DEBUG("OnRequestAck: " << subscription.name);

        const auto actor = parseSvsProducerActor(subscription.producerPrefix, "provider");
        if (!actor || !subscription.packet || !validator ||
            !validator->isProducerSignerAuthorized(*subscription.packet, *actor)) return;
        const auto& packetName = subscription.packet->getName();
        if (packetName != subscription.name &&
            !(subscription.name.isPrefixOf(packetName) && packetName.size() == subscription.name.size() + 2 &&
              packetName[-2].isVersion() && packetName[-1].isSegment())) return;

        const auto ackV2 = resolvePendingReplyContext(subscription.name, "ACK", *actor);
        if (ackV2) {
            if (!isFresh(subscription)) return;
            traceSpec192StatusAck(ackV2->serviceName, "ack-data-received");
            const auto authenticationEvidence =
                makeAckAuthenticationEvidence(subscription);
            traceSpec192StatusAck(ackV2->serviceName, "ack-auth-evidence-created");
            logValidatedPublicationAudit(
                "user", "ACK", subscription,
                ackV2->requestId, ackV2->serviceName,
                ackV2->requesterName, ackV2->providerName);
            const auto ackReceiveUs = nowMicroseconds();
            recordObservedAckProvider(ackV2->serviceName,
                                      ackV2->providerName,
                                      ackReceiveUs);
            logControlTiming("user", "ACK_OBSERVED", ackV2->requestId,
                             {{"providerName", ackV2->providerName.toUri()},
                              {"serviceName", ackV2->serviceName.toUri()},
                              {"ackName", subscription.name.toUri()},
                              {"contentBytes", std::to_string(subscription.data.size())}});
            auto pendingCall = m_pendingCalls.find(ackV2->requestId);
            traceSpec192StatusAck(ackV2->serviceName,
                pendingCall == m_pendingCalls.end() ?
                    "ack-pending-request-missing" : "ack-pending-request-found");
            const auto phaseAttempt = pendingCall != m_pendingCalls.end() &&
                pendingCall->second.streamOptions &&
                pendingCall->second.streamOptions->attemptEpoch != 0 ?
                std::to_string(pendingCall->second.streamOptions->attemptEpoch) :
                std::string("request");
            if (m_timelineTrace) {
                logTimelineTrace("user", "first_ack_observed", ackV2->requestId,
                                 {{"providerName", ackV2->providerName.toUri()},
                                  {"serviceName", ackV2->serviceName.toUri()},
                                  {"ackName", subscription.name.toUri()},
                                  {"attempt", phaseAttempt}});
            }
            logAckMatchAttempt(ackV2->requestId,
                               subscription.name,
                               ackV2->providerName,
                               ackReceiveUs,
                               "pre_decrypt");
            const bool mayCollectResponseRetryCandidate =
                pendingCall != m_pendingCalls.end() &&
                pendingCall->second.responseRetryEnabled &&
                pendingCall->second.strategy == ndn_service_framework::tlv::FirstResponding &&
                !pendingCall->second.targetedMode &&
                !pendingCall->second.isCollaboration &&
                !pendingCall->second.acksHandler &&
                !pendingCall->second.ackCandidatesHandler &&
                pendingCall->second.providerSelected &&
                !pendingCall->second.selectedProvider.empty() &&
                !pendingCall->second.selectedProvider.equals(ackV2->providerName);
            // Observe a protected ACK from an authorized reader without admitting
            // it as a candidate or borrowing the selection collection lease.
            const bool readerOnly = pendingCall != m_pendingCalls.end() &&
                ((!pendingCall->second.providers.empty() &&
                  !containsName(pendingCall->second.providers, ackV2->providerName)) ||
                 (!mayCollectResponseRetryCandidate &&
                  (pendingCall->second.providerSelected || !pendingCall->second.selectedProvider.empty())));
            const auto readerObservation = readerOnly && !subscription.data.empty() ?
                beginBasicReaderObservation(ackV2->requestId, ackV2->providerName) : 0;
            if (readerOnly && readerObservation == 0) return;
            if (readerObservation == 0 && (pendingCall == m_pendingCalls.end() ||
                pendingCall->second.hasResponse ||
                pendingCall->second.timedOut ||
                ((pendingCall->second.providerSelected ||
                  !pendingCall->second.selectedProvider.empty()) &&
                 !mayCollectResponseRetryCandidate))) {
                traceSpec192StatusAck(ackV2->serviceName,
                                     "ack-rejected-before-decryption");
                if (pendingCall == m_pendingCalls.end()) {
                    logAckNoPending(ackV2->requestId,
                                    subscription.name,
                                    ackV2->providerName,
                                    ackReceiveUs);
                }
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_MATCH_SKIPPED_PRE_DECRYPT timestamp_us="
                          << ackReceiveUs
                          << " requestId=" << ackV2->requestId.toUri()
                          << " ackName=" << subscription.name.toUri()
                          << " providerName=" << ackV2->providerName.toUri()
                          << " pendingCallPresent=" << (pendingCall != m_pendingCalls.end())
                          << " hasResponse="
                          << (pendingCall != m_pendingCalls.end() &&
                              pendingCall->second.hasResponse)
                          << " timedOut="
                          << (pendingCall != m_pendingCalls.end() &&
                              pendingCall->second.timedOut)
                          << " providerSelected="
                          << (pendingCall != m_pendingCalls.end() &&
                              pendingCall->second.providerSelected)
                          << " selectedProvider="
                          << (pendingCall != m_pendingCalls.end() &&
                              !pendingCall->second.selectedProvider.empty() ?
                              pendingCall->second.selectedProvider.toUri() : "-"));
                NDN_LOG_TRACE("Skip decrypting irrelevant V2 ACK: "
                              << subscription.name);
                return;
            }

            // An explicit candidate set cannot admit this named Provider.
            // Retain the authenticated post-decode check for requested names.
            if (readerObservation == 0 && !pendingCall->second.providers.empty() &&
                !containsName(pendingCall->second.providers, ackV2->providerName)) {
                traceSpec192StatusAck(ackV2->serviceName,
                                     "ack-unrequested-provider-skipped-pre-decrypt");
                return;
            }

            const bool collectAckCandidates =
                readerObservation == 0 && shouldTrackAckDecrypt(pendingCall->second);
            if (collectAckCandidates) {
                const auto deadlineUs = pendingCall->second.ackWindowDeadlineUs;
                if (pendingCall->second.ackWindowExpired &&
                    deadlineUs > 0 &&
                    ackReceiveUs > deadlineUs) {
                    traceSpec192StatusAck(ackV2->serviceName,
                                         "ack-rejected-after-collection-window");
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SKIPPED_AFTER_ACK_WINDOW timestamp_us="
                              << ackReceiveUs
                              << " requestId=" << ackV2->requestId.toUri()
                              << " ackName=" << subscription.name.toUri()
                              << " providerName=" << ackV2->providerName.toUri()
                              << " deadlineUs=" << deadlineUs);
                    return;
                }
                if (deadlineUs == 0 || ackReceiveUs <= deadlineUs) {
                    ++pendingCall->second.ackDecryptsInFlight;
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_DECRYPT_IN_FLIGHT timestamp_us="
                              << ackReceiveUs
                              << " requestId=" << ackV2->requestId.toUri()
                              << " ackName=" << subscription.name.toUri()
                              << " providerName=" << ackV2->providerName.toUri()
                              << " inFlight=" << pendingCall->second.ackDecryptsInFlight
                              << " deadlineUs=" << deadlineUs);
                }
            }

            if(subscription.data.size() > 0){
                traceSpec192StatusAck(ackV2->serviceName,
                                     "ack-hybrid-decrypt-dispatch");
                const auto decryptStartUs = nowMicroseconds();
                if (m_timelineTrace) {
                    logTimelineTrace("user", "ack_decrypt_start", ackV2->requestId,
                                     {{"providerName", ackV2->providerName.toUri()},
                                      {"serviceName", ackV2->serviceName.toUri()}});
                }
                if (plaintextAckDiagEnabled()) {
                    ndn::Buffer plaintext(subscription.data.begin(), subscription.data.end());
                    const auto decryptEndUs = nowMicroseconds();
                    logCryptoDiag("user", "ack", "decrypt", "plaintext",
                                  "success", decryptStartUs, decryptEndUs,
                                  subscription.name, plaintext.size());
                    decodeRequestAck(ackV2->providerName,
                                                          ackV2->serviceName,
                                                          ackV2->requestId,
                                                          plaintext,
                                                          authenticationEvidence, readerObservation);
                    return;
                }
                ndn::Block ackEnvelope;
                try {
                    ackEnvelope = ndn::Block(subscription.data);
                }
                catch (const std::exception&) {
                    if (readerObservation == 0) throw;
                    releaseBasicReaderObservation(ackV2->requestId, ackV2->providerName, readerObservation);
                    return;
                }
                bool hybridDispatched = false;
                try {
                    hybridDispatched = decryptHybridMessage(
                        subscription.name,
                        ackEnvelope,
                        ackV2->providerName, ackV2->serviceName, ackV2->requestId,
                        [this, providerName = ackV2->providerName,
                         serviceName = ackV2->serviceName,
                         requestId = ackV2->requestId,
                         authenticationEvidence,
                         readerObservation,
                         subscriptionName = ndn::Name(subscription.name),
                         decryptStartUs](const ndn::Buffer& buffer) {
                            const auto decryptEndUs = nowMicroseconds();
                            if (m_timelineTrace) {
                                logTimelineTrace("user", "ack_decrypt_done", requestId,
                                                 {{"providerName", providerName.toUri()},
                                                  {"serviceName", serviceName.toUri()},
                                                  {"duration_us",
                                                   std::to_string(decryptEndUs >= decryptStartUs ?
                                                                  decryptEndUs - decryptStartUs : 0)}});
                            }
                            logCryptoDiag("user", "ack", "decrypt", "hybrid",
                                          "success", decryptStartUs, decryptEndUs,
                                          subscriptionName, buffer.size());
                            decodeRequestAck(providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  buffer,
                                                                  authenticationEvidence, readerObservation);
                        },
                        [this, providerName = ackV2->providerName,
                         serviceName = ackV2->serviceName,
                         requestId = ackV2->requestId,
                         readerObservation,
                         subscriptionName = ndn::Name(subscription.name),
                         decryptStartUs](const std::string& error) {
                            const auto decryptEndUs = nowMicroseconds();
                            logCryptoDiag("user", "ack", "decrypt", "hybrid",
                                          "failure", decryptStartUs, decryptEndUs,
                                          subscriptionName, 0, error);
                            if (readerObservation != 0) {
                                releaseBasicReaderObservation(requestId, providerName, readerObservation);
                                return;
                            }
                            OnRequestAckDecryptionErrorCallback(providerName,
                                                                serviceName,
                                                                requestId,
                                                                error);
                        });
                }
                catch (const std::exception&) {
                    if (readerObservation == 0) throw;
                    releaseBasicReaderObservation(ackV2->requestId, ackV2->providerName, readerObservation);
                    return;
                }
                if (hybridDispatched) {
                    return;
                }
                if (readerObservation != 0) {
                    releaseBasicReaderObservation(ackV2->requestId, ackV2->providerName, readerObservation);
                    return;
                }
                OnRequestAckDecryptionErrorCallback(ackV2->providerName,
                                                    ackV2->serviceName,
                                                    ackV2->requestId,
                                                    "invalid hybrid ACK envelope");
                return;
                activeNacConsumer().consume(
                            ndn::Name(subscription.name),
                            makeNacInlineContentBlock(subscription.data),
                            [this, providerName = ackV2->providerName,
                             serviceName = ackV2->serviceName,
                             requestId = ackV2->requestId,
                             authenticationEvidence,
                             subscriptionName = ndn::Name(subscription.name),
                             decryptStartUs](const ndn::Buffer& buffer) {
                                const auto decryptEndUs = nowMicroseconds();
                                if (m_timelineTrace) {
                                    logTimelineTrace("user", "ack_decrypt_done", requestId,
                                                     {{"providerName", providerName.toUri()},
                                                      {"serviceName", serviceName.toUri()},
                                                      {"duration_us",
                                                       std::to_string(decryptEndUs >= decryptStartUs ?
                                                                      decryptEndUs - decryptStartUs : 0)}});
                                }
                                logCryptoDiag("user", "ack", "decrypt", "normal",
                                              "success", decryptStartUs, decryptEndUs,
                                              subscriptionName, buffer.size());
                                OnRequestAckDecryptionSuccessCallback(providerName,
                                                                      serviceName,
                                                                      requestId,
                                                                      buffer,
                                                                      authenticationEvidence);
                            },
                         [this, providerName = ackV2->providerName,
                          serviceName = ackV2->serviceName,
                          requestId = ackV2->requestId,
                          authenticationEvidence,
                          subscriptionName = ndn::Name(subscription.name),
                             decryptStartUs](const std::string& error) {
                                const auto decryptEndUs = nowMicroseconds();
                                logCryptoDiag("user", "ack", "decrypt", "normal",
                                              "failure", decryptStartUs, decryptEndUs,
                                              subscriptionName, 0, error);
                                OnRequestAckDecryptionErrorCallback(providerName,
                                                                    serviceName,
                                                                    requestId,
                                                                    error);
                            });
            }else{
                traceSpec192StatusAck(ackV2->serviceName,
                                     "ack-nac-consume-dispatch");
                const auto decryptStartUs = nowMicroseconds();
                activeNacConsumer().consume(
                            ndn::Name(subscription.name),
                            [this, providerName = ackV2->providerName,
                             serviceName = ackV2->serviceName,
                             requestId = ackV2->requestId,
                             authenticationEvidence,
                             subscriptionName = ndn::Name(subscription.name),
                             decryptStartUs](const ndn::Buffer& buffer) {
                                const auto decryptEndUs = nowMicroseconds();
                                logCryptoDiag("user", "ack", "decrypt", "normal",
                                              "success", decryptStartUs, decryptEndUs,
                                              subscriptionName, buffer.size());
                                OnRequestAckDecryptionSuccessCallback(providerName,
                                                                      serviceName,
                                                                      requestId,
                                                                      buffer,
                                                                      authenticationEvidence);
                            },
                            [this, providerName = ackV2->providerName,
                             serviceName = ackV2->serviceName,
                             requestId = ackV2->requestId,
                             subscriptionName = ndn::Name(subscription.name),
                             decryptStartUs](const std::string& error) {
                                const auto decryptEndUs = nowMicroseconds();
                                logCryptoDiag("user", "ack", "decrypt", "normal",
                                              "failure", decryptStartUs, decryptEndUs,
                                              subscriptionName, 0, error);
                                OnRequestAckDecryptionErrorCallback(providerName,
                                                                    serviceName,
                                                                    requestId,
                                                                    error);
                            });
            }
            return;
        }

        NDN_LOG_WARN("Reject unmatched or inactive ACK: " << subscription.name);
    }

    void ServiceUser::OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {

        NDN_LOG_DEBUG("OnResponse: " << subscription.name);

        ndn::Name requesterName, providerName, ServiceName, RequestId;
        const auto actor = parseSvsProducerActor(subscription.producerPrefix, "provider");
        if (!actor || !subscription.packet || !validator ||
            !validator->isProducerSignerAuthorized(*subscription.packet, *actor)) return;
        const auto& packetName = subscription.packet->getName();
        if (packetName != subscription.name &&
            !(subscription.name.isPrefixOf(packetName) && packetName.size() == subscription.name.size() + 2 &&
              packetName[-2].isVersion() && packetName[-1].isSegment())) return;

        const auto resultsV2 = resolvePendingReplyContext(subscription.name, "RESPONSE", *actor, true);
        if (!resultsV2) {
            NDN_LOG_WARN("Reject unmatched or inactive response: " << subscription.name);
            return;
        }
        if (!isFresh(subscription)) return;
        requesterName = resultsV2->requesterName;
        providerName = resultsV2->providerName;
        ServiceName = resultsV2->serviceName;
        RequestId = resultsV2->requestId;
        const bool traceSpec192Status = isSpec192StatusService(ServiceName);
        if (traceSpec192Status) {
            traceSpec192StatusResponse("response-subscription-entered");
        }

        if (!subscription.packet) {
            NDN_LOG_WARN("Reject RESPONSE without its validated Data packet: "
                         << subscription.name);
            return;
        }
        const auto& responseSignatureInfo = subscription.packet->getSignatureInfo();
        if (!responseSignatureInfo.hasKeyLocator() ||
            responseSignatureInfo.getKeyLocator().getType() != ndn::tlv::Name) {
            NDN_LOG_WARN("Reject RESPONSE without a named KeyLocator: "
                         << subscription.name);
            return;
        }
        const auto& responseSignerName = responseSignatureInfo.getKeyLocator().getName();
        const auto responseProviderIdentity =
            ndn_service_framework::detail::matchResponseProviderIdentity(
                providerName, responseSignerName);
        if (!responseProviderIdentity) {
            ++m_runtimeDiagnostics.responseRejectedProviderIdentity;
            NDN_LOG_WARN("Reject RESPONSE whose KeyLocator identity does not match "
                         "the Provider in its name: " << subscription.name);
            return;
        }
        if (traceSpec192Status) {
            traceSpec192StatusResponse("response-provider-authenticated");
        }
        const std::string responseSignerCertificate = responseSignerName.toUri();
        const std::string authenticatedProviderIdentity =
            responseProviderIdentity->toUri();

        logValidatedPublicationAudit(
            "user", "RESPONSE", subscription,
            RequestId, ServiceName, requesterName, providerName);

        const ndn::Name wireResponseName(subscription.name);
        const ndn::Name responseName = makeResponseNameV2(providerName, requesterName, ServiceName, RequestId);
        auto responsePending = m_pendingCalls.find(RequestId);
        if (responsePending == m_pendingCalls.end()) {
            if (traceSpec192Status) {
                traceSpec192StatusResponse("response-pending-call-missing");
            }
            ++m_runtimeDiagnostics.callbackSkippedNoPending;
            ++m_runtimeDiagnostics.responseAfterPendingTimeout;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << RequestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " responseName=" << responseName.toUri());
            return;
        }
        const bool expectMultipleResponses =
            !responsePending->second.isCollaboration &&
            responsePending->second.expectedResponseProviders.size() > 1;
        if ((responsePending->second.hasResponse && !expectMultipleResponses) ||
            (expectMultipleResponses &&
             containsName(responsePending->second.responseProviders, providerName)) ||
            containsName(responsePending->second.responseDecryptProvidersInFlight,
                         providerName)) {
            if (traceSpec192Status) {
                traceSpec192StatusResponse(
                    "response-duplicate-or-decrypt-in-flight");
            }
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_DUPLICATE_SKIPPED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << RequestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " responseName=" << responseName.toUri());
            return;
        }
        if (expectMultipleResponses &&
            !containsName(responsePending->second.expectedResponseProviders, providerName)) {
            if (traceSpec192Status) {
                traceSpec192StatusResponse("response-provider-not-selected");
            }
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_REJECTED_UNSELECTED_PROVIDER timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << RequestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " responseName=" << responseName.toUri());
            return;
        }
        if (traceSpec192Status) {
            traceSpec192StatusResponse("response-admitted-for-decryption");
        }
        addUniqueName(responsePending->second.responseDecryptProvidersInFlight,
                      providerName);
        if (responsePending != m_pendingCalls.end() &&
            responsePending->second.responseObservedAtUs == 0) {
            responsePending->second.responseObservedAtUs = nowMicroseconds();
        }
        updateRequestLifecycleState(RequestId, RequestLifecycleState::RESPONSE_OBSERVED);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_OBSERVED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << RequestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << ServiceName.toUri()
                  << " responseName=" << responseName.toUri()
                  << " contentBytes=" << subscription.data.size());
        logControlTiming("user", "RESPONSE_OBSERVED", RequestId,
                         {{"providerName", providerName.toUri()},
                          {"serviceName", ServiceName.toUri()},
                          {"responseName", responseName.toUri()},
                          {"contentBytes", std::to_string(subscription.data.size())}});
        if (m_timelineTrace) {
            logTimelineTrace("user", "response_observed", RequestId,
                             {{"providerName", providerName.toUri()},
                              {"serviceName", ServiceName.toUri()},
                              {"responseName", responseName.toUri()}});
        }
        const auto decryptStartUs = nowMicroseconds();
        if (m_timelineTrace) {
            logTimelineTrace("user", "response_decrypt_start", RequestId,
                             {{"providerName", providerName.toUri()},
                              {"serviceName", ServiceName.toUri()}});
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPT_START timestamp_us="
                  << decryptStartUs
                  << " requestId=" << RequestId.toUri()
                  << " responseName=" << responseName.toUri());
        const std::string responseDataName = subscription.packet->getName().toUri();
        const auto responseWire = subscription.packet->wireEncode();
        const std::string responseWireDigest = sha256DigestString(ndn::Buffer(
            responseWire.data(), responseWire.data() + responseWire.size()));
        auto onSuccess = [this, responseName, RequestId, decryptStartUs,
                          responseDataName, responseSignerCertificate,
                          responseWireDigest, authenticatedProviderIdentity,
                          traceSpec192Status](
                              const ndn::Buffer& buffer) {
            if (traceSpec192Status) {
                traceSpec192StatusResponse("response-decryption-complete");
            }
            const auto decryptEndUs = nowMicroseconds();
            auto pendingIt = m_pendingCalls.find(RequestId);
            if (pendingIt != m_pendingCalls.end()) {
                pendingIt->second.responseDecryptedAtUs = decryptEndUs;
            }
            updateRequestLifecycleState(RequestId, RequestLifecycleState::RESPONSE_DECRYPTED);
            logCryptoDiag("user", "response", "decrypt", "normal",
                          "success", decryptStartUs, decryptEndUs,
                          responseName, buffer.size());
            if (m_timelineTrace) {
                logTimelineTrace("user", "response_decrypt_done", RequestId,
                                 {{"responseName", responseName.toUri()},
                                  {"duration_us",
                                   std::to_string(decryptEndUs >= decryptStartUs ?
                                                  decryptEndUs - decryptStartUs : 0)}});
            }
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPT_DONE timestamp_us="
                      << decryptEndUs
                      << " requestId=" << RequestId.toUri()
                      << " responseName=" << responseName.toUri()
                      << " payloadBytes=" << buffer.size()
                      << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                            decryptEndUs - decryptStartUs : 0));
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_RECEIVED timestamp_us="
                      << decryptEndUs
                      << " requestId=" << RequestId.toUri()
                      << " responseName=" << responseName.toUri());
            dispatchDecryptedResponseByName(
                responseName, RequestId, buffer, responseDataName,
                responseSignerCertificate, responseWireDigest,
                authenticatedProviderIdentity);
        };
        auto onError = [this, providerName, ServiceName, RequestId,
                        responseName, decryptStartUs](const std::string& error) {
            if (isSpec192StatusService(ServiceName)) {
                traceSpec192StatusResponse("response-decryption-failed");
            }
            auto pendingIt = m_pendingCalls.find(RequestId);
            if (pendingIt != m_pendingCalls.end()) {
                removeName(pendingIt->second.responseDecryptProvidersInFlight,
                           providerName);
            }
            const auto decryptEndUs = nowMicroseconds();
            logCryptoDiag("user", "response", "decrypt", "normal",
                          "failure", decryptStartUs, decryptEndUs,
                          responseName, 0, error);
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPT_FAILED timestamp_us="
                      << decryptEndUs
                      << " requestId=" << RequestId.toUri()
                      << " responseName=" << responseName.toUri()
                      << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                            decryptEndUs - decryptStartUs : 0)
                      << " error=" << error);
            OnResponseDecryptionErrorCallback(providerName, ServiceName,
                                              RequestId, error);
        };

        if(subscription.data.size() > 0){
            if (plaintextResponseDiagEnabled()) {
                ndn::Buffer plaintext(subscription.data.begin(), subscription.data.end());
                const auto decryptEndUs = nowMicroseconds();
                auto pendingIt = m_pendingCalls.find(RequestId);
                if (pendingIt != m_pendingCalls.end()) {
                    pendingIt->second.responseDecryptedAtUs = decryptEndUs;
                }
                updateRequestLifecycleState(RequestId, RequestLifecycleState::RESPONSE_DECRYPTED);
                logCryptoDiag("user", "response", "decrypt", "plaintext",
                              "success", decryptStartUs, decryptEndUs,
                              responseName, plaintext.size());
                if (m_timelineTrace) {
                    logTimelineTrace("user", "response_decrypt_done", RequestId,
                                     {{"responseName", responseName.toUri()},
                                      {"duration_us",
                                       std::to_string(decryptEndUs >= decryptStartUs ?
                                                      decryptEndUs - decryptStartUs : 0)}});
                }
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPT_DONE timestamp_us="
                          << decryptEndUs
                          << " requestId=" << RequestId.toUri()
                          << " responseName=" << responseName.toUri()
                          << " payloadBytes=" << plaintext.size()
                          << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                decryptEndUs - decryptStartUs : 0)
                          << " mode=plaintext");
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_RECEIVED timestamp_us="
                          << decryptEndUs
                          << " requestId=" << RequestId.toUri()
                          << " responseName=" << responseName.toUri());
                dispatchDecryptedResponseByName(
                    responseName, RequestId, plaintext, responseDataName,
                    responseSignerCertificate, responseWireDigest,
                    authenticatedProviderIdentity);
                return;
            }
            if (decryptHybridMessage(responseName,
                                     ndn::Block(subscription.data),
                                     providerName, ServiceName, RequestId,
                                     onSuccess,
                                     onError)) {
                if (traceSpec192Status) {
                    traceSpec192StatusResponse("response-decryption-started");
                }
                return;
            }
            onError("invalid hybrid response envelope");
            return;
            activeNacConsumer().consume(
                        ndn::Name(subscription.name),
                        makeNacInlineContentBlock(subscription.data),
                        onSuccess,
                        onError);
        }else{
            activeNacConsumer().consume(
                        ndn::Name(subscription.name),
                        onSuccess,
                        onError);
        }
    }

void ServiceUser::OnRequestAckDecryptionSuccessCallback(
    const ndn::Name& providerName,
    const ndn::Name& serviceName,
    const ndn::Name& requestID,
    const ndn::Buffer& buffer,
    AckAuthenticationEvidence authenticationEvidence)
{
    decodeRequestAck(providerName, serviceName, requestID, buffer,
                     std::move(authenticationEvidence), 0);
}

void ServiceUser::decodeRequestAck(
    const ndn::Name& providerName, const ndn::Name& serviceName,
    const ndn::Name& requestID, const ndn::Buffer& buffer,
    AckAuthenticationEvidence authenticationEvidence, uint64_t readerObservation)
{
    traceSpec192StatusAck(serviceName, "ack-decryption-complete");
    auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());
    auto decodeAndPost = [this, providerName, serviceName, requestID, raw,
                          authenticationEvidence, readerObservation]() mutable {
        RequestAckMessage AckMessage;
        std::string error;
        try {
            ndn::Block block(ndn::span<const uint8_t>(raw->data(), raw->size()));
            if (!AckMessage.WireDecode(block)) {
                error = "wire_decode_failed";
            }
        }
        catch (const std::exception& e) {
            error = e.what();
        }
        traceSpec192StatusAck(serviceName,
                              error.empty() ? "ack-wire-decode-complete" :
                                              "ack-wire-decode-failed");

        boost::asio::post(m_face.getIoContext(),
            [this, providerName, serviceName, requestID,
             authenticationEvidence, readerObservation,
             AckMessage = std::move(AckMessage), error = std::move(error)]() mutable {
                if (!error.empty()) {
                    NDN_LOG_ERROR("RequestAckMessage decode failed: " << error);
                    if (readerObservation != 0) {
                        releaseBasicReaderObservation(requestID, providerName, readerObservation);
                        return;
                    }
                    auto pendingCall = m_pendingCalls.find(requestID);
                    if (pendingCall != m_pendingCalls.end() &&
                        (pendingCall->second.acksHandler ||
                         pendingCall->second.ackCandidatesHandler) &&
                        pendingCall->second.ackDecryptsInFlight > 0) {
                        --pendingCall->second.ackDecryptsInFlight;
                    }
                    return;
                }
                finishRequestAckOnEventLoop(providerName, serviceName,
                                            requestID, std::move(AckMessage),
                                            std::move(authenticationEvidence), readerObservation);
            });
    };

    const bool queued =
        buffer.size() > HybridMessageCrypto::INLINE_CRYPTO_MAX_BYTES &&
        m_ackProcessingPool.getThreadCount() != 0 &&
        m_ackProcessingPool.post(decodeAndPost);
    if (!queued) {
        decodeAndPost();
    }
}

void ServiceUser::finishRequestAckOnEventLoop(
    const ndn::Name& providerName,
    const ndn::Name& serviceName,
    const ndn::Name& requestID,
    ndn_service_framework::RequestAckMessage AckMessage,
    AckAuthenticationEvidence authenticationEvidence, uint64_t readerObservation)
{
        const auto fail = [&](const std::string& error) {
            if (readerObservation != 0)
                releaseBasicReaderObservation(requestID, providerName, readerObservation);
            else
                OnRequestAckDecryptionErrorCallback(providerName, serviceName, requestID, error);
        };
        if (readerObservation != 0) {
            if (!isBasicReaderObservationCurrent(requestID, providerName, readerObservation)) {
                fail("reader observation no longer current");
                return;
            }
            const auto& call = m_pendingCalls.at(requestID);
            const auto& token = call.requestMessage.getUserToken();
            const auto version = AckMessage.hasControllerVersion() ?
                std::optional<ControllerVersion>(AckMessage.getControllerVersion()) : std::nullopt;
            if (call.serviceName != serviceName || !AckMessage.getStatus() ||
                !AckMessage.hasProviderSecurityProfile() ||
                (m_useTokens && (token.empty() || AckMessage.getUserToken() != token ||
                                 AckMessage.getProviderToken().empty())) ||
                !authenticationEvidence.trustSchemaValidated ||
                authenticationEvidence.signerIdentity != providerName.toUri() ||
                authenticationEvidence.signerKeyLocator.empty() || authenticationEvidence.wireDigest.empty() ||
                !isAcceptablePolicyEpoch(serviceName, AckMessage.getPolicyEpoch()) ||
                !isAcceptableControllerVersion(serviceName, version)) {
                fail("reader observation rejected");
                return;
            }
        }
        if (AckMessage.hasProviderSecurityProfile() && !AckMessage.hasProviderEncryptionCertificate()) {
            const auto pending = m_pendingCalls.find(requestID);
            if (pending == m_pendingCalls.end()) return;
            const auto now = nowMicroseconds();
            const auto deadline = pending->second.requestDeadlineUs != 0 ?
                pending->second.requestDeadlineUs :
                (pending->second.timeoutMs > 0 ? pending->second.createdAtUs +
                    static_cast<uint64_t>(pending->second.timeoutMs) * 1000 : 0);
            if (deadline != 0 && deadline <= now) {
                fail("request deadline expired before security profile resolution");
                return;
            }
            try {
                if (!m_securityProfiles)
                    m_securityProfiles = std::make_unique<SecurityProfileStore>(m_face, validator);
                const auto reference = AckMessage.getProviderSecurityProfile();
                // timeoutMs <= 0 means no invocation deadline. The metadata
                // Interest remains bounded without imposing one on the call.
                const auto remaining = deadline == 0 ? ndn::time::milliseconds(5000) :
                    ndn::time::milliseconds(std::max<uint64_t>(1, (deadline - now + 999) / 1000));
                m_securityProfiles->resolve(reference, providerName, remaining,
                    [this, providerName, serviceName, requestID,
                     readerObservation, message = std::move(AckMessage), evidence = std::move(authenticationEvidence)]
                    (auto record, const auto& error) mutable {
                        const auto reject = [&](const std::string& reason) {
                            if (readerObservation != 0)
                                releaseBasicReaderObservation(requestID, providerName, readerObservation);
                            else
                                OnRequestAckDecryptionErrorCallback(providerName, serviceName, requestID, reason);
                        };
                        if (!record) {
                            reject("security profile rejected: " + error);
                            return;
                        }
                        try {
                            message.resolveProviderSecurityProfile(
                                std::shared_ptr<const EncryptionCertificateAdvertisement>(record, &record->advertisement));
                            if (message.hasSelectionInputKeyOffer()) {
                                auto offer = message.getSelectionInputKeyOffer();
                                const std::map<std::string, std::string> stable{
                                    {"recipientCertName", record->profile.certificate.getName().toUri()},
                                    {"recipientCertDigest", record->publicKeyDigest},
                                    {"recipientPublicKey", record->publicKeyHex}};
                                for (const auto& [field, value] : stable) {
                                    if (offer.hasField(field) && offer.getField(field) != value)
                                        throw std::invalid_argument("ACK contradicts validated security profile");
                                    offer.setField(field, value);
                                }
                                message.resolveSelectionInputKeyOffer(offer);
                            }
                            finishRequestAckOnEventLoop(providerName, serviceName, requestID,
                                                       std::move(message), std::move(evidence), readerObservation);
                        }
                        catch (const std::exception& failure) {
                            reject(failure.what());
                        }
                    });
            }
            catch (const std::exception& error) {
                fail(error.what());
            }
            return;
        }
        if (readerObservation != 0) {
            rememberBasicProfileReader(providerName, serviceName, AckMessage, authenticationEvidence);
            releaseBasicReaderObservation(requestID, providerName, readerObservation);
            return;
        }
        NDN_LOG_DEBUG("OnRequestAckDecryptionSuccessCallback: "
                     << providerName.toUri() << " "
                     << serviceName.toUri() << " "
                     << requestID.toUri());
        const auto ackPayload = AckMessage.getPayload();
        const std::string ackPayloadText(
            reinterpret_cast<const char*>(ackPayload.data()),
            ackPayload.size());
        NDN_LOG_DEBUG("[ServiceUser] ACK received timestampMs="
                  << nowMilliseconds()
                  << " requestId=" << requestID.toUri()
                  << " providerName=" << providerName.toUri()
                  << " status=" << AckMessage.getStatus()
                  << " message=" << AckMessage.getMessage()
                  << " userToken=" << AckMessage.getUserToken()
                  << " providerToken=" << AckMessage.getProviderToken()
                  << " payload=" << ackPayloadText);
        const ndn::Name ackName =
            ndn_service_framework::makeRequestAckNameV2(providerName, identity,
                                                        serviceName, requestID);
        const bool accepted = handleRequestAckByName(
            ackName, AckMessage, std::move(authenticationEvidence));
        traceSpec192StatusAck(serviceName,
                              accepted ? "ack-candidate-stored" :
                                         "ack-candidate-rejected-after-decode");
        if (!accepted) {
            NDN_LOG_DEBUG("V2 ACK did not update pending call state for requestID: "
                          << requestID.toUri());
        }
}

    void ServiceUser::OnRequestAckDecryptionErrorCallback(
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestID,
        const std::string& error)
    {
        traceSpec192StatusAck(serviceName, "ack-decryption-failed");
        // log error
        NDN_LOG_ERROR("OnRequestAckDecryptionErrorCallback: "
                      << providerName.toUri() << serviceName.toUri()
                      << requestID.toUri() << " with error: " << error);
        auto pendingCall = m_pendingCalls.find(requestID);
        if (pendingCall != m_pendingCalls.end() &&
            shouldTrackAckDecrypt(pendingCall->second) &&
            pendingCall->second.ackDecryptsInFlight > 0) {
            --pendingCall->second.ackDecryptsInFlight;
        }
    }

    std::string ServiceUser::selectionRetryContext(const ndn::Name& requestId, const ndn::Name& provider) const
    {
        const auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end()) throw std::runtime_error("Selection retry lacks pending owner");
        ndn::util::Sha256 digest;
        const auto append = [&](const uint8_t* bytes, size_t size) {
            digest << std::to_string(size) << ":" << (size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(bytes), size));
        };
        const auto block = [&](const ndn::Block& value) { append(value.data(), value.size()); };
        const auto text = [&](const std::string& value) { append(reinterpret_cast<const uint8_t*>(value.data()), value.size()); };
        block(identity.wireEncode());
        block(provider.wireEncode());
        block(requestId.wireEncode());
        block(pending->second.serviceName.wireEncode());
        block(pending->second.requestMessage.WireEncode());
        bool foundOffer = false;
        for (const auto& ack : pending->second.requestAcks) {
            if (ack.providerName != provider) continue;
            if (foundOffer) throw std::runtime_error("Selection retry has ambiguous Provider offer");
            foundOffer = true;
            block(ack.message.WireEncode());
        }
        text(foundOffer ? "offer" : "no-offer");
        const auto providerUri = provider.toUri();
        const auto token = pending->second.providerTokens.find(providerUri);
        text(token == pending->second.providerTokens.end() ? "" : token->second);
        for (const auto* assignments : {&pending->second.collaborationAssignments,
                                       &pending->second.selectionAssignmentPayloads}) {
            const auto assignment = assignments->find(providerUri);
            text(assignment == assignments->end() ? "absent" : "present");
            if (assignment != assignments->end()) append(assignment->second.data(), assignment->second.size());
        }
        append(pending->second.collaborationPlan.sharedAssignmentMetadata.data(),
               pending->second.collaborationPlan.sharedAssignmentMetadata.size());
        text(pending->second.collaborationCommittedPlanDigest);
        for (const auto& participant : pending->second.collaborationCommittedParticipants) {
            block(participant.provider.wireEncode());
            block(participant.service.wireEncode());
            text(participant.role);
        }
        if (pending->second.deploymentPlan) block(pending->second.deploymentPlan->WireEncode());
        if (pending->second.streamOptions) block(pending->second.streamOptions->wireEncode());
        return digest.toString();
    }

    void ServiceUser::PublishServiceSelectionMessageV2(const ndn::Name& providerName,
                                                          const ndn::Name& serviceName,
                                                          const ndn::Name& requestId,
                                                          std::function<void(bool)> onPublished)
    {
        const auto invocation = m_pendingCalls.find(requestId);
        if (invocation == m_pendingCalls.end() || invocation->second.serviceName != serviceName)
            throw std::runtime_error("Selection service differs from pending invocation context");
        if (invocation->second.timedOut) { if (onPublished) onPublished(false); return; }
        const auto rejectPreparation = [&](const std::string& error) {
            SelectionExecutionStatus failed;
            failed.providerName = providerName;
            failed.serviceName = serviceName;
            failed.requestId = requestId;
            failed.state = SelectionExecutionState::Failed;
            failed.message = "Selection preparation failed: " + error;
            failed.updatedAtUs = nowMicroseconds();
            invocation->second.selectionStatusesByProvider[providerName.toUri()] = std::move(failed);
            invocation->second.timedOut = true;
            boost::asio::post(m_face.getIoContext(), [this, requestId] {
                cancelStreamRequest(requestId);
                finalizeTimedOutPendingCall(requestId);
            });
            if (onPublished) onPublished(false);
        };
        if (!authorizeControllerTransition(serviceName,
                                           ProtectedTransition::SELECTION)) {
            NDN_LOG_ERROR("Reject Selection under revoked Controller status requestId="
                          << requestId.toUri());
            SelectionExecutionStatus failed;
            failed.providerName = providerName;
            failed.serviceName = serviceName;
            failed.requestId = requestId;
            failed.state = SelectionExecutionState::Failed;
            failed.message = "Selection rejected by current Controller authorization";
            failed.updatedAtUs = nowMicroseconds();
            invocation->second.selectionStatusesByProvider[providerName.toUri()] = std::move(failed);
            invocation->second.timedOut = true;
            boost::asio::post(m_face.getIoContext(), [this, requestId] {
                cancelStreamRequest(requestId);
                finalizeTimedOutPendingCall(requestId);
            });
            if (onPublished) onPublished(false);
            return;
        }
        const auto existing = m_pendingCalls.find(requestId);
        if (existing != m_pendingCalls.end()) {
            const auto prepared = existing->second.preparedSelectionsByProvider.find(providerName.toUri());
            if (prepared != existing->second.preparedSelectionsByProvider.end()) {
                const auto context = existing->second.selectionPreparationContextsByProvider.find(providerName.toUri());
                if (context == existing->second.selectionPreparationContextsByProvider.end() ||
                    context->second != selectionRetryContext(requestId, providerName))
                    throw std::runtime_error("Selection retry request, offer, recipient or plan context changed");
                const auto current = getControllerVersion(serviceName);
                if (prepared->second.getPolicyEpoch() != getCurrentPolicyEpoch(serviceName) ||
                    (prepared->second.hasControllerVersion() != bool(current)) ||
                    (current && prepared->second.getControllerVersion().compare(*current) != 0))
                    throw std::runtime_error("Selection retry protection context changed");
                const auto accepted = existing->second.selectionOuterNamesByProvider.find(providerName.toUri());
                if (accepted == existing->second.selectionOuterNamesByProvider.end()) {
                    if (containsName(existing->second.selectionPublishedProviders, providerName) && m_localPublicationHandler) {
                        const auto wire = prepared->second.WireEncode();
                        m_localPublicationHandler(makeServiceSelectionNameV2(identity, providerName, serviceName, requestId),
                            ndn::Buffer(wire.begin(), wire.end()));
                        if (onPublished) onPublished(true);
                        return;
                    }
                    throw std::runtime_error("Selection publication already prepared or awaiting original receipt");
                }
                if (!m_svsps) throw std::runtime_error("Selection original publisher unavailable");
                auto& originals = existing->second.selectionOriginalPacketsByProvider[providerName.toUri()];
                if (originals.empty()) originals = copyOriginalSvsPublication(*m_svsps, accepted->second,
                    makeServiceSelectionNameV2(identity, providerName, serviceName, requestId));
                for (const auto& original : originals) {
                    const auto owned = std::make_shared<ndn::Data>(original);
                    m_IMS.insert(*owned);
                    m_face.put(*owned);
                }
                if (onPublished) onPublished(true);
                return;
            }
        }
        NDN_LOG_DEBUG("PublishServiceSelectionMessageV2: "
                     << providerName.toUri()
                     << serviceName.toUri()
                     << requestId.toUri());
        NDN_LOG_DEBUG("[ServiceUser] PublishServiceSelectionMessage called timestampMs="
                  << nowMilliseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri());

        ServiceSelectionMessage selectionMessage;
        selectionMessage.setRequestIDs({requestId.toUri()});
        selectionMessage.setPolicyEpoch(getCurrentPolicyEpoch(serviceName));
        if (const auto version = getControllerVersion(serviceName))
            selectionMessage.setControllerVersion(*version);
        auto pendingIt = m_pendingCalls.find(requestId);
        bool providerTokenPresent = false;
        SelectionProviderEntry providerEntry;
        providerEntry.providerName = providerName;
        if (pendingIt != m_pendingCalls.end()) {
            auto tokenIt =
                pendingIt->second.providerTokens.find(providerName.toUri());
            if (m_useTokens && tokenIt != pendingIt->second.providerTokens.end()) {
                providerEntry.providerTokenHash =
                    computeSelectionProviderTokenProofHash(identity,
                                                           providerName,
                                                           serviceName,
                                                           tokenIt->second);
                providerTokenPresent = true;
            }
        }
        if (!m_useTokens) {
            providerTokenPresent = true;
        }
        if (pendingIt != m_pendingCalls.end()) {
            if (pendingIt->second.deploymentPlan) {
                selectionMessage.setDeploymentPlan(*pendingIt->second.deploymentPlan);
            }
            else if (pendingIt->second.requestMessage.hasDeploymentIntent()) {
                const auto ackIt = std::find_if(
                    pendingIt->second.requestAcks.begin(),
                    pendingIt->second.requestAcks.end(),
                    [&providerName] (const StoredAck& ack) {
                        return ack.providerName.equals(providerName);
                    });
                if (ackIt == pendingIt->second.requestAcks.end() ||
                    !ackIt->message.hasProviderCapabilityOffer()) {
                    throw std::runtime_error(
                        "deployment Selection requires a negotiated capability offer");
                }
                DeploymentPlan plan;
                plan.setField("requestId", requestId.toUri());
                plan.setField("attempt", "1");
                plan.setField("requesterIdentity", identity.toUri());
                plan.setField("intentDigest",
                              pendingIt->second.requestMessage.getDeploymentIntent().computeDigest());
                plan.setField("member.0.provider", providerName.toUri());
                plan.setField("member.0.role", "primary");
                plan.setField("member.0.offerDigest",
                              ackIt->message.getProviderCapabilityOffer().computeDigest());
                plan.setField("member.0.statusHandle", makeOpaqueControlHandle());
                plan.setField("member.0.statusKey", generateSecureStatusKeyHex());
                selectionMessage.setDeploymentPlan(plan);
                pendingIt->second.deploymentPlan = plan;
            }
            bool collaborationAssignmentAttached = false;
            auto assignmentIt =
                pendingIt->second.collaborationAssignments.find(providerName.toUri());
            if (assignmentIt != pendingIt->second.collaborationAssignments.end()) {
                try {
                    assignmentIt->second = externalizeLargeCollaborationAssignment(
                        providerName, serviceName, requestId, assignmentIt->second);
                }
                catch (const std::exception& error) {
                    NDN_LOG_ERROR("Cannot publish collaboration Selection for "
                                  << requestId.toUri() << ": " << error.what());
                    rejectPreparation(error.what());
                    return;
                }
                providerEntry.assignmentPayload = assignmentIt->second;
                collaborationAssignmentAttached = true;
            }
            auto genericAssignmentIt =
                pendingIt->second.selectionAssignmentPayloads.find(providerName.toUri());
            if (genericAssignmentIt != pendingIt->second.selectionAssignmentPayloads.end()) {
                providerEntry.assignmentPayload =
                    mergeSelectionAssignmentPayloads(providerEntry.assignmentPayload,
                                                     genericAssignmentIt->second);
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_ASSIGNMENT_PAYLOAD_ATTACHED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << requestId.toUri()
                          << " providerName=" << providerName.toUri()
                          << " serviceName=" << serviceName.toUri()
                          << " payloadBytes=" << providerEntry.assignmentPayload.size());
            }
            if (!providerEntry.assignmentPayload.empty() &&
                !collaborationAssignmentAttached) {
                try {
                    providerEntry.assignmentPayload =
                        externalizeLargeCollaborationAssignment(
                            providerName, serviceName, requestId,
                            providerEntry.assignmentPayload);
                    if (genericAssignmentIt !=
                        pendingIt->second.selectionAssignmentPayloads.end()) {
                        genericAssignmentIt->second =
                            providerEntry.assignmentPayload;
                    }
                }
                catch (const std::exception& error) {
                    NDN_LOG_ERROR("Cannot publish structured Selection assignment for "
                                  << requestId.toUri() << ": " << error.what());
                    rejectPreparation(error.what());
                    return;
                }
            }
            if (!providerEntry.assignmentPayload.empty()) {
                NDN_LOG_INFO("NDNSF_SELECTION_ASSIGNMENT_ATTACHED requestId="
                             << requestId.toUri()
                             << " providerName=" << providerName.toUri()
                             << " serviceName=" << serviceName.toUri()
                             << " payloadBytes="
                             << providerEntry.assignmentPayload.size()
                             << " compact=0");
            }
        }
        if (pendingIt != m_pendingCalls.end() &&
            !pendingIt->second.selectionGatedInputKey.empty()) {
            const auto ackIt = std::find_if(
                pendingIt->second.requestAcks.begin(),
                pendingIt->second.requestAcks.end(),
                [&providerName] (const StoredAck& ack) {
                    return ack.providerName.equals(providerName);
                });
            if (ackIt == pendingIt->second.requestAcks.end() ||
                !ackIt->message.hasSelectionInputKeyOffer())
                throw std::runtime_error(
                    "SelectionGatedInputV1 selected ACK missing key offer");
            const auto& offer = ackIt->message.getSelectionInputKeyOffer();
            if (!offer.hasField("recipient") ||
                offer.getField("recipient") != providerName.toUri() ||
                !offer.hasField("recipientCertName") ||
                !offer.hasField("recipientPublicKey") ||
                !offer.hasField("recipientCertDigest"))
                throw std::runtime_error("invalid Selection input key offer binding");
            const auto wrapped = wrapSelectionGatedInputKey(
                pendingIt->second.selectionGatedInputKey,
                selectionGatedUnhex(offer.getField("recipientPublicKey")));
            SelectionInputKeyGrant grant;
            grant.setField("schemaVersion", "1");
            grant.setField("recipient", providerName.toUri());
            grant.setField("recipientCertName", offer.getField("recipientCertName"));
            grant.setField("recipientCertDigest", offer.getField("recipientCertDigest"));
            grant.setField("wrappedInputKey", selectionGatedHex(wrapped));
            grant.setField("encryptedInputDigest",
                           pendingIt->second.requestMessage.getEncryptedRequestInput().computeDigest());
            grant.setField("requestId", requestId.toUri());
            grant.setField("attempt", "1");
            selectionMessage.setSelectionInputKeyGrant(grant);
        }
        bool isStreamGrantRecipient = true;
        if (pendingIt != m_pendingCalls.end() &&
            pendingIt->second.isCollaboration) {
            isStreamGrantRecipient = false;
            for (const auto& participant :
                 pendingIt->second.collaborationCommittedParticipants) {
                if (!participant.provider.equals(providerName) ||
                    !participant.service.equals(serviceName)) {
                    continue;
                }
                const auto role = std::find_if(
                    pendingIt->second.collaborationPlan.roles.begin(),
                    pendingIt->second.collaborationPlan.roles.end(),
                    [&participant] (const CollaborationRoleSpec& candidate) {
                        return candidate.role == participant.role &&
                               candidate.service == participant.service;
                    });
                if (role != pendingIt->second.collaborationPlan.roles.end() &&
                    role->terminalResponseOwner) {
                    isStreamGrantRecipient = true;
                    break;
                }
            }
        }
        if (pendingIt != m_pendingCalls.end() &&
            pendingIt->second.streamOptions &&
            !pendingIt->second.streamEventKey.empty() &&
            isStreamGrantRecipient) {
            const auto ackIt = std::find_if(
                pendingIt->second.requestAcks.begin(),
                pendingIt->second.requestAcks.end(),
                [&providerName] (const StoredAck& ack) {
                    return ack.providerName.equals(providerName);
                });
            if (ackIt == pendingIt->second.requestAcks.end() ||
                !ackIt->message.hasSelectionInputKeyOffer()) {
                throw std::runtime_error(
                    "stream Selection requires a Provider key offer");
            }
            const auto& offer = ackIt->message.getSelectionInputKeyOffer();
            if (!offer.hasField("recipient") ||
                offer.getField("recipient") != providerName.toUri() ||
                !offer.hasField("recipientCertName") ||
                !offer.hasField("recipientPublicKey") ||
                !offer.hasField("recipientCertDigest") ||
                !offer.hasField("providerBootEpoch") ||
                offer.getField("providerBootEpoch").empty()) {
                throw std::runtime_error("invalid streamed key offer binding");
            }
            StreamBinding grantBinding;
            grantBinding.requestId = requestId;
            grantBinding.requester = identity;
            grantBinding.serviceName = serviceName;
            grantBinding.producer = providerName;
            grantBinding.producerBootId = offer.getField("providerBootEpoch");
            grantBinding.attemptEpoch =
                pendingIt->second.streamOptions->attemptEpoch;
            // The grant is itself part of the Selection message, so including
            // the final Selection digest here would be circular.  Bind all
            // pre-selection identity, token, policy, epoch, and deadline
            // fields; the Provider additionally checks the final accepted
            // Selection digest when it constructs the stream binding.
            grantBinding.planDigest.fill(0);
            grantBinding.generationId = pendingIt->second.streamOptions->generationId;
            grantBinding.streamEpoch = pendingIt->second.streamOptions->streamEpoch;
            grantBinding.eventKeyCommitment =
                pendingIt->second.streamOptions->eventKeyCommitment;
            grantBinding.userToken = ndn::Buffer(
                reinterpret_cast<const uint8_t*>(
                    pendingIt->second.requestMessage.getUserToken().data()),
                pendingIt->second.requestMessage.getUserToken().size());
            grantBinding.policyEpoch = selectionMessage.getPolicyEpoch();
            grantBinding.controllerVersion = selectionMessage.hasControllerVersion() ?
                std::optional<ControllerVersion>(selectionMessage.getControllerVersion()) :
                pendingIt->second.streamOptions->controllerVersion;
            grantBinding.deadlineEpochMs =
                pendingIt->second.streamOptions->deadlineEpochMs;
            const auto grantBindingDigest = computeStreamGrantBindingDigest(grantBinding);
            const auto wrapped = wrapSelectionGatedInputKey(
                pendingIt->second.streamEventKey,
                selectionGatedUnhex(offer.getField("recipientPublicKey")));
            HybridMessageEnvelope grant;
            // A stream grant binds the full SHA-256 pre-Selection digest.
            // V2 compact key IDs are only for ordinary message-key lookup.
            grant.setVersion(1);
            grant.setAlgorithm("RSA-OAEP");
            grant.setKeyId(selectionGatedHex(ndn::span<const uint8_t>(
                grantBindingDigest.data(), grantBindingDigest.size())));
            grant.setEpochId(std::to_string(
                pendingIt->second.streamOptions->streamEpoch));
            grant.setMessageType("STREAM-GRANT");
            grant.setWrappedMessageKey(wrapped);
            selectionMessage.setStreamEventKeyGrant(grant.WireEncode());
        }
        if (pendingIt != m_pendingCalls.end() &&
            pendingIt->second.isCollaboration) {
            // Each collaboration projection contains one local
            // Provider entry. Keep the complete role binding in
            // framework-owned shared metadata so every Provider can resolve
            // cross-Provider dependency names without inspecting opaque
            // application assignment bytes.
            std::map<std::string, std::string> roleProviders;
            for (const auto& [assignmentProvider, payload] :
                 pendingIt->second.collaborationAssignments) {
                for (const auto& item : decodeOpaqueAssignmentSet(payload)) {
                    CollaborationAssignmentEnvelope envelope;
                    try {
                        if (decodeCollaborationAssignmentEnvelope(item, envelope) &&
                            !envelope.role.empty()) {
                            roleProviders[envelope.role] = assignmentProvider;
                        }
                    }
                    catch (const std::exception&) {
                        // The Provider will reject an invalid assignment
                        // envelope during normal selection processing.
                    }
                }
            }
            if (!roleProviders.empty()) {
                std::string metadata;
                for (const auto& [role, assignmentProvider] : roleProviders) {
                    metadata += "roleProvider." + role + "=" +
                                assignmentProvider + ";";
                }
                selectionMessage.setAssignmentPayload(
                    ndn::Buffer(reinterpret_cast<const uint8_t*>(metadata.data()),
                                metadata.size()));
            }
        }
        selectionMessage.addProviderEntry(providerEntry);

        // Request-scoped confidentiality is established only after the
        // Provider has been selected.  The discovery Request was deliberately
        // published without its application payload; now create one fresh
        // invocation key bundle, publish an exact-name User-signed Data
        // packet, and deliver the bundle only to the selected Provider.
        if (pendingIt != m_pendingCalls.end() &&
            pendingIt->second.requestScopedConfidentiality) {
            if (!selectionMessage.hasControllerVersion() ||
                !selectionMessage.getControllerVersion().isValid()) {
                throw std::runtime_error(
                    "request-scoped Selection requires a current ControllerVersion");
            }
            const auto ackIt = std::find_if(
                pendingIt->second.requestAcks.begin(),
                pendingIt->second.requestAcks.end(),
                [&providerName] (const StoredAck& ack) {
                    return ack.providerName.equals(providerName) &&
                           ack.message.getStatus();
                });
            if (ackIt == pendingIt->second.requestAcks.end() ||
                !ackIt->message.hasSelectionInputKeyOffer()) {
                throw std::runtime_error(
                    "request-scoped Selection requires a successful Provider key offer");
            }
            const auto& offer = ackIt->message.getSelectionInputKeyOffer();
            prepareRequestScopedRecipient(selectionMessage, serviceName, requestId,
                                          providerName, pendingIt->second, offer);
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_TOKEN_STATE timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " pendingCallPresent=" << (pendingIt != m_pendingCalls.end())
                  << " providerTokenPresent=" << providerTokenPresent);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_ELIGIBILITY_CHECK timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " eligible=" << (pendingIt != m_pendingCalls.end() && providerTokenPresent)
                  << " reason=publish_entry"
                  << " pendingCallPresent=" << (pendingIt != m_pendingCalls.end())
                  << " providerTokenPresent=" << providerTokenPresent);
        if (pendingIt == m_pendingCalls.end() || !providerTokenPresent) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_REJECTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " reason="
                      << (pendingIt == m_pendingCalls.end() ?
                          "pending_missing" : "provider_token_missing"));
        }

        ndn::Name serviceSelectionName =
            makeServiceSelectionNameV2(identity, providerName,
                                       serviceName, requestId);
        ndn::Name serviceSelectionNameWithoutPrefix =
            makeServiceSelectionNameWithoutPrefixV2(providerName,
                                                    serviceName, requestId);
        const std::string selectionDigest = computeSelectionDigest(selectionMessage);

        if (pendingIt != m_pendingCalls.end() &&
            pendingIt->second.streamOptions &&
            m_streamConsumers.find(requestId) == m_streamConsumers.end()) {
            if (!pendingIt->second.isCollaboration) {
                if (!initializeStreamConsumer(providerName, serviceName, requestId,
                                               selectionDigest)) {
                    throw std::runtime_error(
                        "failed to initialize streamed event consumer");
                }
            }
            else {
                // Initialize before the terminal Selection is published when
                // possible.  Other Providers may be published later, so their
                // exact bindings are added after each Selection is committed.
                const CollaborationRoleSpec* terminalRole = nullptr;
                const SelectedParticipant* terminalParticipant = nullptr;
                for (const auto& participant :
                     pendingIt->second.collaborationCommittedParticipants) {
                    const auto roleIt = std::find_if(
                        pendingIt->second.collaborationPlan.roles.begin(),
                        pendingIt->second.collaborationPlan.roles.end(),
                        [&participant](const CollaborationRoleSpec& role) {
                            return role.role == participant.role &&
                                   role.service == participant.service;
                        });
                    if (roleIt == pendingIt->second.collaborationPlan.roles.end()) {
                        throw std::runtime_error(
                            "streamed collaboration participant role is not in plan");
                    }
                    if (roleIt->terminalResponseOwner) {
                        if (terminalRole != nullptr) {
                            throw std::runtime_error(
                                "streamed collaboration has multiple terminal roles");
                        }
                        terminalRole = &*roleIt;
                        terminalParticipant = &participant;
                    }
                }
                if (terminalRole != nullptr && terminalParticipant != nullptr &&
                    terminalParticipant->provider.equals(providerName)) {
                    std::vector<StreamProgressBinding> progressBindings;
                    for (const auto& participant :
                         pendingIt->second.collaborationCommittedParticipants) {
                        std::string participantDigest;
                        if (participant.provider.equals(providerName)) {
                            participantDigest = selectionDigest;
                        }
                        else {
                            const auto digestIt =
                                pendingIt->second.selectionDigestsByProvider.find(
                                    participant.provider.toUri());
                            if (digestIt !=
                                pendingIt->second.selectionDigestsByProvider.end()) {
                                participantDigest = digestIt->second;
                            }
                        }
                        if (participantDigest.empty()) {
                            continue;
                        }
                        progressBindings.push_back({
                            participant.provider.toUri(), participantDigest,
                            participantDigest + ":" + participant.role +
                              ":assembly-progress"});
                    }
                    if (!initializeStreamConsumer(
                            terminalParticipant->provider,
                            terminalParticipant->service,
                            requestId,
                            selectionDigest,
                            selectionDigest + ":" + terminalRole->role +
                              ":assembly-progress",
                            std::move(progressBindings))) {
                        throw std::runtime_error(
                            "failed to initialize streamed collaboration consumer");
                    }
                }
            }
        }

        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_PUBLISH_ATTEMPT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " messageName=" << serviceSelectionName.toUri());
        if (pendingIt == m_pendingCalls.end()) throw std::runtime_error("Selection lacks pending invocation");
        if (pendingIt->second.selectionRequiredProviders.empty()) {
            if (pendingIt->second.isCollaboration) {
                for (const auto& participant : pendingIt->second.collaborationCommittedParticipants)
                    addUniqueName(pendingIt->second.selectionRequiredProviders, participant.provider);
            }
            if (pendingIt->second.selectionRequiredProviders.empty())
                pendingIt->second.selectionRequiredProviders.push_back(providerName);
        }
        if (selectionMessage.WireEncode().size() > 16 * 1024 * 1024)
            throw std::runtime_error("prepared Selection exceeds 16MiB invocation bound");
        pendingIt->second.preparedSelectionsByProvider.emplace(providerName.toUri(), selectionMessage);
        pendingIt->second.selectionPreparationContextsByProvider.emplace(providerName.toUri(),
            selectionRetryContext(requestId, providerName));
        PublishMessage(serviceSelectionName, serviceSelectionNameWithoutPrefix, selectionMessage,
            [this, providerName, serviceName, requestId, selectionDigest, serviceSelectionName, onPublished]
            (bool accepted, const std::string& error) {
                auto pendingIt = m_pendingCalls.find(requestId);
                if (pendingIt == m_pendingCalls.end()) { if (onPublished) onPublished(false); return; }
                if (!accepted) {
                    NDN_LOG_ERROR("Selection publication failed provider=" << providerName << " reason=" << error);
                    SelectionExecutionStatus failed;
                    failed.providerName = providerName;
                    failed.serviceName = serviceName;
                    failed.requestId = requestId;
                    failed.selectionDigest = selectionDigest;
                    failed.state = SelectionExecutionState::Failed;
                    failed.message = "Selection publication failed: " + error;
                    failed.updatedAtUs = nowMicroseconds();
                    pendingIt->second.selectionStatusesByProvider[providerName.toUri()] = std::move(failed);
                    pendingIt->second.timedOut = true;
                    boost::asio::post(m_face.getIoContext(), [this, requestId] {
                        cancelStreamRequest(requestId);
                        finalizeTimedOutPendingCall(requestId);
                    });
                    if (onPublished) onPublished(false);
                    return;
                }
        if (pendingIt != m_pendingCalls.end()) {
            addUniqueName(pendingIt->second.selectionPublishedProviders, providerName);
            pendingIt->second.selectionDigestsByProvider[providerName.toUri()] =
                selectionDigest;
            SelectionExecutionStatus status;
            status.providerName = providerName;
            status.serviceName = serviceName;
            status.requestId = requestId;
            status.selectionDigest = selectionDigest;
            status.state = SelectionExecutionState::Unknown;
            status.message = "selection published; awaiting provider status";
            status.updatedAtUs = nowMicroseconds();
            pendingIt->second.selectionStatusesByProvider[providerName.toUri()] =
                status;
            if (pendingIt->second.isCollaboration) {
                if (const auto consumerIt = m_streamConsumers.find(requestId);
                    consumerIt != m_streamConsumers.end()) {
                    for (const auto& participant :
                         pendingIt->second.collaborationCommittedParticipants) {
                        if (participant.provider.equals(providerName)) {
                            consumerIt->second->addExpectedProgressBinding({
                                providerName.toUri(), selectionDigest,
                                selectionDigest + ":" + participant.role +
                                  ":assembly-progress"});
                        }
                    }
                }
            }
            if (pendingIt->second.trackSelectionStatus &&
                pendingIt->second.selectionStatusOptions.enabled) {
                scheduleInitialSelectionStatusQuery(requestId,
                                                     providerName,
                                                     selectionDigest);
            }
        }
        const bool allPublished = std::all_of(pendingIt->second.selectionRequiredProviders.begin(),
            pendingIt->second.selectionRequiredProviders.end(), [&](const ndn::Name& provider) {
                return containsName(pendingIt->second.selectionPublishedProviders, provider);
            });
        if (allPublished) {
            pendingIt->second.selectionPublishedAtUs = nowMicroseconds();
            logPhaseTiming("user", "selectionCommitted", requestId,
                {{"selectedCount", std::to_string(pendingIt->second.selectionRequiredProviders.size())}});
            updateRequestLifecycleState(requestId, RequestLifecycleState::SELECTION_PUBLISHED);
            if (const auto consumer = m_streamConsumers.find(requestId); consumer != m_streamConsumers.end())
                consumer->second->prefetchWindow();
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_PUBLISHED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " messageName=" << serviceSelectionName.toUri());
        NDN_LOG_DEBUG("[ServiceUser] selection PublishMessage returned timestampMs="
                  << nowMilliseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri());
                scheduleResponseAttemptTimeout(requestId, providerName);
                if (onPublished) onPublished(true);
            });
    }

    void ServiceUser::PublishCompactServiceSelectionMessageV2(
        const std::vector<StoredAck>& selectedAcks)
    {
        if (selectedAcks.empty()) {
            return;
        }
        if (selectedAcks.size() == 1) {
            const auto& ack = selectedAcks.front();
            PublishServiceSelectionMessageV2(ack.providerName,
                                             ack.serviceName,
                                             ack.requestId);
            return;
        }

        const ndn::Name requestId = selectedAcks.front().requestId;
        const ndn::Name serviceName = selectedAcks.front().serviceName;
        for (const auto& ack : selectedAcks) {
            if (!ack.requestId.equals(requestId) ||
                !ack.serviceName.equals(serviceName)) {
                for (const auto& fallbackAck : selectedAcks) {
                    PublishServiceSelectionMessageV2(fallbackAck.providerName,
                                                     fallbackAck.serviceName,
                                                     fallbackAck.requestId);
                }
                return;
            }
        }

        auto pendingIt = m_pendingCalls.find(requestId);
        if (pendingIt == m_pendingCalls.end()) {
            for (const auto& fallbackAck : selectedAcks) {
                PublishServiceSelectionMessageV2(fallbackAck.providerName,
                                                 fallbackAck.serviceName,
                                                 fallbackAck.requestId);
            }
            return;
        }

        ServiceSelectionMessage selectionMessage;
        selectionMessage.setRequestIDs({requestId.toUri()});
        selectionMessage.setPolicyEpoch(getCurrentPolicyEpoch(serviceName));
        if (const auto version = getControllerVersion(serviceName))
            selectionMessage.setControllerVersion(*version);
        if (pendingIt->second.requestMessage.hasDeploymentIntent()) {
            DeploymentPlan plan;
            plan.setField("requestId", requestId.toUri());
            plan.setField("attempt", "1");
            plan.setField("requesterIdentity", identity.toUri());
            plan.setField("intentDigest",
                          pendingIt->second.requestMessage.getDeploymentIntent().computeDigest());
            size_t memberIndex = 0;
            for (const auto& selectedAck : selectedAcks) {
                if (!selectedAck.message.hasProviderCapabilityOffer()) {
                    throw std::runtime_error(
                        "deployment Selection requires capability offers from every member");
                }
                const auto prefix = "member." + std::to_string(memberIndex++) + ".";
                plan.setField(prefix + "provider", selectedAck.providerName.toUri());
                plan.setField(prefix + "role", "member-" + std::to_string(memberIndex));
                plan.setField(prefix + "offerDigest",
                              selectedAck.message.getProviderCapabilityOffer().computeDigest());
                plan.setField(prefix + "statusHandle", makeOpaqueControlHandle());
                plan.setField(prefix + "statusKey", generateSecureStatusKeyHex());
            }
            plan.setField("memberCount", std::to_string(selectedAcks.size()));
            selectionMessage.setDeploymentPlan(plan);
            pendingIt->second.deploymentPlan = plan;
        }
        pendingIt->second.selectionRequiredProviders.clear();
        for (const auto& ack : selectedAcks)
            addUniqueName(pendingIt->second.selectionRequiredProviders, ack.providerName);
        publishSelectionFanout(selectedAcks, 0);
    }

    void ServiceUser::publishSelectionFanout(std::vector<StoredAck> selectedAcks, size_t index)
    {
        if (index >= selectedAcks.size()) return;
        const auto ack = selectedAcks[index];
        if (m_pendingCalls.find(ack.requestId) == m_pendingCalls.end()) return;
        try {
            PublishServiceSelectionMessageV2(ack.providerName, ack.serviceName, ack.requestId,
                [this, selectedAcks = std::move(selectedAcks), index](bool accepted) mutable {
                    if (accepted) publishSelectionFanout(std::move(selectedAcks), index + 1);
                });
        }
        catch (const std::exception& error) {
            NDN_LOG_ERROR("Selection fanout preparation failed provider=" << ack.providerName
                          << " reason=" << error.what());
            if (const auto pending = m_pendingCalls.find(ack.requestId); pending != m_pendingCalls.end()) {
                SelectionExecutionStatus failed;
                failed.providerName = ack.providerName;
                failed.serviceName = ack.serviceName;
                failed.requestId = ack.requestId;
                failed.state = SelectionExecutionState::Failed;
                failed.message = "Selection preparation failed: " + std::string(error.what());
                failed.updatedAtUs = nowMicroseconds();
                pending->second.selectionStatusesByProvider[ack.providerName.toUri()] = std::move(failed);
            }
            const ndn::Name failedRequest = ack.requestId;
            if (const auto pending = m_pendingCalls.find(failedRequest); pending != m_pendingCalls.end())
                pending->second.timedOut = true;
            boost::asio::post(m_face.getIoContext(), [this, failedRequest] {
                cancelStreamRequest(failedRequest);
                finalizeTimedOutPendingCall(failedRequest);
            });
        }
    }

    void ServiceUser::onMissingData(const std::vector<ndn::svs::MissingDataInfo>& infoVector)
    {
        // for (const auto& info : infoVector) {
        //     NDN_LOG_INFO("onMissingData from node " << info.nodeId
        //                 << " seq range [" << info.low << ", " << info.high << "]");
        // }
    }


    bool ServiceUser::replyFromIMS(const ndn::Interest &interest)
    {
        std::optional<ndn::Data> dataToSend;
        bool suppressFallback = false;
        std::shared_lock<std::shared_mutex> fence(m_protectedReuseMutex);
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            const auto isRetiredInterest = [&] {
                for (const auto& prefixUri : m_retiredProtectedPrefixes) {
                    const ndn::Name prefix(prefixUri);
                    if (prefix.isPrefixOf(interest.getName()) ||
                        interest.getName().isPrefixOf(prefix)) {
                        return true;
                    }
                }
                return false;
            };
            suppressFallback = isRetiredInterest();
            if (suppressFallback) {
                NDN_LOG_TRACE("Suppress stale protected IMS fallback: "
                              << interest.getName().toUri());
            }
            else if (auto data = m_IMS.find(interest)) {
                dataToSend.emplace(*data);
            }
        }
        if (suppressFallback) {
            return true;
        }
        if (dataToSend)
        {
            NDN_LOG_TRACE("Reply from IMS: " << interest.getName().toUri());
            m_face.put(*dataToSend);
        }else{
            size_t imsSize = 0;
            {
                std::lock_guard<std::mutex> lock(_cache_mutex);
                imsSize = m_IMS.size();
            }
            NDN_LOG_TRACE("Not Found In IMS: " << interest.getName().toUri()<<" SIZE: "<< imsSize);
        }
        return false;
    }

    bool ServiceUser::replyFromLargeDataFile(const ndn::Interest& interest)
    {
        std::shared_ptr<LargeDataFilePublication> publication;
        const auto& interestName = interest.getName();
        std::shared_lock<std::shared_mutex> fence(m_protectedReuseMutex);
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            for (const auto& item : m_largeDataFiles) {
                const auto& candidate = item.second;
                if (!candidate) {
                    continue;
                }
                if (candidate->manifestPacket && candidate->manifestPacket->getName() == interestName) {
                    publication = candidate;
                    break;
                }
                if (!candidate->baseName.isPrefixOf(interestName)) continue;
                // SegmentFetcher starts with the versioned base name and
                // CanBePrefix, then requests explicit /seg=N names.  Serve
                // segment zero for that discovery Interest so file-backed
                // publications work with the standard segmented-data API.
                if ((candidate->baseName.equals(interestName) &&
                     interest.getCanBePrefix()) ||
                    (interestName.size() == candidate->baseName.size() + 1 &&
                     interestName.get(-1).isSegment())) {
                    publication = candidate;
                    break;
                }
            }
        }
        if (publication && !publication->publicationIdentity.empty()) {
            bool current = true;
            // Do not acquire the Controller mutex while holding the cache
            // mutex.  The protected read-side fence prevents a status
            // retirement from linearizing between this check and m_face.put.
            std::lock_guard<std::mutex> versionLock(m_controllerVersionMutex);
            const auto statusIt = m_revocationStates.find(
                publication->serviceName);
            if (publication->controllerVersioned) {
                current = statusIt != m_revocationStates.end() &&
                    statusIt->second.hasCurrentStatus() &&
                    statusIt->second.currentVersion().compare(
                        publication->controllerVersion) == 0;
            }
            else {
                current = statusIt == m_revocationStates.end() ||
                    !statusIt->second.hasCurrentStatus();
            }
            if (!current) {
                return true;
            }
        }
        if (!publication) {
            for (const auto& prefixUri : m_retiredProtectedPrefixes) {
                const ndn::Name prefix(prefixUri);
                if (prefix.isPrefixOf(interestName) ||
                    interestName.isPrefixOf(prefix)) {
                    return true;
                }
            }
            return false;
        }

        if (publication->manifestPacket && publication->manifestPacket->getName() == interestName) {
            m_face.put(*publication->manifestPacket);
            return true;
        }
        const auto segment = interestName.equals(publication->baseName) ?
            std::uint64_t(0) : interestName.get(-1).toSegment();
        try {
            if (const auto cached = publication->findCached(segment)) {
                m_face.put(*cached);
                NDN_LOG_TRACE("Reply large-data segment from retransmission window: "
                               << cached->getName());
                return true;
            }
            if (publication->originalPackets || publication->originalPacketSource) {
                {
                    std::lock_guard<std::mutex> lock(publication->streamMutex);
                    ++publication->segmentReadCount;
                }
                const auto wire = publication->originalPacketSource ?
                    publication->rangeSource->readOriginalDataPacket(ndn::Name(publication->baseName).appendSegment(segment)) :
                    publication->originalPackets->read(segment);
                if (wire.empty() || wire.size() > ndn::MAX_NDN_PACKET_SIZE)
                    throw std::runtime_error("ORIGINAL_PACKET_SIZE_INVALID");
                auto data = std::make_shared<ndn::Data>(
                    ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size())));
                if (data->getName() != ndn::Name(publication->baseName).appendSegment(segment))
                    throw std::runtime_error("ORIGINAL_PACKET_NAME_MISMATCH");
                publication->cache(segment, data);
                m_face.put(*data);
                return true;
            }
            // Explicit historical range restoration; migration to persistent
            // original packet metadata is a separate pending T002 boundary.
            auto data = std::make_shared<ndn::Data>(
                ndn::Name(publication->baseName).appendSegment(segment));
            data->setFreshnessPeriod(publication->freshness);
            data->setFinalBlock(ndn::name::Component::fromSegment(
                publication->segmentCount - 1));
            data->setContent(publication->readSegment(segment));
            auto& signingKeyChain =
                m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain;
            signingKeyChain.sign(*data, m_signingInfo);
            if (data->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE) {
                throw ndn::Face::OversizedPacketError(
                    'D', data->getName(), data->wireEncode().size());
            }
            publication->cache(segment, data);
            m_face.put(*data);
            NDN_LOG_TRACE("Reply large-data segment from file: " << data->getName());
        }
        catch (const std::exception& error) {
            NDN_LOG_ERROR("Failed to serve large-data file segment name="
                          << interestName << " reason=" << error.what());
        }
        // The object is known to this user.  Do not fall through to IMS when
        // a file read or signing error occurs, which could otherwise expose a
        // stale packet under the same name.
        return true;
    }

    LargeDataServingMetrics
    ServiceUser::getLargeDataServingMetricsForTest() const
    {
        LargeDataServingMetrics metrics;
        std::lock_guard<std::mutex> lock(_cache_mutex);
        metrics.publicationCount = m_largeDataFiles.size();
        for (const auto& item : m_largeDataFiles) {
            const auto& publication = item.second;
            if (!publication) {
                continue;
            }
            std::lock_guard<std::mutex> publicationLock(publication->streamMutex);
            metrics.segmentReadCount += publication->segmentReadCount;
            metrics.retransmissionHitCount += publication->retransmissionHitCount;
            metrics.peakWindowSegments = std::max(
                metrics.peakWindowSegments, publication->peakWindowSegments);
        }
        return metrics;
    }

    void ServiceUser::onPrefixRegisterFailure(const ndn::Name &prefix, const std::string &reason)
    {
        // log error
        NDN_LOG_ERROR("Prefix registration failed for prefix " << prefix.toUri() << " reason: " << reason);
    }

    bool ServiceUser::handleProviderReadyInterest(const ndn::Interest& interest)
    {
        const auto parsed = parseProviderReadyName(interest.getName(), identity);
        if (!parsed || !parseControlRpcName(interest, identity, "PROVIDER-READY")) return false;
        if (m_originalControlAcks.count(interest.getName())) {
            try {
                ProviderReadyMessage ready;
                if (!ready.WireDecode(interest.getApplicationParameters()) ||
                    !ready.hasField("requestId") || !ready.hasField("providerIdentity") ||
                    !ready.hasField("deploymentPlanDigest") || !ready.hasField("selectionDigest")) return true;
                const auto pending = m_pendingCalls.find(ndn::Name(ready.getField("requestId")));
                if (pending == m_pendingCalls.end() || pending->second.timedOut || !pending->second.deploymentPlan ||
                    !authorizeControllerTransition(pending->second.serviceName, ProtectedTransition::SELECTION) ||
                    pending->second.deploymentPlan->computeDigest() != ready.getField("deploymentPlanDigest")) return true;
                const auto selected = std::find_if(pending->second.selectionDigestsByProvider.begin(),
                    pending->second.selectionDigestsByProvider.end(), [&](const auto& entry) {
                        return ndn::Name(entry.first) == ndn::Name(ready.getField("providerIdentity"));
                    });
                if (selected == pending->second.selectionDigestsByProvider.end() ||
                    selected->second != ready.getField("selectionDigest")) return true;
                // Only the previously authenticated signed parameter transcript
                // bypasses duplicate nonce rejection, under its still-live owner.
                serveOriginalControlAck(interest);
            }
            catch (const std::exception& error) { NDN_LOG_WARN("ProviderReady retry refused: " << error.what()); }
            return true;
        }
        nac_validator.validate(
            interest,
            [this, parsed](const ndn::Interest& validated) {
              try {
                ProviderReadyMessage ready;
                bool accepted = ready.WireDecode(validated.getApplicationParameters());
                if (!accepted || !ready.hasField("expiresAtUs")) return;
                const auto expiresAtUs = std::stoull(ready.getField("expiresAtUs"));
                const auto signer = validated.getSignatureInfo();
                if (!signer || !signer->hasKeyLocator() ||
                    signer->getKeyLocator().getType() != ndn::tlv::Name ||
                    !ready.hasField("providerIdentity") ||
                    ndn::security::extractIdentityFromCertName(signer->getKeyLocator().getName()) !=
                        ndn::Name(ready.getField("providerIdentity"))) return;
                if (!admitOriginalControlAck(validated.getName(), expiresAtUs)) return;
                std::string reason;
                PendingCall* pending = nullptr;
                ndn::Name requestId;
                if (!accepted || !ready.hasField("requestId")) {
                    accepted = false;
                    reason = "malformed ProviderReadyMessage";
                }
                else {
                    requestId = ndn::Name(ready.getField("requestId"));
                    auto it = m_pendingCalls.find(requestId);
                    if (it == m_pendingCalls.end() || !it->second.deploymentPlan) {
                        accepted = false;
                        reason = "unknown deployment request";
                    }
                    else {
                        pending = &it->second;
                    }
                }
                if (accepted && (pending->timedOut ||
                    !authorizeControllerTransition(pending->serviceName, ProtectedTransition::SELECTION))) {
                    accepted = false; reason = "ProviderReady owner authority is not current";
                }
                std::string memberKey;
                if (accepted) {
                    static const std::vector<std::string> required = {
                        "attempt", "selectionDigest", "deploymentPlanDigest",
                        "providerIdentity", "providerBootEpoch", "role",
                        "deploymentInstanceId", "operationId", "readySequence",
                        "issuedAtUs", "expiresAtUs"
                    };
                    for (const auto& field : required) {
                        if (!ready.hasField(field) || ready.getField(field).empty()) {
                            accepted = false;
                            reason = "ProviderReadyMessage missing " + field;
                            break;
                        }
                    }
                }
                if (accepted) {
                    const auto& plan = *pending->deploymentPlan;
                    const auto provider = ready.getField("providerIdentity");
                    const auto role = ready.getField("role");
                    const auto signatureInfo = validated.getSignatureInfo();
                    if (!signatureInfo || !signatureInfo->hasKeyLocator() ||
                        signatureInfo->getKeyLocator().getType() != ndn::tlv::Name ||
                        ndn::security::extractIdentityFromCertName(
                            signatureInfo->getKeyLocator().getName()) != ndn::Name(provider)) {
                        accepted = false;
                        reason = "ProviderReady signer identity mismatch";
                    }
                    bool exactMember = false;
                    bool exactHandle = false;
                    std::string declaredProvider;
                    for (size_t i = 0; i < DeploymentControlMessage::MAX_FIELDS; ++i) {
                        const auto prefix = "member." + std::to_string(i) + ".";
                        if (!plan.hasField(prefix + "provider")) continue;
                        if (ndn::Name(plan.getField(prefix + "provider")) == ndn::Name(provider) &&
                            plan.hasField(prefix + "role") &&
                            plan.getField(prefix + "role") == role) {
                            exactMember = true;
                            declaredProvider = plan.getField(prefix + "provider");
                            exactHandle = plan.hasField(prefix + "statusHandle") &&
                                plan.getField(prefix + "statusHandle") == parsed->controlHandle;
                            break;
                        }
                    }
                    const auto selectionIt = std::find_if(pending->selectionDigestsByProvider.begin(),
                        pending->selectionDigestsByProvider.end(), [&](const auto& entry) {
                            return ndn::Name(entry.first) == ndn::Name(provider);
                        });
                    if (!accepted) {
                        // Preserve the signer rejection above; never admit it
                        // through the exact-member branch.
                    }
                    else if (!exactMember || !exactHandle ||
                        ready.getField("deploymentPlanDigest") != plan.computeDigest() ||
                        selectionIt == pending->selectionDigestsByProvider.end() ||
                        ready.getField("selectionDigest") != selectionIt->second) {
                        accepted = false;
                        reason = "ProviderReadyMessage exact binding mismatch";
                    }
                    else {
                        if (serveOriginalControlAck(validated)) return;
                        memberKey = declaredProvider + "|" + role;
                        auto existing = pending->deploymentReadyByMember.find(memberKey);
                        if (existing != pending->deploymentReadyByMember.end() &&
                            existing->second.computeDigest() != ready.computeDigest()) {
                            accepted = false;
                            reason = "conflicting ProviderReadyMessage";
                        }
                        else {
                            pending->deploymentReadyByMember[memberKey] = ready;
                        }
                    }
                }

                ReadyAcknowledgement ack;
                ack.setField("readyMessageDigest", ready.computeDigest());
                ack.setField("requesterIdentity", identity.toUri());
                ack.setField("accepted", accepted ? "true" : "false");
                ack.setField("reason", reason);
                ack.setField("acknowledgementSequence", "1");
                ack.setField("issuedAtUs", std::to_string(nowMicroseconds()));
                ack.setField("expiresAtUs", std::to_string(nowMicroseconds() + 1000000));
                ndn::Data data(validated.getName());
                data.setFreshnessPeriod(ndn::time::milliseconds(250));
                data.setContent(ack.WireEncode());
                (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain)
                    .sign(data, m_signingInfo);
                publishOriginalControlAck(validated, data, expiresAtUs);
                if (accepted && pending != nullptr) {
                    maybeActivateReadyDeployment(requestId, *pending);
                }
              }
              catch (const std::exception& error) {
                NDN_LOG_WARN("ProviderReady processing failed: " << error.what());
              }
            },
            [](const ndn::Interest&, const ndn::security::ValidationError& error) {
                NDN_LOG_WARN("ProviderReady signature validation failed: " << error);
            });
        return true;
    }

    void ServiceUser::maybeActivateReadyDeployment(const ndn::Name& requestId,
                                                    PendingCall& pendingCall)
    {
        if (!pendingCall.deploymentPlan || pendingCall.deploymentActivationSent) return;
        const auto& plan = *pendingCall.deploymentPlan;
        std::vector<std::tuple<std::string, std::string, std::string>> members;
        for (size_t i = 0; i < DeploymentControlMessage::MAX_FIELDS; ++i) {
            const auto prefix = "member." + std::to_string(i) + ".";
            if (!plan.hasField(prefix + "provider")) continue;
            if (!plan.hasField(prefix + "role") || !plan.hasField(prefix + "statusHandle")) return;
            members.emplace_back(plan.getField(prefix + "provider"),
                                 plan.getField(prefix + "role"),
                                 plan.getField(prefix + "statusHandle"));
        }
        if (members.empty() || pendingCall.deploymentReadyByMember.size() != members.size()) return;
        DeploymentPlan readySummary;
        DeploymentPlan memberSummary;
        size_t index = 0;
        for (const auto& member : members) {
            const auto key = std::get<0>(member) + "|" + std::get<1>(member);
            auto readyIt = pendingCall.deploymentReadyByMember.find(key);
            if (readyIt == pendingCall.deploymentReadyByMember.end()) return;
            readySummary.setField("ready." + std::to_string(index),
                                  readyIt->second.computeDigest());
            memberSummary.setField("member." + std::to_string(index), key);
            ++index;
        }
        pendingCall.deploymentActivationSent = true;
        for (const auto& member : members) {
            const auto provider = ndn::Name(std::get<0>(member));
            const auto key = std::get<0>(member) + "|" + std::get<1>(member);
            const auto& ready = pendingCall.deploymentReadyByMember.at(key);
            ExecutionActivateMessage activation;
            activation.setField("requestId", requestId.toUri());
            activation.setField("attempt", plan.getField("attempt"));
            activation.setField("selectionDigest", ready.getField("selectionDigest"));
            activation.setField("deploymentPlanDigest", plan.computeDigest());
            activation.setField("readySetDigest", readySummary.computeDigest());
            activation.setField("memberSetDigest", memberSummary.computeDigest());
            activation.setField("requesterIdentity", identity.toUri());
            activation.setField("activationSequence", "1");
            activation.setField("issuedAtUs", std::to_string(nowMicroseconds()));
            activation.setField("expiresAtUs", std::to_string(nowMicroseconds() + 2000000));
            publishExecutionActivate(provider, std::get<2>(member), activation);
        }
    }

    bool ServiceUser::admitOriginalControlAck(const ndn::Name& name, uint64_t expiresAtUs)
    {
        const auto now = nowMicroseconds();
        if (expiresAtUs <= now) return false;
        checkNamingNameSize(name);
        for (auto it = m_originalControlAcks.begin(); it != m_originalControlAcks.end();) {
            if (it->second.expiresAtUs <= now) {
                m_originalControlAckBytes -= it->second.packet->wireEncode().size() + it->second.request.wireEncode().size();
                it = m_originalControlAcks.erase(it);
            }
            else ++it;
        }
        return m_originalControlAcks.count(name) != 0 ||
            (m_originalControlAcks.size() < 4096 &&
             m_originalControlAckBytes <= 64 * 1024 * 1024 - 2 * ndn::MAX_NDN_PACKET_SIZE);
    }

    bool ServiceUser::serveOriginalControlAck(const ndn::Interest& request)
    {
        const auto original = m_originalControlAcks.find(request.getName());
        if (original == m_originalControlAcks.end() || original->second.expiresAtUs <= nowMicroseconds() ||
            !request.isParametersDigestValid() ||
            !(original->second.request.getApplicationParameters() == request.getApplicationParameters()) ||
            original->second.request.getSignatureInfo() != request.getSignatureInfo() ||
            !(original->second.request.getSignatureValue() == request.getSignatureValue())) return false;
        try { m_face.put(*original->second.packet); }
        catch (const std::exception& error) {
            NDN_LOG_WARN("Accepted original control ACK delivery failed: " << error.what());
        }
        return true;
    }

    void ServiceUser::publishOriginalControlAck(const ndn::Interest& request, const ndn::Data& packet, uint64_t expiresAtUs)
    {
        if (request.getName() != packet.getName() || !request.isParametersDigestValid() ||
            !admitOriginalControlAck(packet.getName(), expiresAtUs) ||
            request.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE ||
            packet.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE ||
            !validator->isProducerSignerAuthorized(packet, identity))
            throw std::invalid_argument("control ACK ownership, expiry or capacity invalid");
        const auto existing = m_originalControlAcks.find(packet.getName());
        if (existing != m_originalControlAcks.end()) {
            if (!(existing->second.packet->wireEncode() == packet.wireEncode()))
                throw std::invalid_argument("immutable control ACK name conflict");
        }
        else {
            auto owned = std::make_shared<const ndn::Data>(packet);
            m_originalControlAcks.emplace(packet.getName(), OriginalControlAck{owned, request, expiresAtUs});
            m_originalControlAckBytes += owned->wireEncode().size() + request.wireEncode().size();
        }
        serveOriginalControlAck(request);
    }

    void ServiceUser::publishExecutionActivate(
        const ndn::Name& provider,
        const std::string& controlHandle,
        const ExecutionActivateMessage& activation,
        int attempt)
    {
        if (attempt > 2) {
            NDN_LOG_WARN("ExecutionActivate acknowledgement retry exhausted requestId="
                         << activation.getField("requestId")
                         << " provider=" << provider.toUri());
            return;
        }
        uint64_t expiresAtUs = 0;
        try { expiresAtUs = std::stoull(activation.getField("expiresAtUs")); }
        catch (...) { return; }
        if (expiresAtUs <= nowMicroseconds()) return;
        try {
        const auto parameters = activation.WireEncode();
        const auto logical = makeControlRpcName(provider, "EXECUTION-ACTIVATE", parameters, controlHandle);
        ndn::Interest command(logical);
        command.setMustBeFresh(true);
        command.setCanBePrefix(false);
        command.setInterestLifetime(ndn::time::milliseconds(500));
        const auto prepared = m_preparedControlInterests.find(logical);
        if (prepared != m_preparedControlInterests.end()) command = prepared->second;
        else {
            if (m_preparedControlInterests.size() >= 8192) {
                NDN_LOG_WARN("ExecutionActivate prepared control capacity exhausted"); return;
            }
            command.setApplicationParameters(parameters);
            auto& keyChain = m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain;
            if (!m_controlInterestSigner || m_controlInterestSigningKeyChain != &keyChain) {
                m_controlInterestSigner = std::make_unique<ndn::security::InterestSigner>(keyChain);
                m_controlInterestSigningKeyChain = &keyChain;
            }
            m_controlInterestSigner->makeSignedInterest(command, m_signingInfo);
            checkNamingNameSize(command.getName());
            if (command.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE) {
                NDN_LOG_WARN("ExecutionActivate prepared Interest exceeds wire limit"); return;
            }
            m_preparedControlInterests.emplace(logical, command);
            m_scheduler.schedule(ndn::time::microseconds(std::max<int64_t>(1, expiresAtUs - nowMicroseconds())),
              [this, logical] { m_preparedControlInterests.erase(logical); });
        }
        auto retry = [this, provider, controlHandle, activation, attempt] {
            m_scheduler.schedule(ndn::time::milliseconds(100 * (attempt + 1)),
                [this, provider, controlHandle, activation, attempt] {
                    publishExecutionActivate(provider, controlHandle, activation, attempt + 1);
                });
        };
        m_face.expressInterest(
            command,
            [this, provider, activation, retry](const ndn::Interest& query, const ndn::Data& data) {
                if (data.getName() != query.getName()) { retry(); return; }
                nac_validator.validate(
                    data,
                    [this, provider, activation, retry](const ndn::Data& validated) {
                        if (!validator->isProducerSignerAuthorized(validated, provider)) { retry(); return; }
                        ReadyAcknowledgement ack;
                        if (!ack.WireDecode(validated.getContent()) ||
                            !ack.hasField("accepted") ||
                            ack.getField("accepted") != "true" ||
                            !ack.hasField("activationDigest") ||
                            ack.getField("activationDigest") != activation.computeDigest()) {
                            retry();
                        }
                    },
                    [retry](const ndn::Data&, const ndn::security::ValidationError&) {
                        retry();
                    });
            },
            [retry](const ndn::Interest&, const ndn::lp::Nack&) { retry(); },
            [retry](const ndn::Interest&) { retry(); });
        }
        catch (const std::exception& error) {
            NDN_LOG_WARN("ExecutionActivate publication failed: " << error.what());
        }
    }

    void ServiceUser::onInterest(const ndn::InterestFilter &, const ndn::Interest &interest)
    {
        if (m_localSecurityProfile &&
            interest.getName() == m_localSecurityProfile->packet->getName()) {
            m_face.put(*m_localSecurityProfile->packet);
            return;
        }
        NDN_LOG_DEBUG("Received Interest: " << interest.getName().toUri());
        if (handleProviderReadyInterest(interest)) {
            return;
        }
        if (replyFromLargeDataFile(interest)) {
            return;
        }
        replyFromIMS(interest);

    }
    void ServiceUser::serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data> &contentData, ndn::nacabe::SPtrVector<ndn::Data> &ckData)
    {
        std::map<ndn::Name, std::shared_ptr<ndn::Data>> originals;
        for (const auto* packets : {&contentData, &ckData}) for (const auto& packet : *packets) {
            if (!packet || !identity.isPrefixOf(packet->getName()) ||
                !validator->isProducerSignerAuthorized(*packet, identity) ||
                packet->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
                throw std::invalid_argument("wrapped key original namespace/signature/budget rejected");
            const auto inserted = originals.emplace(packet->getName(), packet);
            if (!inserted.second && inserted.first->second->wireEncode() != packet->wireEncode())
                throw std::invalid_argument("wrapped key original batch conflict");
        }
        std::lock_guard<std::mutex> lock(_cache_mutex);
        for (const auto& [name, original] : originals) {
          // Check every digest variant through the existing IMS name index.
          for (auto entry = m_IMS.lowerBound(name);
               entry != m_IMS.end() && name.isPrefixOf(entry->getFullName()); ++entry) {
            const auto& existing = *entry;
            if (existing.getName() == name && existing.wireEncode() != original->wireEncode()) {
              throw std::invalid_argument("immutable wrapped key original conflict");
            }
          }
        }
        for (const auto& original : originals) {
            m_IMS.insert(*original.second);
        }
    }

    std::optional<RequestNameV2>
    ServiceUser::resolvePendingPublicationContext(const ndn::Name& messageName) const
    {
      const auto type = hybridMessageTypeForName(messageName, identity);
      if (type != "REQUEST" && type != "SELECTION") {
        return std::nullopt;
      }

      std::optional<RequestNameV2> result;
      const auto checkCandidate = [&](const ndn::Name& requestId) {
        const auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end() || (result && result->requestId == requestId)) {
          return;
        }
        const auto& service = pending->second.serviceName;
        bool matches = false;
        if (type == "REQUEST") {
          const auto parsed = parseRequestNameV2(messageName, identity, service);
          matches = parsed && parsed->requestId == requestId;
        }
        else {
          const auto selection = parseServiceSelectionNameV2(messageName, identity, service);
          matches = selection && selection->requestId == requestId;
          if (!matches) {
            const auto decision = parseServiceSelectionDecisionNameV2(messageName, identity, service);
            matches = decision && decision->requestId == requestId;
          }
        }
        if (!matches) {
          return;
        }
        if (result) {
          throw std::runtime_error("ambiguous hybrid publication context");
        }
        result = RequestNameV2{identity, service, requestId};
      };

      // Every valid context has a nonempty service and request ID. Consider
      // component boundaries only, then validate each pending hit with the
      // contextual parser. This preserves ambiguity checks without scanning
      // unrelated outstanding calls, including arbitrary multi-component IDs.
      const size_t firstRequestComponent = identity.size() + (type == "REQUEST" ? 3 : 4);
      for (size_t start = firstRequestComponent; start < messageName.size(); ++start) {
        checkCandidate(messageName.getSubName(start));
        if (type == "SELECTION" && start + 1 < messageName.size()) {
          // Only the existing decision parser may recognize the attempt suffix.
          checkCandidate(messageName.getSubName(start, messageName.size() - start - 1));
        }
      }
      return result;
    }

    void ServiceUser::publishHybridMessage(const ndn::Name& messageName,
                                           const ndn::Name&,
                                           AbstractMessage& message,
                                            std::function<void(bool, const std::string&)> onPublished)
    {
        const auto plaintextBlock = message.WireEncode();
        auto plaintext = ndn::Buffer(plaintextBlock.begin(), plaintextBlock.end());
        std::string expectedSelectionContext;
        std::optional<ControllerVersion> expectedControllerVersion;
        if (hybridMessageTypeForName(messageName, identity) == "SELECTION") {
            const auto context = resolvePendingPublicationContext(messageName);
            if (!context) {
                if (onPublished) onPublished(false, "Selection lacks pending preparation context");
                return;
            }
            const auto& pending = m_pendingCalls.at(context->requestId);
            const auto selection = parseServiceSelectionNameV2(messageName, identity, context->serviceName);
            const bool isSelection = selection && selection->requestId == context->requestId;
            const auto decision = isSelection ? std::nullopt :
                parseServiceSelectionDecisionNameV2(messageName, identity, context->serviceName);
            if (!isSelection && (!decision || decision->requestId != context->requestId)) {
                if (onPublished) onPublished(false, "Selection lacks pending preparation context");
                return;
            }
            const auto provider = isSelection ? selection->providerName : decision->providerName;
            const auto prepared = pending.selectionPreparationContextsByProvider.find(provider.toUri());
            expectedSelectionContext = isSelection && prepared != pending.selectionPreparationContextsByProvider.end()
                ? prepared->second : selectionRetryContext(context->requestId, provider);
            expectedControllerVersion = getControllerVersion(context->serviceName);
        }
        boost::asio::post(m_face.getIoContext(),
            [this, messageName, plaintext = std::move(plaintext), onPublished,
             expectedSelectionContext = std::move(expectedSelectionContext), expectedControllerVersion]() mutable {
                try { publishHybridEncodedMessage(messageName, std::move(plaintext), onPublished,
                        std::move(expectedSelectionContext), expectedControllerVersion); }
                catch (const std::exception& error) { if (onPublished) onPublished(false, error.what()); }
            });
    }

    void ServiceUser::publishHybridEncodedMessage(const ndn::Name& messageName,
                                                  ndn::Buffer plaintext,
                                                  std::function<void(bool, const std::string&)> onPublished,
                                                  std::string expectedSelectionContext,
                                                  std::optional<ControllerVersion> expectedControllerVersion)
    {
        ndn::Name senderPrefix = identity;
        const auto context = resolvePendingPublicationContext(messageName);
        if (!context) {
            NDN_LOG_ERROR("Hybrid publish lacks pending invocation context: " << messageName);
            if (onPublished) onPublished(false, "missing pending invocation context");
            return;
        }
        const auto& serviceName = context->serviceName;
        const auto& requestId = context->requestId;

        const auto messageType = hybridMessageTypeForName(messageName, identity);
        const auto accessAttribute = hybridAccessAttributeForName(messageName, serviceName, identity);
        ndn::Name selectionProvider;
        if (messageType == "SELECTION") {
            if (const auto parsed = parseServiceSelectionNameV2(messageName, identity, serviceName))
                selectionProvider = parsed->providerName;
            else if (const auto parsed = parseServiceSelectionDecisionNameV2(messageName, identity, serviceName))
                selectionProvider = parsed->providerName;
            else {
                if (onPublished) onPublished(false, "Selection lacks exact Provider context");
                return;
            }
        }
        auto key = m_hybridMessageCrypto.getOrCreateSendKey(
            serviceName, senderPrefix, accessAttribute, messageType, m_hybridCryptoCounters);

        const ndn::Buffer ad = hybridAssociatedData(messageName, messageType, requestId,
                                                    serviceName, senderPrefix,
                                                    key.keyId, key.epochId);
        HybridMessageEnvelope envelope;
        envelope.setKeyId(key.keyId);
        envelope.setEpochId(key.epochId);
        envelope.setMessageType(messageType);
        if (m_timelineTrace) {
            logTimelineTrace("user", "aes_gcm_encrypt_start", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"messageType", messageType}});
            logTimelineTrace("user", cryptoStageForName(messageName, identity) + "_crypto_start", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"messageName", messageName.toUri()},
                              {"mode", "hybrid"}});
        }

        // Publish the NAC-ABE wrapped MessageKey once under its deterministic
        // epoch name.  The first packet in a new epoch also carries that
        // wrapped key inline so a cold receiver can decrypt without racing the
        // named-key Interest.  Subsequent packets carry only compact key/epoch
        // identifiers and recover the key by name when needed.
        ndn::Buffer cachedWrappedKey;
        const bool wrappedKeyKnown =
            m_hybridMessageCrypto.getWrappedSendKey(key.keyId, cachedWrappedKey);
        if (wrappedKeyKnown) {
            std::vector<std::shared_ptr<ndn::Data>> originals, originalKeys;
            if (m_hybridMessageCrypto.getWrappedSendPackets(key.keyId, originals, originalKeys))
                serveDataWithIMS(originals, originalKeys);
        }
        if (!wrappedKeyKnown && m_hybridMessageCrypto.shouldAttachWrappedKey(key.keyId)) {
            if (m_timelineTrace) {
                logTimelineTrace("user", "wrapped_key_published", requestId,
                                 {{"value", "true"},
                                  {"source", "new"},
                                  {"serviceName", serviceName.toUri()},
                                  {"messageType", messageType}});
                logTimelineTrace("user", "hybrid_key_wrap_start", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"messageType", messageType}});
            }
            const auto wrapStartUs = timelineSteadyMicroseconds();
            ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
            std::tie(contentData, ckData) =
                (m_testNacProducer ? *m_testNacProducer : nacProducer).produce(key.keyName,
                                    std::vector<std::string>{accessAttribute},
                                    ndn::span<const uint8_t>(key.key.data(), key.key.size()),
                                    m_signingInfo);
            auto wrapped = mergeDataContents(contentData);
            if (wrapped.empty()) {
                NDN_LOG_ERROR("Hybrid MessageKey wrap produced empty content for "
                              << messageName.toUri());
                if (onPublished) onPublished(false, "hybrid MessageKey wrap produced empty content");
                return;
            }
            ndn::Buffer wrappedBuffer(wrapped.data(), wrapped.size());
            envelope.setWrappedMessageKey(wrappedBuffer);
            contentData = prepareHybridMessageKeyPackets(serviceName, senderPrefix,
                accessAttribute, key.epochId, ndn::span<const uint8_t>(wrapped.data(), wrapped.size()),
                (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain), m_signingInfo,
                contentData.front()->getFreshnessPeriod(), contentData.front()->getContentType());
            for (const auto& packet : contentData)
                if (!validator->isProducerSignerAuthorized(*packet, senderPrefix))
                    throw std::invalid_argument("wrapped key original signer is not authorized for Producer");
            for (const auto& packet : ckData)
                if (!packet || !identity.isPrefixOf(packet->getName()) ||
                    !validator->isProducerSignerAuthorized(*packet, identity) ||
                    packet->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
                    throw std::invalid_argument("original NAC CK signer/namespace/budget rejected");
            m_hybridMessageCrypto.cacheWrappedSendPackets(serviceName, key.keyId,
                ndn::Buffer(wrapped.data(), wrapped.size()), contentData, ckData);
            serveDataWithIMS(contentData, ckData);
            ++m_hybridCryptoCounters.nac_abe_key_wrap_count;
            const auto wrapEndUs = timelineSteadyMicroseconds();
            if (m_timelineTrace) {
                logTimelineTrace("user", "hybrid_key_wrap_done", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"messageType", messageType},
                                  {"duration_us", std::to_string(wrapEndUs >= wrapStartUs ?
                                                                 wrapEndUs - wrapStartUs : 0)}});
            }
        }
        else if (m_timelineTrace) {
            logTimelineTrace("user", "wrapped_key_published", requestId,
                             {{"value", "false"},
                              {"source", "epoch-cache"},
                              {"serviceName", serviceName.toUri()},
                              {"messageType", messageType}});
        }
        const bool inlineCrypto = plaintext.size() <= HybridMessageCrypto::INLINE_CRYPTO_MAX_BYTES;
        auto encryptAndPost = [this, messageName, requestId, serviceName, messageType, onPublished,
                               selectionProvider, expectedSelectionContext, expectedControllerVersion,
                               keyId = key.keyId, epochId = key.epochId,
                               keyBytes = key.key, ad = std::move(ad),
                               plaintext = std::move(plaintext),
                               envelope = std::move(envelope)]() mutable {
            const auto aesStartUs = timelineSteadyMicroseconds();
            ndn::Buffer buffer;
            size_t ciphertextBytes = 0;
            bool wrappedKeyAttached = envelope.hasWrappedMessageKey();
            std::string error;
            try {
                auto encrypted = hybridAesGcmEncrypt(
                    keyBytes,
                    ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                    ndn::span<const uint8_t>(ad.data(), ad.size()));
                envelope.setNonce(encrypted.nonce);
                envelope.setCipherText(encrypted.ciphertext);
                envelope.setAuthTag(encrypted.tag);
                ciphertextBytes = encrypted.ciphertext.size();
                auto envelopeBlock = envelope.WireEncode();
                buffer = ndn::Buffer(envelopeBlock.begin(), envelopeBlock.end());
            }
            catch (const std::exception& e) {
                error = e.what();
            }
            const auto aesEndUs = timelineSteadyMicroseconds();
            auto publish = std::make_shared<std::function<void()>>();
            std::weak_ptr<std::function<void()>> weakPublish = publish;
            *publish =
                [this, messageName, requestId, serviceName, messageType, onPublished,
                 selectionProvider, expectedSelectionContext, expectedControllerVersion,
                 keyId, epochId, aesStartUs, aesEndUs, wrappedKeyAttached,
                 ciphertextBytes, error = std::move(error),
                 buffer = std::move(buffer), weakPublish, cryptoReported = false]() mutable {
                if (messageType == "REQUEST" || messageType == "SELECTION") {
                    const auto owner = m_pendingCalls.find(requestId);
                    if (owner == m_pendingCalls.end() || owner->second.timedOut ||
                        owner->second.serviceName != serviceName) {
                        if (onPublished) onPublished(false, "invocation owner is no longer active");
                        return;
                    }
                }
                if (!error.empty()) {
                    NDN_LOG_ERROR("[NDNSF_HYBRID] role=user event=HYBRID_PUBLISH_FAILED"
                                  << " messageName=" << messageName.toUri()
                                  << " reason=" << error);
                    if (onPublished) onPublished(false, error);
                    return;
                }
                if (!cryptoReported) {
                    ++m_hybridCryptoCounters.symmetric_encrypt_count;
                    if (m_useTokens) {
                        if (messageType == "REQUEST" || messageType == "RESPONSE")
                            ++m_hybridCryptoCounters.user_token_symmetric_encrypt_count;
                        if (messageType == "ACK" || messageType == "SELECTION")
                            ++m_hybridCryptoCounters.provider_token_symmetric_encrypt_count;
                    }
                    cryptoReported = true;
                }
                // Synchronous SVS publication advances the async commit cursor.
                // Let earlier reserved publications finish before committing a
                // SELECTION. Poll on the Face so its async commits can run; the
                // original invocation timeout/cancellation is checked above on
                // every continuation. The weak self-reference owns no cycle.
                const auto preparation = m_svsps ? m_svsps->getPublicationPreparationStats()
                    : ndn::svs::SVSPubSub::PublicationPreparationStats{};
                if (messageType == "SELECTION" && (preparation.failed || preparation.cancelled)) {
                    if (onPublished) onPublished(false, "earlier SVS publication failed or was cancelled");
                    return;
                }
                if (messageType == "SELECTION" && preparation.outstanding != 0) {
                    if (const auto retry = weakPublish.lock()) {
                        m_scheduler.schedule(ndn::time::milliseconds(1),
                            [retry] { (*retry)(); });
                    }
                    return;
                }
                if (messageType == "SELECTION") {
                    // Check the original prepared context once when ready,
                    // rather than rehash assignments on each waiting tick.
                    std::string rejection;
                    try {
                        if (!authorizeControllerTransition(serviceName, ProtectedTransition::SELECTION) ||
                            getControllerVersion(serviceName) != expectedControllerVersion ||
                            selectionRetryContext(requestId, selectionProvider) != expectedSelectionContext) {
                            rejection = "Selection authorization or preparation context changed";
                        }
                    }
                    catch (const std::exception& failure) {
                        rejection = failure.what();
                    }
                    if (!rejection.empty()) {
                        if (onPublished) onPublished(false, rejection);
                        return;
                    }
                }
                if (m_timelineTrace) {
                    logTimelineTrace("user", "aes_gcm_encrypt_done", requestId,
                                     {{"serviceName", serviceName.toUri()},
                                      {"messageType", messageType},
                                      {"duration_us", std::to_string(aesEndUs >= aesStartUs ?
                                                                     aesEndUs - aesStartUs : 0)}});
                    logTimelineTrace("user", cryptoStageForName(messageName, identity) + "_crypto_done",
                                     requestId,
                                     {{"serviceName", serviceName.toUri()},
                                      {"messageName", messageName.toUri()},
                                      {"mode", "hybrid"}});
                }
                const auto queuedAtUs = nowMicroseconds();
                NDN_LOG_DEBUG("[NDNSF_HYBRID] role=user event=HYBRID_PUBLISH"
                              << " messageName=" << messageName.toUri()
                              << " messageType=" << messageType
                              << " keyId=" << keyId
                              << " epochId=" << epochId
                              << " wrappedKeyAttached=" << wrappedKeyAttached
                              << " ciphertextBytes=" << ciphertextBytes);
                ndn::Block contentBlock(buffer);
                const auto beginUs = nowMicroseconds();
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SVS_PUBLISH_BEGIN timestamp_us="
                          << beginUs
                          << " messageName=" << messageName.toUri()
                          << " contentBytes=" << contentBlock.value_size()
                          << " eventLoopLagUs=" << (beginUs >= queuedAtUs ? beginUs - queuedAtUs : 0)
                          << " mode=hybrid-message-crypto");
                logControlTiming("user", "SVS_PUBLISH_BEGIN", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"messageType", messageType},
                                  {"messageName", messageName.toUri()},
                                  {"contentBytes", std::to_string(contentBlock.value_size())},
                                  {"eventLoopLagUs", std::to_string(beginUs >= queuedAtUs ?
                                                                    beginUs - queuedAtUs : 0)},
                                  {"mode", "hybrid-message-crypto"}});
                if (m_timelineTrace) {
                    const auto& rid = requestId;
                    const auto& svc = serviceName;
                    if (!rid.empty()) {
                        logTimelineTrace("user", cryptoStageForName(messageName, identity) + "_publish_start",
                                         rid,
                                         {{"serviceName", svc.toUri()},
                                          {"messageName", messageName.toUri()},
                                          {"mode", "hybrid"}});
                    }
                }
                ndn::svs::SeqNo publishedSeqNo = 0;
                try {
                    if (!m_svsps) throw std::runtime_error("SVS publisher unavailable");
                    if (messageType == "SELECTION") {
                        // SVS measures both signed envelopes and retains its
                        // existing transactional segmented publication path.
                        publishedSeqNo = m_svsps->publish(messageName,
                            ndn::span<const uint8_t>(buffer.data(), buffer.size()),
                            ndn::Name(), ndn::time::milliseconds(10000));
                    }
                    else publishedSeqNo = publishSvs(m_svsps, messageName, contentBlock);
                }
                catch (const std::exception& error) {
                    if (onPublished) onPublished(false, error.what());
                    return;
                }
                std::vector<ndn::Data> originals;
                if (messageType == "SELECTION") {
                    const auto pending = m_pendingCalls.find(requestId);
                    const auto selected = parseServiceSelectionNameV2(messageName, identity, serviceName);
                    if (pending != m_pendingCalls.end() && selected) {
                        auto& sync = m_svsps->getSVSync();
                        const auto outerName = sync.getDataName(sync.getCore().getNodeId(), publishedSeqNo);
                        pending->second.selectionOuterNamesByProvider[selected->providerName.toUri()] = outerName;
                        try {
                            originals = copyOriginalSvsPublication(*m_svsps, outerName, messageName);
                            pending->second.selectionOriginalPacketsByProvider[selected->providerName.toUri()] = originals;
                        }
                        catch (const std::exception& error) {
                            NDN_LOG_WARN("Selection accepted; original cache recovery pending name=" << messageName
                                         << " reason=" << error.what());
                        }
                    }
                }
                // Namespace authorization was emitted. Later serving errors
                // must not report that publication as rolled back.
                if (onPublished) onPublished(true, "");
                try {
                    for (const auto& original : originals) {
                        const auto owned = std::make_shared<ndn::Data>(original);
                        m_IMS.insert(*owned);
                        if (original.getName() == messageName && isTruthyEnv("NDNSF_SELECTION_TARGETED_PREFETCH"))
                            m_face.put(*owned);
                    }
                }
                catch (const std::exception& error) {
                    NDN_LOG_WARN("Selection accepted; original direct serving failed name=" << messageName
                                 << " reason=" << error.what());
                }
                logControlTiming("user", "SVS_PUBLISH_DONE", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"messageType", messageType},
                                  {"messageName", messageName.toUri()},
                                  {"contentBytes", std::to_string(contentBlock.value_size())},
                                  {"mode", "hybrid-message-crypto"}});
                if (m_timelineTrace) {
                    const auto& rid = requestId;
                    const auto& svc = serviceName;
                    if (!rid.empty()) {
                        logTimelineTrace("user", cryptoStageForName(messageName, identity) + "_publish_done",
                                         rid,
                                         {{"serviceName", svc.toUri()},
                                          {"messageName", messageName.toUri()},
                                          {"mode", "hybrid"}});
                    }
                }
            };
            boost::asio::post(m_face.getIoContext(), [publish] { (*publish)(); });
        };
        if (inlineCrypto || m_handlerPool.getThreadCount() == 0 ||
            !m_handlerPool.post(encryptAndPost)) {
            encryptAndPost();
        }
    }

    bool ServiceUser::decryptHybridMessage(const ndn::Name& messageName,
                                           const ndn::Block& envelopeBlock,
                                               const ndn::Name& expectedActor,
                                               const ndn::Name& expectedService,
                                               const ndn::Name& expectedRequestId,
                                           std::function<void(const ndn::Buffer&)> onSuccess,
                                           std::function<void(const std::string&)> onError)
    {
        const auto decryptEntryUs = timelineSteadyMicroseconds();
        HybridMessageEnvelope envelope;
        if (!envelope.WireDecode(envelopeBlock)) {
            return false;
        }

        const auto messageType = hybridMessageTypeForName(messageName, expectedActor);
        if (messageType == "UNKNOWN" || envelope.getMessageType() != messageType) {
            if (onError) onError("hybrid message category differs from invocation context");
            return true;
        }
        const ndn::Name serviceName = expectedService;
        const ndn::Name requestId = expectedRequestId;
        const ndn::Name senderPrefix = expectedActor;
        const ndn::Name logicalMessageName = messageName;
        const auto accessAttribute = hybridAccessAttributeForName(logicalMessageName, serviceName, senderPrefix);
        const auto keyDataName = makeHybridMessageKeyDataName(
            serviceName, senderPrefix, accessAttribute, envelope.getEpochId());

        auto finish = [this, envelope, logicalMessageName, serviceName, requestId,
                       senderPrefix, decryptEntryUs, onSuccess = std::move(onSuccess),
                       onError](const ndn::Buffer& key) mutable {
            const auto keyReadyUs = timelineSteadyMicroseconds();
            const auto ad = hybridAssociatedData(logicalMessageName, envelope.getMessageType(),
                                                requestId, serviceName, senderPrefix,
                                                envelope.getKeyId(), envelope.getEpochId());
            auto decryptAndPost = [this, key, envelope, ad, requestId, keyReadyUs, decryptEntryUs,
                                   onSuccess = std::move(onSuccess),
                                   onError = std::move(onError)]() mutable {
                const auto aesStartUs = timelineSteadyMicroseconds();
                ndn::Buffer plaintext;
                const bool ok = hybridAesGcmDecrypt(
                    key, envelope, ndn::span<const uint8_t>(ad.data(), ad.size()), plaintext);
                const auto aesDoneUs = timelineSteadyMicroseconds();
                logHybridCryptoTiming("user", "hybrid_decrypt_aes_done", requestId,
                                      {{"messageType", envelope.getMessageType()},
                                       {"aesUs", std::to_string(aesDoneUs - aesStartUs)},
                                       {"entryToKeyReadyUs", std::to_string(keyReadyUs - decryptEntryUs)},
                                       {"keyReadyToAesStartUs", std::to_string(aesStartUs - keyReadyUs)},
                                       {"cipherBytes", std::to_string(envelope.getCipherText().size())},
                                       {"ok", ok ? "true" : "false"}});
                boost::asio::post(m_face.getIoContext(),
                    [this, ok, envelope, plaintext = std::move(plaintext), requestId,
                     aesDoneUs,
                     onSuccess = std::move(onSuccess),
                     onError = std::move(onError)]() mutable {
                    const auto callbackUs = timelineSteadyMicroseconds();
                    logHybridCryptoTiming("user", "hybrid_decrypt_callback_dispatch", requestId,
                                          {{"messageType", envelope.getMessageType()},
                                           {"aesDoneToCallbackUs", std::to_string(callbackUs - aesDoneUs)},
                                           {"ok", ok ? "true" : "false"}});
                    if (!ok) {
                        ++m_hybridCryptoCounters.auth_decrypt_failure_count;
                        if (onError) {
                            onError("hybrid AES-GCM authentication failed");
                        }
                        return;
                    }
                    ++m_hybridCryptoCounters.symmetric_decrypt_count;
                    if (m_useTokens) {
                        if (envelope.getMessageType() == "ACK") {
                            ++m_hybridCryptoCounters.provider_token_symmetric_decrypt_count;
                            ++m_hybridCryptoCounters.user_token_symmetric_decrypt_count;
                        }
                        if (envelope.getMessageType() == "RESPONSE") {
                            ++m_hybridCryptoCounters.user_token_symmetric_decrypt_count;
                        }
                    }
                    if (onSuccess) {
                        onSuccess(plaintext);
                    }
                });
            };
            BoundedWorkerPool& decryptPool =
                envelope.getMessageType() == "ACK" ? m_ackProcessingPool : m_handlerPool;
            if (envelope.getCipherText().size() <= HybridMessageCrypto::INLINE_CRYPTO_MAX_BYTES ||
                decryptPool.getThreadCount() == 0 ||
                !decryptPool.post(decryptAndPost)) {
                decryptAndPost();
            }
        };

        ndn::Buffer key;
        if (m_hybridMessageCrypto.findReceiveKey(envelope.getKeyId(), key,
                                                 m_hybridCryptoCounters)) {
            logHybridCryptoTiming("user", "hybrid_decrypt_key_cache", requestId,
                                  {{"messageType", envelope.getMessageType()},
                                   {"hit", "true"},
                                   {"entryToCacheLookupUs",
                                    std::to_string(timelineSteadyMicroseconds() - decryptEntryUs)}});
            finish(key);
            return true;
        }
        logHybridCryptoTiming("user", "hybrid_decrypt_key_cache", requestId,
                              {{"messageType", envelope.getMessageType()},
                               {"hit", "false"},
                               {"wrappedKeyAttached",
                                envelope.hasWrappedMessageKey() ? "true" : "false"},
                               {"entryToCacheLookupUs",
                                std::to_string(timelineSteadyMicroseconds() - decryptEntryUs)}});
        ++m_hybridCryptoCounters.nac_abe_key_unwrap_count;
        const auto unwrapStartUs = timelineSteadyMicroseconds();
        logHybridCryptoTiming("user", "hybrid_decrypt_key_unwrap_start", requestId,
                              {{"messageType", envelope.getMessageType()},
                               {"source", envelope.hasWrappedMessageKey() ?
                                          "inline" : "named-fetch"},
                               {"keyName", keyDataName.toUri()}});
        try {
            auto onKey = [this, serviceName, envelope, finish = std::move(finish), requestId,
                          unwrapStartUs, keyDataName](const ndn::Buffer& unwrappedKey) mutable {
                                    logHybridCryptoTiming("user", "hybrid_decrypt_key_unwrap_done", requestId,
                                                          {{"messageType", envelope.getMessageType()},
                                                           {"source", envelope.hasWrappedMessageKey() ?
                                                                      "inline" : "named-fetch"},
                                                           {"keyName", keyDataName.toUri()},
                                                           {"unwrapUs", std::to_string(timelineSteadyMicroseconds() - unwrapStartUs)},
                                                           {"keyBytes", std::to_string(unwrappedKey.size())}});
                                    m_hybridMessageCrypto.cacheReceiveKey(serviceName,
                                                                          envelope.getKeyId(),
                                                                          envelope.getEpochId(),
                                                                          unwrappedKey);
                                    finish(unwrappedKey);
                                };
            // Both unwrap failure and the success/AES path need the callback;
            // retain the outer copy for synchronous consume exceptions too.
            auto onKeyError = [onError, keyDataName](const std::string& error) {
                                    if (onError) {
                                        onError("hybrid MessageKey " +
                                                std::string(keyDataName.empty() ? "unwrap" :
                                                            "fetch/unwrap") +
                                                " failed: " + error);
                                    }
                                };
            if (envelope.hasWrappedMessageKey()) {
                activeNacConsumer().consume(keyDataName,
                                    makeNacInlineContentBlock(envelope.getWrappedMessageKey()),
                                    std::move(onKey), std::move(onKeyError));
            }
            else {
                fetchHybridMessageKeyWrapper(m_face, nac_validator, keyDataName,
                    [dataValidator = validator, producer = senderPrefix](const ndn::Data& packet) {
                        return dataValidator->isProducerSignerAuthorized(packet, producer);
                    },
                    [this, onKey = std::move(onKey), onKeyError](const ndn::Name& object,
                                                             const ndn::Block& cipher) mutable {
                        try { activeNacConsumer().consume(object, cipher, std::move(onKey), onKeyError); }
                        catch (const std::exception& e) { onKeyError(e.what()); }
                    }, onKeyError);
            }
        }
        catch (const std::exception& e) {
            if (onError) {
                onError("hybrid MessageKey unwrap failed: " + std::string(e.what()));
            }
        }
        return true;
    }

    void ServiceUser::PublishMessage(const ndn::Name &messageName, const ndn::Name &messageNameWithoutPrefix,AbstractMessage &message,
                                      std::function<void(bool, const std::string&)> onPublished)
    {
        // log message
        NDN_LOG_DEBUG("PublishMessage: " << messageName.toUri());
        if (m_svsps == nullptr) {
            if (m_localPublicationHandler) {
                const auto wireBlock = message.WireEncode();
                const ndn::Buffer wire(wireBlock.data(), wireBlock.size());
                m_localPublicationHandler(messageName, wire);
                if (onPublished) onPublished(true, "");
            }
            else if (onPublished) onPublished(false, "SVS publisher unavailable");
            NDN_LOG_DEBUG("PublishMessage skipped because SVS publisher is not initialized");
            return;
        }

        publishHybridMessage(messageName, messageNameWithoutPrefix, message, std::move(onPublished));
        return;
    }

    void ServiceUser::registerNDNSFMessages()
    {

        // log register
        NDN_LOG_INFO("Register NDNSF Messages in ndn-svs");

        // V2 ACK/RESPONSE subscriptions match every provider and parse counted
        // requester/service names from the message body.
        std::string regex_str = "^(<>*)" + NameToRegexString(ndn::Name("/NDNSF/ACK")) + "(<>*)$";
        NDN_LOG_INFO(regex_str);
        // Keep the validated Data packet for ACK provenance.  The ACK-aware
        // V3 planner must bind the inner ProviderOfferV3 to the signer and
        // wire digest produced by the Trust-Schema-validated packet; a
        // payload-only subscription cannot provide that evidence.
        m_svsps->subscribeWithRegex(ndn::Regex(regex_str),
                                    std::bind(&ServiceUser::OnRequestAck, this, _1),
                                    true, true);
        std::string regex_str2 = "^(<>*)" + NameToRegexString(ndn::Name("/NDNSF/RESPONSE")) + "(<>*)$";
        NDN_LOG_INFO(regex_str2);
        m_svsps->subscribeWithRegex(ndn::Regex(regex_str2),
                                    std::bind(&ServiceUser::OnResponse, this, _1),
                                    true, false);
        const std::string eventRegex = "^(<>*)" + NameToRegexString(ndn::Name("/NDNSF/event")) + "(<>*)$";
        m_svsps->subscribeWithRegex(ndn::Regex(eventRegex),
                                    std::bind(&ServiceUser::onStreamEvent, this, _1),
                                    true, true);
        const std::string collaborationRegex =
            "^(<>*)<NDNSF><COLLAB>(<>*)$";
        m_svsps->subscribeWithRegex(
            ndn::Regex(collaborationRegex),
            std::bind(&ServiceUser::OnCollaborationData, this, _1),
            true, true);

    }

    void ServiceUser::OnCollaborationData(
        const ndn::svs::SVSPubSub::SubscriptionData& subscription)
    {
        const bool collaborationAuthTrace =
            isTruthyEnv("NDNSF_COLLAB_AUTH_TRACE");
        if (collaborationAuthTrace) {
            NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_seen"
                         << " user=" << identity.toUri()
                         << " dataName=" << subscription.name.toUri()
                         << " producer=" << subscription.producerPrefix.toUri());
        }
        if (!isFresh(subscription)) {
            if (collaborationAuthTrace) {
                NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                             << " reason=stale"
                             << " dataName=" << subscription.name.toUri());
            }
            return;
        }

        const auto actor = parseSvsProducerActor(subscription.producerPrefix, "provider");
        if (!actor || !subscription.packet || !validator ||
            subscription.packet->getName() != subscription.name ||
            !validator->isProducerSignerAuthorized(*subscription.packet, *actor)) return;
        const auto parsed = parseCollaborationDataName(subscription.name, *actor);
        if (!parsed || !parsed->requesterName.equals(identity) ||
            !subscription.packet ||
            subscription.packet->getName() != subscription.name) {
            if (collaborationAuthTrace) {
                NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                             << " reason=name_binding"
                             << " dataName=" << subscription.name.toUri());
            }
            return;
        }

        const auto signatureType = subscription.packet->getSignatureType();
        if ((signatureType != ndn::tlv::SignatureSha256WithRsa &&
             signatureType != ndn::tlv::SignatureSha256WithEcdsa) ||
            !subscription.packet->getSignatureInfo().hasKeyLocator() ||
            subscription.packet->getSignatureInfo().getKeyLocator().getType() !=
                ndn::tlv::Name) {
            if (collaborationAuthTrace) {
                NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                             << " reason=signature_shape"
                             << " dataName=" << subscription.name.toUri());
            }
            return;
        }
        try {
            if (!isSignedByIdentity(*subscription.packet,
                                    parsed->producerName)) {
                if (collaborationAuthTrace) {
                    NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                                 << " reason=signature_identity"
                                 << " dataName=" << subscription.name.toUri());
                }
                return;
            }
        }
        catch (const std::exception&) {
            if (collaborationAuthTrace) {
                NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                             << " reason=signature_exception"
                             << " dataName=" << subscription.name.toUri());
            }
            return;
        }

        CollaborationDataMessage message;
        try {
            ndn::Block block(subscription.data);
            if (!message.WireDecode(block)) {
                return;
            }
        }
        catch (const std::exception&) {
            if (collaborationAuthTrace) {
                NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                             << " reason=message_decode"
                             << " dataName=" << subscription.name.toUri());
            }
            return;
        }

        if (message.getKeyScope() != parsed->keyScope ||
            message.getTopic() != parsed->topic ||
            message.getSequence() != parsed->sequence ||
            message.getProducerRole().empty()) {
            if (collaborationAuthTrace) {
                NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                             << " reason=message_binding"
                             << " dataName=" << subscription.name.toUri());
            }
            return;
        }

        ndn::Buffer scopeKey;
        {
            std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
            const auto requestIt =
                m_userCollaborationScopeKeys.find(parsed->requestId);
            if (requestIt == m_userCollaborationScopeKeys.end()) {
                if (collaborationAuthTrace) {
                    NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                                 << " reason=scope_key_request_missing"
                                 << " dataName=" << subscription.name.toUri());
                }
                return;
            }
            const auto keyIt = requestIt->second.find(parsed->keyScope);
            if (keyIt == requestIt->second.end() ||
                keyIt->second.size() !=
                    HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                if (collaborationAuthTrace) {
                    NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                                 << " reason=scope_key_missing"
                                 << " dataName=" << subscription.name.toUri()
                                 << " keyScope=" << parsed->keyScope);
                }
                return;
            }
            scopeKey = keyIt->second;
        }

        HybridMessageEnvelope envelope;
        try {
            ndn::Block envelopeBlock(message.getPayload());
            if (!envelope.WireDecode(envelopeBlock)) {
                return;
            }
        }
        catch (const std::exception&) {
            if (collaborationAuthTrace) {
                NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                             << " reason=envelope_decode"
                             << " dataName=" << subscription.name.toUri());
            }
            return;
        }

        // Envelope v2 carries the compact eight-byte key identifier on the
        // wire.  Collaboration encryption and its associated data are still
        // bound to the canonical request-scoped key id; accept either form
        // at the binding boundary and use the canonical id for AEAD AD.
        const std::string expectedKeyId =
            "collab|" + parsed->requestId.toUri(ndn::name::UriFormat::CANONICAL) + "|" +
            parsed->keyScope;
        const std::string compactExpectedKeyId =
            hybridCompactKeyId(expectedKeyId);
        if (envelope.getMessageType() != "COLLAB" ||
            (envelope.getKeyId() != expectedKeyId &&
             envelope.getKeyId() != compactExpectedKeyId) ||
            envelope.getEpochId() != "session" ||
            envelope.hasWrappedMessageKey()) {
            if (collaborationAuthTrace) {
                NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                             << " reason=envelope_binding"
                             << " dataName=" << subscription.name.toUri());
            }
            return;
        }

        ndn::Buffer plaintext;
        const auto associatedData = userCollaborationAssociatedData(
            subscription.name, parsed->requestId, message,
            expectedKeyId, envelope.getEpochId());
        if (!hybridAesGcmDecrypt(
                scopeKey, envelope,
                ndn::span<const uint8_t>(associatedData.data(),
                                         associatedData.size()),
                plaintext)) {
            if (collaborationAuthTrace) {
                NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=receive_rejected"
                             << " reason=decrypt"
                             << " dataName=" << subscription.name.toUri());
            }
            return;
        }

        VerifiedCollaborationData verified;
        verified.dataName = subscription.name;
        verified.requestId = parsed->requestId;
        verified.keyScope = parsed->keyScope;
        verified.topic = parsed->topic;
        verified.producer = parsed->producerName;
        verified.producerRole = message.getProducerRole();
        verified.sequence = parsed->sequence;
        verified.payload = std::move(plaintext);
        verified.signerCertificate =
            subscription.packet->getSignatureInfo()
                .getKeyLocator().getName().toUri();
        const auto wire = subscription.packet->wireEncode();
        verified.wireDigest = sha256DigestString(
            ndn::Buffer(wire.data(), wire.data() + wire.size()));

        {
            std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
            auto& records =
                m_verifiedCollaborationData[parsed->requestId];
            const auto duplicate = std::find_if(
                records.begin(), records.end(),
                [&verified] (const auto& candidate) {
                    return candidate.dataName == verified.dataName;
                });
            if (duplicate != records.end()) {
                return;
            }
            records.push_back(std::move(verified));
        }
        if (collaborationAuthTrace) {
            NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=verified"
                         << " user=" << identity.toUri()
                         << " requestId=" << parsed->requestId.toUri()
                         << " dataName=" << subscription.name.toUri()
                         << " keyScope=" << parsed->keyScope
                         << " topic=" << parsed->topic.toUri()
                         << " producerRole=" << message.getProducerRole()
                         << " payloadBytes=" << plaintext.size());
        }
        m_verifiedCollaborationCv.notify_all();
    }

    std::vector<VerifiedCollaborationData>
    ServiceUser::waitForVerifiedCollaborationData(
        const RequestId& requestId,
        const KeyScope& keyScope,
        const ndn::Name& topicPrefix,
        size_t minCount,
        int timeoutMs,
        bool consume)
    {
        if (minCount == 0) {
            throw std::invalid_argument(
                "verified collaboration minimum count must be positive");
        }
        if (timeoutMs <= 0) {
            throw std::invalid_argument(
                "verified collaboration timeout must be positive");
        }

        auto matches = [this, &requestId, &keyScope, &topicPrefix] {
            std::vector<VerifiedCollaborationData> result;
            const auto requestIt =
                m_verifiedCollaborationData.find(requestId);
            if (requestIt == m_verifiedCollaborationData.end()) {
                return result;
            }
            for (const auto& record : requestIt->second) {
                if (record.keyScope == keyScope &&
                    topicPrefix.isPrefixOf(record.topic)) {
                    result.push_back(record);
                }
            }
            return result;
        };

        std::unique_lock<std::mutex> lock(m_verifiedCollaborationMutex);
        auto result = matches();
        if (result.size() < minCount) {
            m_verifiedCollaborationCv.wait_for(
                lock, std::chrono::milliseconds(timeoutMs), [&] {
                    result = matches();
                    return result.size() >= minCount;
                });
        }

        if (consume && result.size() >= minCount) {
            auto requestIt = m_verifiedCollaborationData.find(requestId);
            if (requestIt != m_verifiedCollaborationData.end()) {
                auto& records = requestIt->second;
                records.erase(
                    std::remove_if(
                        records.begin(), records.end(),
                        [&keyScope, &topicPrefix] (const auto& record) {
                            return record.keyScope == keyScope &&
                                   topicPrefix.isPrefixOf(record.topic);
                        }),
                    records.end());
                if (records.empty()) {
                    m_verifiedCollaborationData.erase(requestIt);
                }
            }
        }
        return result;
    }

    void ServiceUser::clearVerifiedCollaborationData(
        const RequestId& requestId, const KeyScope& keyScope)
    {
        std::lock_guard<std::mutex> lock(m_verifiedCollaborationMutex);
        auto recordsIt = m_verifiedCollaborationData.find(requestId);
        if (recordsIt != m_verifiedCollaborationData.end()) {
            auto& records = recordsIt->second;
            records.erase(
                std::remove_if(
                    records.begin(), records.end(),
                    [&keyScope] (const auto& record) {
                        return record.keyScope == keyScope;
                    }),
                records.end());
            if (records.empty()) {
                m_verifiedCollaborationData.erase(recordsIt);
            }
        }
        auto keysIt = m_userCollaborationScopeKeys.find(requestId);
        if (keysIt != m_userCollaborationScopeKeys.end()) {
            keysIt->second.erase(keyScope);
            if (keysIt->second.empty()) {
                m_userCollaborationScopeKeys.erase(keysIt);
            }
        }
    }

    void ServiceUser::armStreamInactivityTimer(
        const ndn::Name& requestId,
        uint64_t interestLifetimeMs)
    {
        uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lock(m_streamInactivityMutex);
            epoch = ++m_streamInactivityEpochs[requestId];
        }
        m_scheduler.schedule(
            ndn::time::milliseconds(interestLifetimeMs),
            [this, requestId, epoch] {
                {
                    std::lock_guard<std::mutex> lock(m_streamInactivityMutex);
                    const auto timer = m_streamInactivityEpochs.find(requestId);
                    if (timer == m_streamInactivityEpochs.end() ||
                        timer->second != epoch) {
                        return;
                    }
                    m_streamInactivityEpochs.erase(timer);
                }
                const auto consumer = m_streamConsumers.find(requestId);
                const auto state = m_streamStates.find(requestId);
                if (consumer == m_streamConsumers.end() ||
                    state == m_streamStates.end()) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(state->second->mutex);
                    if (state->second->status != StreamedInvocationStatus::Streaming) {
                        return;
                    }
                }
                consumer->second->onInactivityTimeout(
                    std::chrono::steady_clock::now());
            });
    }

    void ServiceUser::disarmStreamInactivityTimer(const ndn::Name& requestId)
    {
        std::lock_guard<std::mutex> lock(m_streamInactivityMutex);
        m_streamInactivityEpochs.erase(requestId);
    }

    bool ServiceUser::initializeStreamConsumer(const ndn::Name& providerName,
                                               const ndn::Name& serviceName,
                                               const ndn::Name& requestId,
                                               const std::string& selectionDigest,
                                               const std::string& expectedProgressOperationId,
                                               std::vector<StreamProgressBinding>
                                                 expectedProgressBindings)
    {
        auto pending = m_pendingCalls.find(requestId);
        if (pending == m_pendingCalls.end() || !pending->second.streamOptions ||
            pending->second.streamEventKey.empty()) return false;
        const auto& options = *pending->second.streamOptions;
        std::string providerBootId;
        for (const auto& ack : pending->second.requestAcks) {
            if (!ack.providerName.equals(providerName) ||
                !ack.message.hasSelectionInputKeyOffer()) continue;
            const auto& offer = ack.message.getSelectionInputKeyOffer();
            if (offer.hasField("providerBootEpoch")) {
                providerBootId = offer.getField("providerBootEpoch");
            }
            break;
        }
        if (providerBootId.empty() &&
            pending->second.requestMessage.getRequestMode() == tlv::TargetedRequest) {
            const auto poolKey = makeTargetedTokenPoolKey(providerName, serviceName);
            std::lock_guard<std::mutex> lock(m_targetedTokenPoolsMutex);
            const auto offerIt = m_targetedStreamOffers.find(poolKey);
            if (offerIt != m_targetedStreamOffers.end()) {
                providerBootId = offerIt->second.providerBootEpoch;
            }
        }
        if (providerBootId.empty()) providerBootId = providerName.toUri();

        StreamBinding binding;
        binding.requestId = requestId;
        binding.requester = identity;
        binding.serviceName = serviceName;
        binding.producer = providerName;
        binding.producerBootId = providerBootId;
        binding.attemptEpoch = options.attemptEpoch;
        binding.planDigest = computeStreamSha256(
            ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(selectionDigest.data()),
                                     selectionDigest.size()));
        binding.generationId = options.generationId;
        binding.streamEpoch = options.streamEpoch;
        binding.eventKeyCommitment = options.eventKeyCommitment;
        binding.userToken = ndn::Buffer(
            reinterpret_cast<const uint8_t*>(pending->second.requestMessage.getUserToken().data()),
            pending->second.requestMessage.getUserToken().size());
        binding.policyEpoch = getCurrentPolicyEpoch(serviceName);
        binding.controllerVersion = options.controllerVersion;
        binding.deadlineEpochMs = options.deadlineEpochMs;
        try { binding.validate(); }
        catch (const std::exception&) { return false; }

        auto lifecycle = pending->second.streamLifecycle;
        if (!lifecycle) lifecycle = attachStreamLifecycle(requestId, false);
        std::shared_ptr<StreamedInvocationSharedState> state;
        const auto stateIt = m_streamStates.find(requestId);
        if (stateIt != m_streamStates.end()) {
            // requestServiceStreamingBytes installs the callbacks before the
            // request enters the normal selection machinery.  Reuse that
            // owner rather than replacing it at ACK/Targeted initialization.
            state = stateIt->second;
        }
        else {
            state = std::make_shared<StreamedInvocationSharedState>();
            state->requestId = requestId;
            state->cancelFence = [lifecycle] {
                lifecycle->user().cancel();
            };
            state->cancel = [this, requestId] {
                boost::asio::post(m_face.getIoContext(), [this, requestId] {
                    cancelStreamRequest(requestId);
                });
            };
            m_streamStates[requestId] = state;
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->status == StreamedInvocationStatus::Cancelled) {
                return false;
            }
            state->status = StreamedInvocationStatus::Streaming;
        }
        auto consumerSlot = std::make_shared<std::weak_ptr<StreamEventConsumer>>();
        auto consumer = std::make_shared<StreamEventConsumer>(
            binding, options, pending->second.streamEventKey, lifecycle,
            [providerName] (const ndn::Data& data) {
                const auto type = data.getSignatureType();
                if ((type != ndn::tlv::SignatureSha256WithRsa &&
                     type != ndn::tlv::SignatureSha256WithEcdsa) ||
                    !data.getSignatureInfo().hasKeyLocator() ||
                    data.getSignatureInfo().getKeyLocator().getType() != ndn::tlv::Name) {
                    return false;
                }
                try {
                    return ndn::security::extractIdentityFromCertName(
                        data.getSignatureInfo().getKeyLocator().getName()) == providerName;
                }
                catch (const std::exception&) {
                    return false;
                }
            },
            [this, state, requestId,
             interestLifetimeMs = options.interestLifetimeMs](
                const InvocationEventMessage& event) {
                StreamBytesCallback onEvent;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->status != StreamedInvocationStatus::Streaming) {
                        return;
                    }
                    onEvent = state->onEvent;
                    ++state->metrics.deliveredEvents;
                }
                if (onEvent) onEvent(event.payload);
                boost::asio::post(
                    m_face.getIoContext(),
                    [this, requestId, interestLifetimeMs] {
                        armStreamInactivityTimer(requestId, interestLifetimeMs);
                    });
            },
            [this, state, requestId](const ResponseMessage& response) {
                StreamBytesCallback onComplete;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->status == StreamedInvocationStatus::Cancelled ||
                        state->status == StreamedInvocationStatus::Completed ||
                        state->status == StreamedInvocationStatus::Failed) {
                        return;
                    }
                    state->status = StreamedInvocationStatus::Completed;
                    onComplete = state->onComplete;
                }
                disarmStreamInactivityTimer(requestId);
                if (onComplete) onComplete(response.getPayload());
            },
            [this, state, requestId](const StreamedInvocationError& error) {
                std::function<void(const StreamedInvocationError&)> onError;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->status == StreamedInvocationStatus::Cancelled ||
                        state->status == StreamedInvocationStatus::Completed ||
                        state->status == StreamedInvocationStatus::Failed) {
                        return;
                    }
                    state->status = StreamedInvocationStatus::Failed;
                    onError = state->onError;
                }
                disarmStreamInactivityTimer(requestId);
                if (onError) onError(error);
            },
            [this, consumerSlot, state,
             interestLifetimeMs = options.interestLifetimeMs](const ndn::Name& name) {
                // A retry can be requested from inside the timeout callback of
                // the previous exact Interest. Defer the same-name expression
                // by one io_context turn so Face finishes removing the old
                // pending Interest before the replacement is registered.
                boost::asio::post(
                    m_face.getIoContext(),
                    [this, consumerSlot, state, interestLifetimeMs, name] {
                        if (!consumerSlot->lock()) {
                            if (std::getenv("SPEC175_TRACE") != nullptr) {
                                std::cerr << "SPEC175_TRACE stream-retry-owner-expired "
                                          << name << std::endl;
                            }
                            return;
                        }
                        ndn::Interest interest(name);
                        interest.setCanBePrefix(false);
                        interest.setMustBeFresh(true);
                        interest.setInterestLifetime(
                            ndn::time::milliseconds(interestLifetimeMs));
                        if (std::getenv("SPEC175_TRACE") != nullptr) {
                            std::cerr << "SPEC175_TRACE stream-retry-express "
                                      << name << " lifetimeMs="
                                      << interestLifetimeMs << std::endl;
                        }
                        m_face.expressInterest(
                            interest,
                            [this, consumerSlot](const ndn::Interest&,
                                                 const ndn::Data& data) {
                                auto consumer = consumerSlot->lock();
                                if (!consumer) return;
                                validator->validateData(
                                    data,
                                    [consumer](const ndn::Data& validated) {
                                        consumer->accept(validated);
                                    },
                                    [consumer](
                                        const ndn::Data&,
                                        const ndn::security::ValidationError&) {
                                        consumer->reject(
                                            StreamedInvocationErrorCode::InvalidSignature,
                                            "stream event signature validation failed");
                                    });
                            },
                            [](const ndn::Interest&, const ndn::lp::Nack&) {},
                            [consumerSlot](const ndn::Interest& timedOut) {
                                if (std::getenv("SPEC175_TRACE") != nullptr) {
                                    std::cerr << "SPEC175_TRACE stream-retry-timeout "
                                              << timedOut.getName() << std::endl;
                                }
                                if (auto consumer = consumerSlot->lock()) {
                                    consumer->onRetryTimeout(
                                        timedOut.getName(),
                                        std::chrono::steady_clock::now());
                                }
                            });
                    });
            },
            [state](const ndn::Name&) {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->metrics.retryCount;
            },
            expectedProgressOperationId,
            std::move(expectedProgressBindings));
        consumer->setAuthorizationCallback(
            [this, serviceName] {
                return authorizeControllerTransition(
                    serviceName, ProtectedTransition::STREAM_EVENT);
        });
        *consumerSlot = consumer;
        consumer->start();
        m_streamConsumers[requestId] = consumer;
        // The caller primes the bounded exact-name window after authenticated
        // Selection has been published. Targeted requests call it immediately
        // after this method returns because they already have their binding;
        // ordinary and collaboration requests must not arm Interests while
        // the Provider is still waiting for Selection.
        // The initial exact Interests already have authoritative NDN timeout
        // callbacks.  Do not arm a second inactivity timer here: before the
        // first event arrives it would race those callbacks and consume the
        // same cursor's bounded retry budget twice.  The event callback above
        // arms inactivity monitoring again after every delivered event.
        return true;
    }

    void ServiceUser::onStreamEvent(
        const ndn::svs::SVSPubSub::SubscriptionData& subscription)
    {
        if (!subscription.packet) return;
        std::optional<ParsedInvocationEventName> parsed;
        std::shared_ptr<StreamEventConsumer> consumer;
        for (const auto& owner : m_streamConsumers) {
            if (!owner.second) continue;
            const auto candidate = owner.second->parseEventName(subscription.packet->getName());
            if (!candidate || !candidate->requester.equals(identity)) continue;
            if (consumer) return;
            parsed = candidate;
            consumer = owner.second;
        }
        if (!parsed || !consumer) return;
        // Stream events are protected transitions too.  A stream may have
        // been selected under an older ControllerVersion, so check the
        // current signed revocation state immediately before admitting each
        // event to the decrypt/reorder consumer.  This records a terminal
        // authorization failure at the User, rather than allowing a stale
        // event to reach the application callback.
        if (!authorizeControllerTransition(parsed->serviceName,
                                           ProtectedTransition::STREAM_EVENT)) {
            consumer->reject(StreamedInvocationErrorCode::Unauthorized,
                             "stream event rejected by Controller revocation state");
            return;
        }
        // Signature verification and any certificate fetch stay on the
        // configured validator path.  Only validated Data enters the bounded
        // reorder/decrypt consumer.
        validator->validateData(
            *subscription.packet,
            [consumer](const ndn::Data& data) { consumer->accept(data); },
            [consumer](const ndn::Data&, const ndn::security::ValidationError&) {
                consumer->reject(StreamedInvocationErrorCode::InvalidSignature,
                                 "stream event signature validation failed");
            });
    }

    void ServiceUser::cancelStreamRequest(const ndn::Name& requestId)
    {
        disarmStreamInactivityTimer(requestId);
        auto consumer = m_streamConsumers.find(requestId);
        if (consumer != m_streamConsumers.end()) {
            auto lifecycle = getStreamLifecycle(requestId);
            if (lifecycle) lifecycle->user().cancel();
            m_streamConsumers.erase(consumer);
        }
        auto state = m_streamStates.find(requestId);
        if (state != m_streamStates.end()) {
            std::lock_guard<std::mutex> lock(state->second->mutex);
            state->second->status = StreamedInvocationStatus::Cancelled;
        }
    }

    void ServiceUser::cancelStreamRequestForTest(const ndn::Name& requestId)
    {
        cancelStreamRequest(requestId);
    }

    StreamedInvocationMetrics
    ServiceUser::getStreamMetricsForTest(const ndn::Name& requestId) const
    {
        const auto state = m_streamStates.find(requestId);
        if (state == m_streamStates.end()) {
            throw std::runtime_error("stream request metrics are unavailable");
        }
        std::lock_guard<std::mutex> lock(state->second->mutex);
        return state->second->metrics;
    }

    bool ServiceUser::hasStreamStateForTest(const ndn::Name& requestId) const
    {
        const auto pending = m_pendingCalls.find(requestId);
        return m_streamStates.count(requestId) != 0 ||
               m_streamConsumers.count(requestId) != 0 ||
               (pending != m_pendingCalls.end() &&
                static_cast<bool>(pending->second.streamLifecycle));
    }

    size_t ServiceUser::streamCallbackQueueHighWaterMarkForTest(
        const ndn::Name& requestId) const
    {
        const auto consumer = m_streamConsumers.find(requestId);
        return consumer == m_streamConsumers.end() ? 0 :
               consumer->second->callbackHighWaterMark();
    }
    void ServiceUser::requestForServiceInfo()
    {
        NDN_LOG_DEBUG("Requesting Service Info");
    }

    bool ServiceUser::isFresh(const ndn::svs::SVSPubSub::SubscriptionData& subscription)
    {
        const ndn::Name& producerPrefix = subscription.producerPrefix;

        if (producerPrefix.size() < 1)
            return false;

        const auto& sessionComponent = producerPrefix[-1];
        if (sessionComponent.type() != ndn::tlv::GenericNameComponent) return false;
        const std::string lastComponentStr(reinterpret_cast<const char*>(sessionComponent.value()),
                                           sessionComponent.value_size());
        if (lastComponentStr.empty() || !std::all_of(lastComponentStr.begin(), lastComponentStr.end(),
            [](char c) { return c >= '0' && c <= '9'; })) return false;
        int sessionID = 0;

        try {
            sessionID = std::stoi(lastComponentStr);
        }
        catch (const std::invalid_argument& e) {
            NDN_LOG_WARN("Wrong sessionID" << lastComponentStr);
            return false;
        }
        catch (const std::out_of_range& e) {
            NDN_LOG_WARN("Wrong sessionID: " << lastComponentStr);
            return false;
        }

        ndn::Name basePrefix = producerPrefix.getPrefix(-1); // 去掉最后一个component作为key

        auto it = m_sessionIDMap.find(basePrefix);
        if (it != m_sessionIDMap.end()) {
            if (it->second > sessionID) {
                return false;
            }
        }

        // Update
        m_sessionIDMap[basePrefix] = sessionID;
        return true;
    }


    void ServiceUser::OnResponseDecryptionErrorCallback(
        const ndn::Name& serviceProviderName,
        const ndn::Name& serviceName,
        const ndn::Name& requestID,
        const std::string& msg)
    {
        NDN_LOG_INFO("[ServiceUser] OnResponseDecryptionErrorCallback provider="
                  << serviceProviderName.toUri()
                  << " service=" << serviceName.toUri()
                  << " requestID=" << requestID.toUri()
                  << " error=" << msg);
        NDN_LOG_ERROR("OnResponseDecryptionErrorCallback: "
                      << serviceProviderName << serviceName << requestID
                      << " with error: " << msg);
    }
}

namespace ndnsf
{
    namespace strategy
    {
        extern const std::shared_ptr<const ndn_service_framework::AckSelectionPolicy>
            FirstResponding = ndn_service_framework::strategy::FirstResponding;
        extern const std::shared_ptr<const ndn_service_framework::AckSelectionPolicy>
            RandomSelection = ndn_service_framework::strategy::RandomSelection;
        extern const std::shared_ptr<const ndn_service_framework::AckSelectionPolicy>
            AllSelected = ndn_service_framework::strategy::AllSelected;
    }
}
