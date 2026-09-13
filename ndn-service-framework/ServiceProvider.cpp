#include <ServiceProvider.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ndn-cxx/security/validation-error.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/transform/public-key.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace ndn_service_framework
{
    NDN_LOG_INIT(ndn_service_framework.ServiceProvider);

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

        ndn::security::Certificate
        getExistingSigningCertificateOrFallback(const ndn::security::Certificate& encryptionCert)
        {
            ndn::KeyChain keyChain;
            return getExistingSigningCertificateOrFallback(keyChain, encryptionCert);
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

            throw std::invalid_argument("ServiceProvider requires an RSA encryption certificate for NAC-ABE");
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
            // MiniNDN places independent applications in distinct HOME
            // directories but may run them under one UID (including UID 0 in
            // a rootless user namespace).  Scope the lock to HOME so a stale
            // privileged-run lock cannot block another node's SVS setup.
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
        boolEnvOrDefault(const char* name, bool fallback)
        {
            const char* value = std::getenv(name);
            if (value == nullptr) {
                return fallback;
            }
            std::string text(value);
            std::transform(text.begin(), text.end(), text.begin(),
                           [] (unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (text.empty()) {
                return fallback;
            }
            return !(text == "0" || text == "false" || text == "no" || text == "off");
        }

        std::string
        replayTokenHash(const std::string& scope,
                        const ndn::Name& peer,
                        const ndn::Name& serviceName,
                        const std::string& token)
        {
            if (token.empty()) {
                return "";
            }
            ndn::util::Sha256 digest;
            digest << scope;
            digest << peer.toUri();
            digest << serviceName.toUri();
            digest << token;
            return digest.toString();
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

        constexpr size_t TARGETED_TOKEN_BATCH_MIN = 1;
        constexpr size_t TARGETED_TOKEN_BATCH_MAX = 256;

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

        size_t
        responseLargeDataThresholdBytes()
        {
            if (isTruthyEnv("NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE")) {
                return 0;
            }
            const int configured =
                intEnvOrDefault("NDNSF_RESPONSE_LARGE_DATA_THRESHOLD", 6000);
            return configured <= 0 ? 0 : static_cast<size_t>(configured);
        }

        std::string
        sanitizeLargeDataObjectId(std::string value)
        {
            if (value.empty()) {
                value = "response";
            }
            for (auto& ch : value) {
                const bool ok = (ch >= 'a' && ch <= 'z') ||
                                (ch >= 'A' && ch <= 'Z') ||
                                (ch >= '0' && ch <= '9') ||
                                ch == '-' || ch == '_' || ch == '.';
                if (!ok) {
                    ch = '-';
                }
            }
            return value;
        }

        ndn::Name
        extractLargeDataProducerPrefix(const ndn::Name& dataName)
        {
            for (ssize_t i = 0; i + 1 < static_cast<ssize_t>(dataName.size()); ++i) {
                if (dataName[i].toUri() == "NDNSF" &&
                    dataName[i + 1].toUri() == "LARGE-DATA") {
                    return dataName.getPrefix(i);
                }
            }
            return {};
        }

        ndn::Name
        makeLargeResponseDataNameWithoutPrefix(const ndn::Name& requesterName,
                                               const ndn::Name& serviceName,
                                               const ndn::Name& requestId,
                                               const std::string& objectId)
        {
            ndn::Name name("/NDNSF/LARGE-RESPONSE");
            name.append(ndn::name::Component(requesterName.toUri()));
            name.append(serviceName);
            name.append(requestId);
            name.append(objectId);
            return name;
        }

        ndn::Name
        makeLargeResponseDataName(const ndn::Name& providerPrefix,
                                  const ndn::Name& requesterName,
                                  const ndn::Name& serviceName,
                                  const ndn::Name& requestId,
                                  const std::string& objectId)
        {
            ndn::Name name(providerPrefix);
            name.append(makeLargeResponseDataNameWithoutPrefix(requesterName,
                                                               serviceName,
                                                               requestId,
                                                               objectId));
            return name;
        }

        size_t
        requestScopedResponseChunkBytes(size_t thresholdBytes)
        {
            const int configured = intEnvOrDefault(
                "NDNSF_REQUEST_SCOPED_RESPONSE_CHUNK_BYTES", 4096);
            if (configured <= 0) {
                return std::max<size_t>(1, thresholdBytes);
            }
            return std::max<size_t>(1, std::min<size_t>(
                static_cast<size_t>(configured),
                std::max<size_t>(1, thresholdBytes)));
        }

        std::string
        sha256DigestString(const ndn::Buffer& payload)
        {
            ndn::util::Sha256 digest;
            if (!payload.empty()) {
                digest << std::string(reinterpret_cast<const char*>(payload.data()),
                                      payload.size());
            }
            // Canonical NDNSF digest strings are lowercase.  ServiceUser's
            // digest helper emits the same canonical form, so an uppercase
            // hex value here breaks every cross-role comparison (for example
            // the externalized collaboration-assignment digest check).
            auto hex = digest.toString();
            std::transform(hex.begin(), hex.end(), hex.begin(),
                           [] (unsigned char value) {
                               return static_cast<char>(std::tolower(value));
                           });
            return "sha256:" + hex;
        }

        ndn::Name
        makeProviderAuthorizationAttribute(const ndn::Name& serviceName)
        {
            ndn::Name attribute("/SERVICE");
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
        defaultNdnsfAckThreads()
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

        struct CollaborationLargeFetchTiming
        {
            std::chrono::steady_clock::time_point start;
            std::chrono::steady_clock::time_point firstSegmentReceived;
            std::chrono::steady_clock::time_point lastSegmentReceived;
            std::chrono::steady_clock::time_point lastSegmentValidated;
            std::chrono::system_clock::time_point firstSegmentWall;
            std::chrono::system_clock::time_point completeWall;
            size_t receivedSegments = 0;
            size_t validatedSegments = 0;
            size_t receivedWireBytes = 0;
            size_t nacks = 0;
            size_t timeouts = 0;
        };

        double
        elapsedMsSince(const std::chrono::steady_clock::time_point& start,
                       const std::chrono::steady_clock::time_point& end)
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() /
                   1000.0;
        }

        int64_t
        epochMs(const std::chrono::system_clock::time_point& timePoint)
        {
            if (timePoint == std::chrono::system_clock::time_point{}) {
                return 0;
            }
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                timePoint.time_since_epoch()).count();
        }

        int
        hexValue(char c)
        {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return 10 + c - 'a';
            }
            if (c >= 'A' && c <= 'F') {
                return 10 + c - 'A';
            }
            return -1;
        }

        ndn::Buffer
        hexDecode(const std::string& text)
        {
            if (text.size() % 2 != 0) {
                return {};
            }
            ndn::Buffer out(text.size() / 2);
            for (size_t i = 0; i < out.size(); ++i) {
                const int hi = hexValue(text[i * 2]);
                const int lo = hexValue(text[i * 2 + 1]);
                if (hi < 0 || lo < 0) {
                    return {};
                }
                out[i] = static_cast<uint8_t>((hi << 4) | lo);
            }
            return out;
        }

        std::string
        hexEncode(const ndn::Buffer& value)
        {
            static const char* digits = "0123456789abcdef";
            std::string out;
            out.reserve(value.size() * 2);
            for (const auto byte : value) {
                out.push_back(digits[(byte >> 4) & 0x0f]);
                out.push_back(digits[byte & 0x0f]);
            }
            return out;
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

        bool
        buffersEqual(const ndn::Buffer& lhs, const ndn::Buffer& rhs)
        {
            return lhs.size() == rhs.size() &&
                   std::equal(lhs.begin(), lhs.end(), rhs.begin());
        }

        ndn::Buffer
        bufferFromText(const std::string& text)
        {
            return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()),
                               text.size());
        }

        std::string
        numberToText(double value)
        {
            std::ostringstream os;
            os << value;
            return os.str();
        }

        uint64_t
        uintFieldOrDefault(const std::map<std::string, std::string>& fields,
                           const std::string& key,
                           uint64_t fallback = 0)
        {
            const auto it = fields.find(key);
            if (it == fields.end() || it->second.empty()) {
                return fallback;
            }
            try {
                return static_cast<uint64_t>(std::stoull(it->second));
            }
            catch (const std::exception&) {
                return fallback;
            }
        }

        double
        doubleFieldOrDefault(const std::map<std::string, std::string>& fields,
                             const std::string& key,
                             double fallback = 0.0)
        {
            const auto it = fields.find(key);
            if (it == fields.end() || it->second.empty()) {
                return fallback;
            }
            try {
                return std::stod(it->second);
            }
            catch (const std::exception&) {
                return fallback;
            }
        }

        ndn::Name
        nameFieldOrDefault(const std::map<std::string, std::string>& fields,
                           const std::string& key)
        {
            const auto it = fields.find(key);
            if (it == fields.end() || it->second.empty()) {
                return ndn::Name();
            }
            return ndn::Name(it->second);
        }

        ndn::Buffer
        collaborationAssociatedData(const ndn::Name& dataName,
                                    const ndn::Name& requestId,
                                    const CollaborationDataMessage& message,
                                    const std::string& keyId,
                                    const std::string& epochId)
        {
            // CollaborationEnvelopeV2 carries the compact wire key ID after
            // decode. Authenticate the same canonical identifier on both
            // producer and consumer paths; otherwise a long logical key ID
            // authenticates successfully before encoding but fails after the
            // consumer observes its compact representation.
            const auto wireKeyId = hybridCompactKeyId(keyId);
            const std::string text =
                dataName.toUri() + "|COLLAB|" + requestId.toUri() + "|" +
                message.getKeyScope() + "|" + message.getTopic().toUri() + "|" +
                message.getProducerRole() + "|" +
                std::to_string(message.getSequence()) + "|" + wireKeyId + "|" + epochId;
            return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
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

        ndn::svs::SeqNo
        publishSvsBytes(const std::shared_ptr<ndn::svs::SVSPubSub>& svs,
                        const ndn::Name& name,
                        const ndn::Buffer& content,
                        int freshnessMs)
        {
            if (svs == nullptr || content.empty()) {
                return 0;
            }
            const auto freshness = ndn::time::milliseconds(
                freshnessMs <= 0 ? 60000 : freshnessMs);
            const ndn::span<const uint8_t> bytes(content.data(), content.size());
            if (useAsyncSvsPublish()) {
                return svs->publishAsync(name, bytes, ndn::Name(), freshness);
            }
            return svs->publish(name, bytes, ndn::Name(), freshness);
        }

        bool
        nameFieldMatches(const ndn::Name& name,
                         const std::string& marker,
                         const ndn::Name& expected)
        {
            if (marker.empty() || expected.empty()) {
                return false;
            }
            bool found = false;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (name.get(i).toUri() != marker) {
                    continue;
                }
                if (found || i + 1 + expected.size() > name.size()) {
                    return false;
                }
                for (std::size_t j = 0; j < expected.size(); ++j) {
                    if (name.get(i + 1 + j) != expected.get(j)) {
                        return false;
                    }
                }
                found = true;
                i += expected.size();
            }
            return found;
        }

        std::optional<std::size_t>
        parseDataV1SegmentNumber(const ndn::Name& name,
                                 const ndn::Name& producerPrefix,
                                 const ndn::Name& requestId,
                                 std::uint64_t operationIndex,
                                 const std::string& producerRank,
                                 const std::string& tensorDigest,
                                 std::size_t maxSegments)
        {
            if (!producerPrefix.isPrefixOf(name) ||
                name.size() <= producerPrefix.size() || maxSegments == 0) {
                return std::nullopt;
            }
            // SVS catch-up is shared by all requests.  Filter by request id
            // before collecting a segment, otherwise an older request with
            // the same operation/rank/tensor can fill the slot and only fail
            // much later in ProviderGroupCoordinator's capability check.
            if (!nameFieldMatches(name, "REQ", requestId)) {
                return std::nullopt;
            }
            bool operationMatched = false;
            bool rankMatched = false;
            bool tensorMatched = false;
            const auto matchesComponent = [] (const ndn::name::Component& actual,
                                               const std::string& expected) {
                if (expected.empty()) {
                    return true;
                }
                const ndn::Name expectedName(expected);
                return expectedName.size() == 1 && actual == expectedName.get(0);
            };
            std::optional<std::size_t> segmentNumber;
            for (std::size_t i = producerPrefix.size(); i + 1 < name.size(); ++i) {
                const auto marker = name.get(i).toUri();
                const auto value = name.get(i + 1).toUri();
                if (marker == "OP") {
                    try {
                        operationMatched =
                          std::stoull(value) == operationIndex;
                    }
                    catch (const std::exception&) {
                        return std::nullopt;
                    }
                }
                else if (marker == "RANK") {
                    rankMatched = matchesComponent(name.get(i + 1), producerRank);
                }
                else if (marker == "TENSOR") {
                    tensorMatched = matchesComponent(name.get(i + 1), tensorDigest);
                }
                else if (marker == "SEG") {
                    try {
                        const auto parsed = std::stoull(value);
                        if (parsed >= maxSegments) {
                            return std::nullopt;
                        }
                        segmentNumber = static_cast<std::size_t>(parsed);
                    }
                    catch (const std::exception&) {
                        return std::nullopt;
                    }
                }
            }
            if (!operationMatched || !rankMatched || !tensorMatched || !segmentNumber) {
                return std::nullopt;
            }
            return segmentNumber;
        }

        ndn::Buffer
        blockToPayloadBuffer(const ndn::Block& block)
        {
            try {
                ndn::Block wireBlock(block);
                if (!wireBlock.hasWire()) {
                    wireBlock.encode();
                }
                return ndn::Buffer(wireBlock.value(), wireBlock.value_size());
            }
            catch (const std::exception&) {
                return ndn::Buffer();
            }
        }

        uint64_t
        nowMicroseconds()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
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
        cryptoStageForName(const ndn::Name& name)
        {
            for (size_t i = 0; i < name.size(); ++i) {
                const auto component = name[i].toUri();
                if (component == "REQUEST") {
                    return "request";
                }
                if (component == "ACK") {
                    return "ack";
                }
                if (component == "SELECTION") {
                    return "selection";
                }
                if (component == "RESPONSE") {
                    return "response";
                }
            }
            return "unknown";
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

        ServiceProvider::AckStrategyHandler
        wrapLegacyAckStrategyHandler(ServiceProvider::LegacyAckStrategyHandler handler)
        {
            if (!handler) {
                return ServiceProvider::AckStrategyHandler{};
            }

            return [handler = std::move(handler)](const RequestMessage&) {
                RequestAckMessage legacyAck;
                const auto result = handler(legacyAck);

                ServiceProvider::AckDecision decision;
                decision.status = result.first;
                decision.payload = blockToPayloadBuffer(result.second);
                return decision;
            };
        }

        uint64_t
        nowMilliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
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
        hasProviderPermission(const ndn::Name& providerIdentity,
                              const ndn::Name& serviceName,
                              const ServiceAuthorizationTable& permissionTable)
        {
            const ndn::Name fullServiceName =
                providerIdentity.isPrefixOf(serviceName)
                    ? serviceName
                    : ndn::Name(providerIdentity.toUri()).append(serviceName);
            return permissionTable.contains(fullServiceName.toUri(),
                                            serviceName.toUri(),
                                            tlv::ProviderPermission);
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

        ndn::Name
        makeCollaborationRolePermissionName(const ndn::Name& serviceName,
                                            const std::string& role)
        {
            ndn::Name roleName(serviceName);
            roleName.append("ROLE");
            if (!role.empty() && role.front() == '/') {
                roleName.append(ndn::Name(role));
            }
            else {
                roleName.append(role);
            }
            return roleName;
        }

        bool
        hasProviderCollaborationRolePermission(
            const ndn::Name& providerIdentity,
            const ndn::Name& serviceName,
            const std::string& role,
            const ServiceAuthorizationTable& permissionTable)
        {
            const auto rolePermission =
                makeCollaborationRolePermissionName(serviceName, role);
            return permissionTable.contains(
                ndn::Name(providerIdentity.toUri()).append(rolePermission).toUri(),
                rolePermission.toUri(), tlv::ProviderPermission);
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

        std::optional<ndn::Name>
        extractPermissionControllerIdentity(const ndn::Interest& interest)
        {
            const auto& name = interest.getName();
            for (size_t i = 0; i < name.size(); ++i) {
                if (name[i].toUri() == "NDNSF") {
                    return name.getPrefix(i);
                }
            }
            return std::nullopt;
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
    }

    void ServiceProvider::setDeploymentPrepareHandler(DeploymentPrepareHandler handler)
    {
        m_deploymentPrepareHandler = std::move(handler);
    }

    void ServiceProvider::setProviderReadyPublisher(ProviderReadyPublisher publisher)
    {
        m_providerReadyPublisher = std::move(publisher);
    }

    bool ServiceProvider::acceptExecutionActivate(
        const ExecutionActivateMessage& activation,
        std::string* rejectionReason)
    {
        auto reject = [&] (const std::string& reason) {
            if (rejectionReason != nullptr) *rejectionReason = reason;
            return false;
        };
        static const std::vector<std::string> required = {
            "requestId", "selectionDigest", "deploymentPlanDigest",
            "readySetDigest", "memberSetDigest", "requesterIdentity",
            "activationSequence", "expiresAtUs"
        };
        for (const auto& field : required) {
            if (!activation.hasField(field) || activation.getField(field).empty()) {
                return reject("activation missing " + field);
            }
        }
        auto preparedIt = m_preparedDeployments.find(
            activation.getField("selectionDigest"));
        if (preparedIt == m_preparedDeployments.end()) {
            return reject("no prepared deployment for selection");
        }
        auto& prepared = preparedIt->second;
        if (prepared.activated) {
            if (activation.computeDigest() == prepared.activationDigest) return true;
            return reject("conflicting duplicate activation");
        }
        uint64_t expiresAtUs = 0;
        try { expiresAtUs = std::stoull(activation.getField("expiresAtUs")); }
        catch (...) { return reject("invalid activation expiry"); }
        if (expiresAtUs <= nowMicroseconds()) return reject("activation expired");
        if (activation.getField("requestId") != prepared.requestId.toUri() ||
            activation.getField("requesterIdentity") != prepared.requesterName.toUri() ||
            activation.getField("deploymentPlanDigest") != prepared.plan.computeDigest()) {
            return reject("activation binding mismatch");
        }
        prepared.activated = true;
        prepared.activationDigest = activation.computeDigest();
        RequestMessage requestCopy = prepared.request;
        std::shared_ptr<RegistrationState> inlineRegistrationState;
        if (dispatchRequestExecutionAsync(prepared.requesterName,
                                          prepared.providerName,
                                          prepared.serviceName,
                                          prepared.requestId,
                                          requestCopy,
                                          prepared.selectionDigest,
                                          &inlineRegistrationState)) {
            return true;
        }
        // spec182: a pool-0 inline dispatch applies the same generation
        // fence as the async path; refusal already published its failure.
        if (!gateInlineRequestExecution(
                prepared.requesterName, prepared.providerName,
                prepared.serviceName, prepared.requestId, requestCopy,
                prepared.selectionDigest, inlineRegistrationState)) {
            return true;
        }
        auto response = dispatchRequest(prepared.requesterName,
                                        prepared.providerName,
                                        prepared.serviceName,
                                        prepared.requestId,
                                        requestCopy);
        finishRequestExecutionOnEventLoop(prepared.requesterName,
                                          prepared.providerName,
                                          prepared.serviceName,
                                          prepared.requestId,
                                          requestCopy,
                                          std::move(response),
                                          prepared.selectionDigest,
                                          inlineRegistrationState);
        return true;
    }

    void ServiceProvider::publishProviderReady(
        const ndn::Name& requesterIdentity,
        const ProviderReadyMessage& ready,
        const std::string& statusHandle,
        int attempt)
    {
        if (attempt > 2) {
            NDN_LOG_WARN("ProviderReady acknowledgement retry exhausted requestId="
                         << ready.getField("requestId"));
            return;
        }
        ndn::Interest interest(makeProviderReadyName(
            requesterIdentity, DeploymentControlMessage::VERSION, statusHandle));
        interest.setMustBeFresh(true);
        interest.setCanBePrefix(false);
        interest.setInterestLifetime(ndn::time::milliseconds(500));
        interest.setApplicationParameters(ready.WireEncode());
        (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain)
            .sign(interest, m_signingInfo);
        m_face.expressInterest(
            interest,
            [this, requesterIdentity, ready, statusHandle](const ndn::Interest&,
                                                           const ndn::Data& data) {
                nac_validator.validate(
                    data,
                    [ready](const ndn::Data& validated) {
                        ReadyAcknowledgement ack;
                        if (!ack.WireDecode(validated.getContent()) ||
                            !ack.hasField("readyMessageDigest") ||
                            ack.getField("readyMessageDigest") != ready.computeDigest()) {
                            NDN_LOG_WARN("Reject malformed ProviderReady acknowledgement");
                        }
                    },
                    [](const ndn::Data&, const ndn::security::ValidationError& error) {
                        NDN_LOG_WARN("ProviderReady acknowledgement validation failed: " << error);
                    });
            },
            [this, requesterIdentity, ready, statusHandle, attempt](const ndn::Interest&,
                                                                    const ndn::lp::Nack&) {
                m_scheduler.schedule(ndn::time::milliseconds(100 * (attempt + 1)),
                    [this, requesterIdentity, ready, statusHandle, attempt] {
                        publishProviderReady(requesterIdentity, ready, statusHandle, attempt + 1);
                    });
            },
            [this, requesterIdentity, ready, statusHandle, attempt](const ndn::Interest&) {
                m_scheduler.schedule(ndn::time::milliseconds(100 * (attempt + 1)),
                    [this, requesterIdentity, ready, statusHandle, attempt] {
                        publishProviderReady(requesterIdentity, ready, statusHandle, attempt + 1);
                    });
            });
    }

    bool ServiceProvider::handleExecutionActivateInterest(const ndn::Interest& interest)
    {
        const auto parsed = parseExecutionActivateName(interest.getName());
        if (!parsed) return false;
        nac_validator.validate(
            interest,
            [this](const ndn::Interest& validated) {
                ExecutionActivateMessage activation;
                bool accepted = activation.WireDecode(validated.getApplicationParameters());
                std::string reason;
                if (accepted) {
                    const auto signatureInfo = validated.getSignatureInfo();
                    accepted = signatureInfo && signatureInfo->hasKeyLocator() &&
                        signatureInfo->getKeyLocator().getType() == ndn::tlv::Name &&
                        activation.hasField("requesterIdentity") &&
                        ndn::security::extractIdentityFromCertName(
                            signatureInfo->getKeyLocator().getName()).toUri() ==
                            activation.getField("requesterIdentity");
                    if (!accepted) reason = "activation signer identity mismatch";
                }
                if (accepted) accepted = acceptExecutionActivate(activation, &reason);
                ReadyAcknowledgement ack;
                ack.setField("accepted", accepted ? "true" : "false");
                ack.setField("reason", reason);
                if (activation.hasField("selectionDigest"))
                    ack.setField("activationDigest", activation.computeDigest());
                ack.setField("issuedAtUs", std::to_string(nowMicroseconds()));
                ndn::Data data(validated.getName());
                data.setFreshnessPeriod(ndn::time::milliseconds(250));
                data.setContent(ack.WireEncode());
                (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain)
                    .sign(data, m_signingInfo);
                m_face.put(data);
            },
            [](const ndn::Interest&, const ndn::security::ValidationError& error) {
                NDN_LOG_WARN("ExecutionActivate signature validation failed: " << error);
            });
        return true;
    }

    ServiceProvider::ServiceProvider(ndn::Face& face,
                                     ndn::Name group_prefix,
                                     ndn::security::Certificate identityCert,
                                     ndn::security::Certificate attrAuthorityCertificate,
                                     std::string trustSchemaPath)
        : ServiceProvider(face,
                          std::move(group_prefix),
                          getExistingEncryptionCertificateOrThrow(identityCert),
                          getExistingSigningCertificateOrFallback(identityCert),
                          std::move(attrAuthorityCertificate),
                          std::move(trustSchemaPath))
    {
    }

    ServiceProvider::ServiceProvider(ndn::Face& face,
                                     ndn::Name group_prefix,
                                     ndn::security::Certificate encryptionCert,
                                     ndn::security::Certificate signingCert,
                                     ndn::security::Certificate attrAuthorityCertificate,
                                     std::string trustSchemaPath)
        : m_face(face),
        m_scheduler(m_face.getIoContext()),
        m_groupPrefix(group_prefix),
        identity(encryptionCert.getIdentity()),
        validator(std::make_shared<MessageValidator>(
          trustSchemaPath, group_prefix, &face)),
        identityCert(encryptionCert),
        signingCert(signingCert),
        random(ndn::random::getRandomNumberEngine()),
        // The io_context-aware IMS constructor enforces MustBeFresh. This is
        // required for streamed-event retention: an exact retry must not
        // retrieve an event after its advertised retention window.
        m_IMS(m_face.getIoContext(), 50000)
    {
        // spec182 scoped registrations hand ownership to this provider; the
        // destructor serializes against the Face-side cleanup through this
        // control object.
        m_registrationControl = std::make_shared<RegistrationControl>();
        m_registrationControl->owner = this;
        ensureSameIdentity(encryptionCert, signingCert, "ServiceProvider");
        if (!isRsaCertificate(encryptionCert)) {
            throw std::invalid_argument("ServiceProvider encryptionCert must be RSA for NAC-ABE");
        }
        NDN_LOG_WARN("NDNSF_PROVIDER_INIT_STAGE stage=constructor_begin provider="
                     << identity.toUri());
        NDN_LOG_WARN("NDNSF_CERT_SELECTION role=provider identity="
                     << identity.toUri()
                     << " encryptionCert=" << encryptionCert.getName()
                     << " signingCert=" << signingCert.getName()
                     << " splitSigning="
                     << (encryptionCert.getName() == signingCert.getName() ? "false" : "true"));
        m_handlerPool.setThreadCount(defaultNdnsfWorkerThreads());
        m_ackPool.setThreadCount(defaultNdnsfAckThreads());
        m_fetchPool.setThreadCount(2);
        NDN_LOG_INFO("NDNSF_HANDLER_THREADS role=provider workers="
                     << m_handlerPool.getThreadCount());
        NDN_LOG_INFO("NDNSF_ACK_THREADS role=provider workers="
                     << m_ackPool.getThreadCount());
        if (isTruthyEnv("NDNSF_ENABLE_NDNSD") &&
            std::getenv("NDNSF_DISABLE_NDNSD") == nullptr) {
            m_ServiceDiscovery.enable(group_prefix,
                                      identity,
                                      m_face,
                                      m_keyChain,
                                      std::bind(&ServiceProvider::processNDNSDServiceInfoCallback, this, _1));
        }

        nac_validator.load(trustSchemaPath);
        nacConsumer = std::make_unique<ndn::nacabe::Consumer>(
            m_face, m_keyChain, nac_validator, encryptionCert,
            attrAuthorityCertificate);
        nacProducer = std::make_unique<ndn::nacabe::CacheProducer>(
            m_face, m_keyChain, nac_validator, encryptionCert,
            attrAuthorityCertificate);
        NDN_LOG_WARN("NDNSF_PROVIDER_INIT_STAGE stage=validator_loaded provider="
                     << identity.toUri());

        NDN_LOG_INFO("[ServiceProvider] NAC_ABE_BOOTSTRAP provider="
                  << identity.toUri()
                  << " authority=" << attrAuthorityCertificate.getIdentity().toUri()
                  << " dkPrefix="
                  << ndn::Name(attrAuthorityCertificate.getIdentity()).append("DKEY").toUri());

        // Serve NDNSF, CK, NDNSF-DI, and application-owned IMS data.  The
        // same filters are re-registered after Controller authorization in
        // registerContentFilters() if NFD rejected the cold-start attempt.
        registerContentFilters();
        NDN_LOG_WARN("NDNSF_PROVIDER_INIT_STAGE stage=content_filters_registered provider="
                     << identity.toUri());

        m_signingInfo = ndn::security::signingByCertificate(signingCert);

        ndn::svs::SecurityOptions secOpts(m_keyChain);
        secOpts.interestSigner = std::make_shared<CommandInterestSigner>(m_keyChain);
        secOpts.interestSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        const auto signingKeyName = signingCert.getKeyName();
        const auto signingCertName = signingCert.getName();
        secOpts.interestSigner->signingInfo.setSigningKeyName(signingKeyName);
        secOpts.dataSigner->signingInfo.setSigningCertName(signingCertName);
        secOpts.dataSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.pubSigner->signingInfo.setSigningCertName(signingCertName);
        secOpts.pubSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.validator = validator;
        secOpts.encapsulatedDataValidator = validator;

        // Do not fetch publications older than 10 seconds
        ndn::svs::SVSPubSubOptions opts;
        configureSvsProtocol(opts);
        NDN_LOG_INFO("NDNSF_SVS_OPTIONS role=provider"
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
        NDN_LOG_WARN("NDNSF_SVS_PUBLICATION_FETCH_CONFIG role=provider retries="
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
        node_id.append("provider");
        int session_id = m_configManager.loadAndIncrement(group_prefix.toUri(), node_id.toUri());
        node_id.append(std::to_string(session_id));
        {
            const auto svsLockPath = userScopedLockPath("/tmp/ndnsf-svs-registration");
            FileLock svsRegistrationLock(svsLockPath.c_str());
            m_svsps = std::make_shared<ndn::svs::SVSPubSub>(
                ndn::Name(group_prefix),
                ndn::Name(node_id),
                m_face,
                std::bind(&ServiceProvider::onMissingData, this, _1),
                opts,
                secOpts);
            const auto& syncProfile = m_svsps->getSyncProtocolOptions();
            NDN_LOG_INFO("NDNSF_SVS_PROTOCOL role=provider version="
                         << static_cast<int>(syncProfile.version)
                         << " lifetimeMs=" << syncProfile.syncInterestLifetime.count()
                         << " suppressionMs=" << syncProfile.suppressionPeriod.count()
                         << " periodicMs=" << syncProfile.periodicTimeout.count());
            if (std::getenv("NDNSF_SVS_PERIODIC_SYNC_MS") != nullptr) {
                const int periodicSyncMs =
                    std::max(1, intEnvOrDefault("NDNSF_SVS_PERIODIC_SYNC_MS", 30000));
                m_svsps->getSVSync().getCore().setPeriodicSyncTime(
                    ndn::time::milliseconds(periodicSyncMs));
                NDN_LOG_INFO("NDNSF_SVS_PERIODIC_SYNC_MS role=provider value="
                             << periodicSyncMs);
            }
            NDN_LOG_INFO("NDNSF_SVS_ASYNC_PUBLISH role=provider "
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
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_SYNC enabled role=provider workers="
                             << workers << " queue=" << queue);
            }
            else {
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_SYNC disabled role=provider"
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
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_PRODUCTION enabled role=provider workers="
                             << workers << " queue=" << queue
                             << " signInWorker=" << signInWorker
                             << " extraBlockInWorker=" << extraBlockInWorker);
            }
            else {
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_PRODUCTION disabled role=provider"
                             " reason=explicit-opt-in-required");
            }
            if (isTruthyEnv("NDNSF_SVS_SYNC_BATCHING")) {
                const int windowMs = std::max(0, intEnvOrDefault("NDNSF_SVS_SYNC_BATCH_MS", 5));
                m_svsps->getSVSync().getCore().setSyncInterestBatching(
                    true, ndn::time::milliseconds(windowMs));
                NDN_LOG_INFO("NDNSF_SVS_SYNC_BATCHING enabled role=provider windowMs="
                             << windowMs);
            }
        }
        NDN_LOG_WARN("NDNSF_PROVIDER_INIT_STAGE stage=svs_pubsub_ready provider="
                     << identity.toUri());

        // Permission renewal must remain reachable even without an initial
        // grant/DKEY. Crypto readiness is asynchronous; admission remains
        // fail-closed until permission, signed status and key installation.
        activeNacConsumer().obtainDecryptionKey();
        if (activeNacConsumer().readyForDecryption())
            NDN_LOG_INFO("DK_DECRYPT_SUCCESS provider=" << identity.toUri());
        else {
            NDN_LOG_INFO("NDNSF_NAC_BOOTSTRAP_PENDING role=provider");
            // Controller status installation can invalidate the constructor-
            // time DKEY fetch before its callback runs.  Re-arm a bounded
            // readiness probe so the first protected request never races the
            // asynchronous NAC-ABE bootstrap.
            scheduleDeferredDkeyRefreshRetry(identity);
        }
        NDN_LOG_WARN("NDNSF_PROVIDER_INIT_STAGE stage=constructor_done provider="
                     << identity.toUri());

        // Opt-in durable runtime status (NDNSF_PERSIST_RUNTIME_STATE, FR-039):
        // re-verify and seed statuses accepted by an earlier process of this
        // identity, then confirm each against its Controller online.
        if (RuntimeStatusStore::enabled()) {
            m_runtimeStatusStore = std::make_unique<RuntimeStatusStore>(
                RuntimeStatusStore::defaultStorePath("provider", identity));
        }
        restorePersistedRuntimeStatuses();

    }

    ServiceProvider::ServiceProvider(LocalMockTag,
                                     ndn::Face& face,
                                     ndn::Name group_prefix,
                                     ndn::security::Certificate identityCert,
                                     ndn::security::Certificate attrAuthorityCertificate,
                                     std::string trustSchemaPath)
        : ServiceProvider(LocalMockTag{},
                          face,
                          std::move(group_prefix),
                          getExistingEncryptionCertificateOrThrow(identityCert),
                          getExistingSigningCertificateOrFallback(identityCert),
                          std::move(attrAuthorityCertificate),
                          std::move(trustSchemaPath))
    {
    }

    ServiceProvider::ServiceProvider(LocalMockTag,
                                     ndn::Face& face,
                                     ndn::Name group_prefix,
                                     ndn::security::Certificate encryptionCert,
                                     ndn::security::Certificate signingCert,
                                     ndn::security::Certificate attrAuthorityCertificate,
                                     std::string trustSchemaPath)
        : m_face(face),
        m_scheduler(m_face.getIoContext()),
        m_groupPrefix(group_prefix),
        identity(encryptionCert.getIdentity()),
        m_keyChain(),
        m_svsps(nullptr),
        validator(std::make_shared<MessageValidator>(trustSchemaPath, group_prefix)),
        nac_validator(m_face),
        identityCert(encryptionCert),
        signingCert(signingCert),
        attrAuthorityCertificate(attrAuthorityCertificate),
        random(ndn::random::getRandomNumberEngine()),
        m_IMS(m_face.getIoContext(), 50000),
        m_configManager("/tmp/ndnsf-service-provider-local-mock.conf")
    {
        m_registrationControl = std::make_shared<RegistrationControl>();
        m_registrationControl->owner = this;
        m_isLocalMock = true;
        ensureSameIdentity(encryptionCert, signingCert, "ServiceProvider");
        if (!isRsaCertificate(encryptionCert)) {
            throw std::invalid_argument("ServiceProvider encryptionCert must be RSA for NAC-ABE");
        }
        // LocalMockTag still owns real NAC-ABE Consumer/Producer instances.
        // Load the same trust schema as the production constructor before the
        // fixture pumps their constructor-time public-parameter Interests.
        nac_validator.load(trustSchemaPath);
        nacConsumer = std::make_unique<ndn::nacabe::Consumer>(
            m_face, m_keyChain, nac_validator, encryptionCert,
            attrAuthorityCertificate);
        nacProducer = std::make_unique<ndn::nacabe::CacheProducer>(
            m_face, m_keyChain, nac_validator, encryptionCert,
            attrAuthorityCertificate);
        // LocalMockTag is the deterministic unit-test boundary. Keep handlers
        // inline by default so selection callbacks have stable synchronous
        // postconditions. Integration fixtures that exercise production-like
        // dependency waits explicitly enable worker threads after construction.
        m_handlerPool.setThreadCount(0);
        m_ackPool.setThreadCount(0);
        m_fetchPool.setThreadCount(2);
        m_signingInfo = ndn::security::signingByCertificate(signingCert);
        // Opt-in durable runtime status (NDNSF_PERSIST_RUNTIME_STATE, FR-039):
        // re-verify and seed statuses accepted by an earlier process of this
        // identity, then confirm each against its Controller online.  The
        // LocalMock trust schema resolves synchronously.
        if (RuntimeStatusStore::enabled()) {
            m_runtimeStatusStore = std::make_unique<RuntimeStatusStore>(
                RuntimeStatusStore::defaultStorePath("provider", identity));
        }
        restorePersistedRuntimeStatuses();
    }

    void
    ServiceProvider::attachLocalMockPubSubForTest(
        std::shared_ptr<ndn::svs::SVSPubSub> pubSub)
    {
        if (pubSub == nullptr) {
            throw std::invalid_argument(
                "ServiceProvider LocalMock PubSub cannot be null");
        }
        if (m_svsps != nullptr) {
            throw std::logic_error(
                "ServiceProvider PubSub is already initialized");
        }
        installLocalMockDataIngressForTest();
        m_svsps = std::move(pubSub);
    }

    void
    ServiceProvider::installLocalMockDataIngressForTest()
    {
        if (m_svsps != nullptr) {
            throw std::logic_error(
                "LocalMock data ingress must be installed before PubSub attachment");
        }
        // LocalMock uses the real production onInterest/IMS path for exact
        // streamed-event retries.  Keep the same content filters that the
        // normal constructor installs; the SVS attachment alone only covers
        // publication notifications and cannot answer an exact Interest.
        const ndn::Name ndnsfFilter = ndn::Name(identity.toUri()).append("NDNSF");
        const ndn::Name ckFilter = ndn::Name(identity.toUri()).append("CK");
        const ndn::Name diDataFilter =
            ndn::Name(identity.toUri()).append("NDNSF-DI");
        const ndn::Name applicationDataFilter = ndn::Name(identity);
        auto registerContentFilter = [this](const ndn::Name& prefix) {
            auto holder = std::make_shared<ndn::ScopedRegisteredPrefixHandle>();
            m_contentRegistrations.push_back(holder);
            *holder = m_face.setInterestFilter(
                prefix,
                std::bind(&ServiceProvider::onInterest, this, _1, _2),
                std::bind(&ServiceProvider::onPrefixRegisterFailure, this, _1, _2));
        };
        registerContentFilter(ndnsfFilter);
        registerContentFilter(ckFilter);
        registerContentFilter(diDataFilter);
        registerContentFilter(applicationDataFilter);
    }

    void
    ServiceProvider::useSigningKeyChainForTest(ndn::KeyChain& keyChain)
    {
        // Fail at fixture setup instead of much later during publication.
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
    }

    bool
    ServiceProvider::isNacConsumerReadyForTest()
    {
        return activeNacConsumer().readyForDecryption();
    }

    void
    ServiceProvider::setStreamPublicationInterceptorForTest(
        StreamPublicationInterceptorForTest interceptor)
    {
        std::lock_guard<std::mutex> lock(m_streamPublicationInterceptorMutex);
        m_streamPublicationInterceptorForTest = std::move(interceptor);
    }

    void
    ServiceProvider::setStreamRetentionInterceptorForTest(
        StreamRetentionInterceptorForTest interceptor)
    {
        std::lock_guard<std::mutex> lock(m_streamPublicationInterceptorMutex);
        m_streamRetentionInterceptorForTest = std::move(interceptor);
    }

    void
    ServiceProvider::setStreamRetentionExpiryObserverForTest(
        StreamRetentionExpiryObserverForTest observer)
    {
        std::lock_guard<std::mutex> lock(m_streamPublicationInterceptorMutex);
        m_streamRetentionExpiryObserverForTest = std::move(observer);
    }

    void
    ServiceProvider::publishStreamPacketForTest(const ndn::Data& data)
    {
        if (!m_svsps) {
            throw std::runtime_error(
                "stream packet test publication requires production SVS");
        }
        boost::asio::post(m_face.getIoContext(),
            [pubSub = m_svsps, packet = ndn::Data(data)] () mutable {
                pubSub->publishPacket(packet);
            });
    }

    size_t
    ServiceProvider::streamPublisherHighWaterMarkForTest(
        const ndn::Name& requesterName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId)
    {
        const auto pendingKey = ndn::Name(requesterName)
            .append(serviceName).append(requestId);
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        const auto found = m_streamPublishers.find(pendingKey);
        return found == m_streamPublishers.end() ? 0 :
            found->second->highWaterMark();
    }

    void
    ServiceProvider::cacheHybridReceiveKeyForTest(const std::string& keyId,
                                                  const std::string& epochId,
                                                  const ndn::Buffer& key)
    {
        m_hybridMessageCrypto.cacheReceiveKey(keyId, epochId, key);
    }

    HybridMessageKey
    ServiceProvider::prepareHybridSendKeyForTest(
        const ndn::Name& serviceName,
        const std::string& messageType)
    {
        if (messageType != "ACK" && messageType != "RESPONSE") {
            throw std::invalid_argument(
                "LocalMock outbound Hybrid key must be ACK or RESPONSE");
        }
        const auto accessAttribute = std::string("/PERMISSION") +
                                      serviceName.toUri();
        auto key = m_hybridMessageCrypto.getOrCreateSendKey(
            serviceName,
            identity,
            accessAttribute,
            messageType,
            m_hybridCryptoCounters);
        m_hybridMessageCrypto.markSendKeyWrapped(key.keyId);
        return key;
    }

    void
    ServiceProvider::markHybridResponseKeyWrappedForTest(
        const ndn::Name& serviceName)
    {
        prepareHybridSendKeyForTest(serviceName, "RESPONSE");
    }

    void ServiceProvider::init()
    {
        registerServiceInfo();
        registerNDNSFMessages();
    }

    void ServiceProvider::registerContentFilters()
    {
        const ndn::Name ndnsfFilter = ndn::Name(identity.toUri()).append("NDNSF");
        const ndn::Name ckFilter = ndn::Name(identity.toUri()).append("CK");
        const ndn::Name diDataFilter =
            ndn::Name(identity.toUri()).append("NDNSF-DI");
        // Collaboration evidence is application-owned Data under the
        // producer namespace.  The identity filter is intentionally kept
        // alongside the framework filters so exact IMS retrieval uses the
        // same onInterest path.
        const ndn::Name applicationDataFilter = ndn::Name(identity);
        auto registerContentFilter = [this](const ndn::Name& prefix) {
            auto holder = std::make_shared<ndn::ScopedRegisteredPrefixHandle>();
            m_contentRegistrations.push_back(holder);
            *holder = m_face.setInterestFilter(
                prefix,
                std::bind(&ServiceProvider::onInterest, this, _1, _2),
                std::bind(&ServiceProvider::onPrefixRegisterFailure, this, _1, _2));
        };
        registerContentFilter(ndnsfFilter);
        registerContentFilter(ckFilter);
        registerContentFilter(diDataFilter);
        registerContentFilter(applicationDataFilter);
    }

    void ServiceProvider::scheduleSvsReinitializationAfterPermission()
    {
        if (m_isLocalMock || m_svsps == nullptr || m_svsReinitializationScheduled) {
            return;
        }
        m_svsReinitializationScheduled = true;
        // NFD authorizes registerPrefix at command time. The constructor
        // starts SVS before the asynchronous Controller permission response,
        // so a cold run can permanently lose its sync route. Recreate the
        // endpoint after the permission table is installed, on the Face
        // scheduler, and reattach all maintained subscriptions.
        m_scheduler.schedule(ndn::time::milliseconds(100), [this] {
            m_svsReinitializationScheduled = false;
            reinitializeSvsPubSubAfterPermission();
        });
    }

    void ServiceProvider::reinitializeSvsPubSubAfterPermission()
    {
        if (m_isLocalMock || m_svsps == nullptr) {
            return;
        }

        // The constructor's registrations may have been rejected by NFD
        // before the provider permission response.  Drop those handles before
        // retrying so a warm process cannot accumulate duplicate filters.
        m_contentRegistrations.clear();
        m_svsps.reset();

        ndn::svs::SecurityOptions secOpts(m_keyChain);
        secOpts.interestSigner = std::make_shared<CommandInterestSigner>(m_keyChain);
        secOpts.interestSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.interestSigner->signingInfo.setSigningKeyName(signingCert.getKeyName());
        secOpts.dataSigner->signingInfo.setSigningCertName(signingCert.getName());
        secOpts.dataSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.pubSigner->signingInfo.setSigningCertName(signingCert.getName());
        secOpts.pubSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.validator = validator;
        secOpts.encapsulatedDataValidator = validator;

        ndn::svs::SVSPubSubOptions opts;
        configureSvsProtocol(opts);
        opts.publicationFetchRetries =
            std::max(0, intEnvOrDefault("NDNSF_SVS_PUBLICATION_FETCH_RETRIES",
                                        opts.publicationFetchRetries));
        opts.publicationFetchInnerRetries =
            std::max(0, intEnvOrDefault("NDNSF_SVS_PUBLICATION_FETCH_INNER_RETRIES",
                                        opts.publicationFetchInnerRetries));
        opts.publicationFetchInterestLifetime = ndn::time::milliseconds(std::max(
            100, intEnvOrDefault("NDNSF_SVS_PUBLICATION_FETCH_LIFETIME_MS",
                                 static_cast<int>(opts.publicationFetchInterestLifetime.count()))));
        opts.publicationFetchFailureBackoff = ndn::time::milliseconds(std::max(
            0, intEnvOrDefault("NDNSF_SVS_PUBLICATION_FETCH_BACKOFF_MS",
                               static_cast<int>(opts.publicationFetchFailureBackoff.count()))));
        opts.publicationFetchMaxBackoff = ndn::time::milliseconds(std::max(
            0, intEnvOrDefault("NDNSF_SVS_PUBLICATION_FETCH_MAX_BACKOFF_MS",
                               static_cast<int>(opts.publicationFetchMaxBackoff.count()))));
        opts.publicationFetchWindow = static_cast<uint16_t>(std::max(
            1, intEnvOrDefault("NDNSF_SVS_PUBLICATION_FETCH_WINDOW",
                               adaptiveSvsPublicationFetchWindow(
                                   static_cast<int>(opts.publicationFetchWindow)))));

        ndn::Name nodeId(identity);
        nodeId.append("provider");
        const int sessionId = m_configManager.loadAndIncrement(
            m_groupPrefix.toUri(), nodeId.toUri());
        nodeId.append(std::to_string(sessionId));
        m_svsps = std::make_shared<ndn::svs::SVSPubSub>(
            m_groupPrefix,
            nodeId,
            m_face,
            std::bind(&ServiceProvider::onMissingData, this, _1),
            opts,
            secOpts);
        if (std::getenv("NDNSF_SVS_PERIODIC_SYNC_MS") != nullptr) {
            const int periodicSyncMs = std::max(
                1, intEnvOrDefault("NDNSF_SVS_PERIODIC_SYNC_MS", 30000));
            m_svsps->getSVSync().getCore().setPeriodicSyncTime(
                ndn::time::milliseconds(periodicSyncMs));
        }
        NDN_LOG_WARN("NDNSF_SVS_REINITIALIZED_AFTER_PERMISSION role=provider"
                     << " group=" << m_groupPrefix
                     << " provider=" << identity);
        registerNDNSFMessages();
        registerContentFilters();
    }

    ServiceProvider::~ServiceProvider()
    {
        // spec182: sever scoped-registration ownership first.  Under the
        // registration control lock the owner is cleared so a posted cleanup
        // closure can no longer reach this provider, and every owned state is
        // atomically closed so late dispatches observe a closed generation.
        // Only the atomic flags are touched here -- handler owners are not
        // destroyed under the lock, and the pool shutdowns below drain work
        // that can no longer publish through a live registration.
        {
            const std::lock_guard<std::mutex> lock(m_registrationControl->mutex);
            m_registrationControl->owner = nullptr;
            closeAllRegistrationStates();
        }
        m_fetchStopping->store(true);
        m_fetchPool.shutdown();
        if (m_svsps != nullptr) {
            const auto stats = m_svsps->getSVSync().getCore().getSyncProcessingStats();
            NDN_LOG_INFO("NDNSF_SVS_SYNC_STATS role=provider"
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
            NDN_LOG_INFO("NDNSF_SVS_DELIVERY_STATS role=provider"
                         << " malformed=" << rejection.malformedEnvelope
                         << " signaturePolicy=" << rejection.signaturePolicy
                         << " vectorDecode=" << rejection.vectorDecode
                         << " fetchStatsAbi=unavailable");
	        }
        m_cryptoProduceQueue.shutdown();
        m_ackPool.shutdown();
        m_handlerPool.shutdown();
    }

    void ServiceProvider::registerServiceInfo()
    {
        NDN_LOG_INFO("No provider service info registration configured for "
                     << identity.toUri());
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     AckStrategyHandler ackHandler,
                                     RequestHandler requestHandler)
    {
        addService(serviceName,
                   std::move(ackHandler),
                   std::move(requestHandler),
                   ServiceMode::Normal);
    }

    void ServiceProvider::addStreamingHandler(const ndn::Name& serviceName,
                                               StreamingHandler handler)
    {
        std::lock_guard<std::mutex> regLock(m_registrationControl->mutex);
        if (!allowLegacyServiceTakeover(serviceName)) {
            NDN_LOG_WARN("[ServiceProvider] legacy stream registration refused "
                         "over active scoped registration service="
                         << serviceName.toUri());
            return;
        }
        auto& service = m_services[serviceName];
        service.streamingHandler = std::move(handler);
        // A Normal streamed request still needs to pass the existing ACK
        // admission gate, which treats the presence of a normal handler as
        // service availability.  The real execution branch below dispatches
        // streamingHandler; this sentinel is never the application callback.
        if (!service.requestHandler) {
            service.requestHandler =
                [] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
                    const ndn::Name&, const RequestMessage&) {
                    ResponseMessage response;
                    response.setStatus(false);
                    response.setErrorInfo(
                        "streamed Normal dispatch requires the stream handler");
                    return response;
                };
        }
        // A streamed request can use either the normal ACK/Selection path or
        // the cached-token Targeted path.  The Targeted ingress gate requires
        // a registered targeted handler even though dispatchRequestExecutionAsync
        // invokes streamingHandler for the actual stream.  Install a small
        // sentinel so the existing authorization/token path recognizes the
        // streamed service without inventing a second application callback.
        if (!service.targetedRequestHandler) {
            service.targetedRequestHandler =
                [] (const ndn::Name&, const ndn::Name&, const ndn::Name&,
                    const ndn::Name&, const RequestMessage&) {
                    ResponseMessage response;
                    response.setStatus(false);
                    response.setErrorInfo(
                        "streamed Targeted dispatch requires the stream handler");
                    return response;
                };
        }
        service.mode = ServiceMode::Normal;
        const auto serviceUri = serviceName.toUri();
        if (std::find(m_serviceNames.begin(), m_serviceNames.end(), serviceUri) ==
            m_serviceNames.end()) {
            m_serviceNames.push_back(serviceUri);
        }
        NDN_LOG_WARN("[ServiceProvider] registered streamed service prefix="
                     << serviceUri);
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     AckStrategyHandler ackHandler,
                                     RequestHandler requestHandler,
                                     ServiceMode mode)
    {
        std::lock_guard<std::mutex> regLock(m_registrationControl->mutex);
        if (!allowLegacyServiceTakeover(serviceName)) {
            NDN_LOG_WARN("[ServiceProvider] legacy service registration refused "
                         "over active scoped registration service="
                         << serviceName.toUri());
            return;
        }
        auto& service = m_services[serviceName];
        if (mode == ServiceMode::Targeted) {
            service.targetedRequestHandler = std::move(requestHandler);
            if (!service.requestHandler) {
                service.mode = ServiceMode::Targeted;
            }
        }
        else {
            service.ackHandler = std::move(ackHandler);
            service.requestHandler = std::move(requestHandler);
            service.mode = ServiceMode::Normal;
        }
        const auto serviceUri = serviceName.toUri();
        if (std::find(m_serviceNames.begin(), m_serviceNames.end(), serviceUri) ==
            m_serviceNames.end()) {
            m_serviceNames.push_back(serviceUri);
        }
        NDN_LOG_WARN("[ServiceProvider] registered service prefix="
                  << serviceUri);
        NDN_LOG_WARN("Registered service handler for " << serviceUri
                     << " mode="
                     << (mode == ServiceMode::Targeted ? "Targeted" : "Normal"));
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     AckStrategyHandler ackHandler,
                                     RequestHandler requestHandler,
                                     ServiceInvocationMode invocationMode)
    {
        std::lock_guard<std::mutex> regLock(m_registrationControl->mutex);
        if (!allowLegacyServiceTakeover(serviceName)) {
            NDN_LOG_WARN("[ServiceProvider] legacy service registration refused "
                         "over active scoped registration service="
                         << serviceName.toUri());
            return;
        }
        auto& service = m_services[serviceName];
        if (invocationMode == ServiceInvocationMode::NormalOnly ||
            invocationMode == ServiceInvocationMode::NormalAndTargeted) {
            service.ackHandler = std::move(ackHandler);
            service.requestHandler = requestHandler;
            service.mode = ServiceMode::Normal;
        }
        if (invocationMode == ServiceInvocationMode::TargetedOnly ||
            invocationMode == ServiceInvocationMode::NormalAndTargeted) {
            service.targetedRequestHandler = std::move(requestHandler);
            if (invocationMode == ServiceInvocationMode::TargetedOnly) {
                service.mode = ServiceMode::Targeted;
            }
        }

        const auto serviceUri = serviceName.toUri();
        if (std::find(m_serviceNames.begin(), m_serviceNames.end(), serviceUri) ==
            m_serviceNames.end()) {
            m_serviceNames.push_back(serviceUri);
        }
        const char* modeText = "NormalOnly";
        if (invocationMode == ServiceInvocationMode::TargetedOnly) {
            modeText = "TargetedOnly";
        }
        else if (invocationMode == ServiceInvocationMode::NormalAndTargeted) {
            modeText = "NormalAndTargeted";
        }
        NDN_LOG_WARN("[ServiceProvider] registered service prefix="
                  << serviceUri);
        NDN_LOG_WARN("Registered service handler for " << serviceUri
                     << " invocation-mode=" << modeText);
    }

    void ServiceProvider::setSelectionStatusQueryable(const ndn::Name& serviceName,
                                                      bool enabled)
    {
        auto& service = m_services[serviceName];
        service.selectionStatusQueryable = enabled;
        const auto serviceUri = serviceName.toUri();
        if (std::find(m_serviceNames.begin(), m_serviceNames.end(), serviceUri) ==
            m_serviceNames.end()) {
            m_serviceNames.push_back(serviceUri);
        }
        NDN_LOG_WARN("Selection status query "
                     << (enabled ? "enabled" : "disabled")
                     << " for service " << serviceUri);
    }

    void ServiceProvider::setR1SelectionDecisionHandler(
        const ndn::Name& serviceName,
        R1SelectionDecisionHandler handler)
    {
        if (!handler) {
            m_r1SelectionDecisionHandlers.erase(serviceName);
            return;
        }
        m_r1SelectionDecisionHandlers[serviceName] = std::move(handler);
        setSelectionStatusQueryable(serviceName, true);
    }

    void ServiceProvider::setR1ReservationTerminalHandler(
        const ndn::Name& serviceName,
        R1ReservationTerminalHandler handler)
    {
        if (!handler) {
            m_r1ReservationTerminalHandlers.erase(serviceName);
            return;
        }
        m_r1ReservationTerminalHandlers[serviceName] = std::move(handler);
    }

    void ServiceProvider::setGenericSelectionTxnStore(
        std::shared_ptr<GenericSelectionTxnStore> store)
    {
        if (!store) {
            throw std::invalid_argument(
                "generic Selection transaction store is required");
        }
        m_genericSelectionTxnStore = std::move(store);
    }

    void ServiceProvider::registerOpaqueSelectionParticipant(
        const ndn::Name& serviceName,
        std::shared_ptr<OpaqueSelectionParticipant> participant)
    {
        if (serviceName.empty() || !participant ||
            participant->participantId().empty() ||
            participant->participantVersion() == 0) {
            throw std::invalid_argument(
                "opaque Selection participant registration is incomplete");
        }
        if (!m_genericSelectionTxnStore) {
            throw std::logic_error(
                "configure generic Selection transaction store first");
        }
        const auto existing =
            m_opaqueSelectionParticipants.find(serviceName);
        if (existing != m_opaqueSelectionParticipants.end() &&
            (existing->second->participantId() !=
                 participant->participantId() ||
             existing->second->participantVersion() !=
                 participant->participantVersion())) {
            throw std::logic_error(
                "opaque Selection participant registration conflicts");
        }
        m_opaqueSelectionParticipants[serviceName] = std::move(participant);
        setSelectionStatusQueryable(serviceName, true);
    }

    void ServiceProvider::ProviderAdmissionLeaseTable::grant(
        GenericAdmissionLease lease)
    {
        if (lease.leaseId.empty()) {
            throw std::invalid_argument("GenericAdmissionLease leaseId is required");
        }
        lease.consumed = false;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_leases[lease.leaseId] = std::move(lease);
    }

    ServiceProvider::GenericLeaseValidationResult
    ServiceProvider::ProviderAdmissionLeaseTable::consume(
        const std::string& leaseId,
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Buffer& resourceBindingProof,
        uint64_t nowMs)
    {
        GenericLeaseValidationResult result;
        result.leaseId = leaseId;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_leases.find(leaseId);
        if (it == m_leases.end()) {
            result.reasonCode = "LEASE_NOT_FOUND";
            return result;
        }
        auto& lease = it->second;
        if (lease.consumed) {
            result.reasonCode = "LEASE_ALREADY_CONSUMED";
            return result;
        }
        if (lease.expiresAtMs > 0 && nowMs > lease.expiresAtMs) {
            result.reasonCode = "LEASE_EXPIRED";
            return result;
        }
        if (!lease.requesterName.empty() && !lease.requesterName.equals(requesterName)) {
            result.reasonCode = "LEASE_REQUESTER_MISMATCH";
            return result;
        }
        if (!lease.providerName.empty() && !lease.providerName.equals(providerName)) {
            result.reasonCode = "LEASE_PROVIDER_MISMATCH";
            return result;
        }
        if (!lease.serviceName.empty() && !lease.serviceName.equals(serviceName)) {
            result.reasonCode = "LEASE_SERVICE_MISMATCH";
            return result;
        }
        if (!lease.resourceBindingProof.empty() &&
            !buffersEqual(lease.resourceBindingProof, resourceBindingProof)) {
            result.reasonCode = "LEASE_RESOURCE_BINDING_MISMATCH";
            return result;
        }
        lease.consumed = true;
        result.status = true;
        result.reasonCode = "OK";
        return result;
    }

    size_t ServiceProvider::ProviderAdmissionLeaseTable::size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_leases.size();
    }

    ndn::Buffer ServiceProvider::makeGenericAdmissionLeaseAckPayload(
        const GenericAdmissionLease& lease,
        const ndn::Buffer& servicePayload)
    {
        std::string payload;
        payload += "leaseId=" + lease.leaseId + ";";
        if (!lease.providerName.empty()) {
            payload += "leaseProvider=" + lease.providerName.toUri() + ";";
        }
        if (!lease.requesterName.empty()) {
            payload += "leaseRequester=" + lease.requesterName.toUri() + ";";
        }
        if (!lease.serviceName.empty()) {
            payload += "leaseService=" + lease.serviceName.toUri() + ";";
        }
        if (lease.expiresAtMs > 0) {
            payload += "leaseExpiresAtMs=" + std::to_string(lease.expiresAtMs) + ";";
        }
        if (!servicePayload.empty()) {
            payload += std::string(reinterpret_cast<const char*>(servicePayload.data()),
                                   servicePayload.size());
            if (!payload.empty() && payload.back() != ';') {
                payload.push_back(';');
            }
        }
        return bufferFromText(payload);
    }

    ndn::Buffer ServiceProvider::makePeerNetworkMetricPayload(
        const PeerNetworkMetric& metric)
    {
        std::string payload;
        if (!metric.srcPeer.empty()) {
            payload += "peerMetricSrc=" + metric.srcPeer.toUri() + ";";
        }
        if (!metric.dstPeer.empty()) {
            payload += "peerMetricDst=" + metric.dstPeer.toUri() + ";";
        }
        payload += "peerMetricRttMs=" + numberToText(metric.rttMs) + ";";
        payload += "peerMetricBandwidthMbps=" + numberToText(metric.bandwidthMbps) + ";";
        payload += "peerMetricLossRate=" + numberToText(metric.lossRate) + ";";
        payload += "peerMetricJitterMs=" + numberToText(metric.jitterMs) + ";";
        if (metric.observedAtMs > 0) {
            payload += "peerMetricObservedAtMs=" + std::to_string(metric.observedAtMs) + ";";
        }
        payload += "peerMetricConfidence=" + numberToText(metric.confidence) + ";";
        return bufferFromText(payload);
    }

    std::optional<ServiceProvider::PeerNetworkMetric>
    ServiceProvider::parsePeerNetworkMetricPayload(const ndn::Buffer& payload)
    {
        const auto fields = parseSemicolonFields(payload);
        if (fields.find("peerMetricSrc") == fields.end() ||
            fields.find("peerMetricDst") == fields.end()) {
            return std::nullopt;
        }
        PeerNetworkMetric metric;
        metric.srcPeer = nameFieldOrDefault(fields, "peerMetricSrc");
        metric.dstPeer = nameFieldOrDefault(fields, "peerMetricDst");
        metric.rttMs = doubleFieldOrDefault(fields, "peerMetricRttMs");
        metric.bandwidthMbps = doubleFieldOrDefault(fields, "peerMetricBandwidthMbps");
        metric.lossRate = doubleFieldOrDefault(fields, "peerMetricLossRate");
        metric.jitterMs = doubleFieldOrDefault(fields, "peerMetricJitterMs");
        metric.observedAtMs = uintFieldOrDefault(fields, "peerMetricObservedAtMs");
        metric.confidence = doubleFieldOrDefault(fields, "peerMetricConfidence", 1.0);
        return metric;
    }

    ndn::Buffer ServiceProvider::makeGenericAckMetadataPayload(
        const GenericAckMetadata& metadata)
    {
        std::string payload;
        if (metadata.runtimeHint) {
            const auto& hint = *metadata.runtimeHint;
            if (!hint.providerName.empty()) {
                payload += "runtimeProvider=" + hint.providerName.toUri() + ";";
            }
            payload += "runtimeQueueLength=" + std::to_string(hint.queueLength) + ";";
            payload += "runtimeEstimatedQueueWaitMs=" +
                       std::to_string(hint.estimatedQueueWaitMs) + ";";
            payload += "runtimeCpuUtilization=" + numberToText(hint.cpuUtilization) + ";";
            payload += "runtimeGpuUtilization=" + numberToText(hint.gpuUtilization) + ";";
            payload += "runtimeFreeMemoryMb=" + std::to_string(hint.freeMemoryMb) + ";";
            payload += "runtimeFreeGpuMemoryMb=" +
                       std::to_string(hint.freeGpuMemoryMb) + ";";
            if (!hint.peerMetrics.empty()) {
                const auto peerPayload = makePeerNetworkMetricPayload(hint.peerMetrics.front());
                payload += std::string(reinterpret_cast<const char*>(peerPayload.data()),
                                       peerPayload.size());
            }
        }
        if (!metadata.leaseOffers.empty()) {
            const auto leasePayload =
                makeGenericAdmissionLeaseAckPayload(metadata.leaseOffers.front());
            payload += std::string(reinterpret_cast<const char*>(leasePayload.data()),
                                   leasePayload.size());
        }
        if (!metadata.servicePayloadSchema.empty()) {
            payload += "servicePayloadSchema=" + metadata.servicePayloadSchema + ";";
        }
        if (!metadata.servicePayload.empty()) {
            payload += "servicePayload=";
            payload += std::string(reinterpret_cast<const char*>(metadata.servicePayload.data()),
                                   metadata.servicePayload.size());
            if (payload.back() != ';') {
                payload.push_back(';');
            }
        }
        return bufferFromText(payload);
    }

    ServiceProvider::GenericAckMetadata
    ServiceProvider::parseGenericAckMetadataPayload(const ndn::Buffer& payload)
    {
        GenericAckMetadata metadata;
        const auto fields = parseSemicolonFields(payload);
        if (fields.find("runtimeProvider") != fields.end()) {
            GenericProviderRuntimeHint hint;
            hint.providerName = nameFieldOrDefault(fields, "runtimeProvider");
            hint.queueLength = uintFieldOrDefault(fields, "runtimeQueueLength");
            hint.estimatedQueueWaitMs =
                uintFieldOrDefault(fields, "runtimeEstimatedQueueWaitMs");
            hint.cpuUtilization = doubleFieldOrDefault(fields, "runtimeCpuUtilization");
            hint.gpuUtilization = doubleFieldOrDefault(fields, "runtimeGpuUtilization");
            hint.freeMemoryMb = uintFieldOrDefault(fields, "runtimeFreeMemoryMb");
            hint.freeGpuMemoryMb = uintFieldOrDefault(fields, "runtimeFreeGpuMemoryMb");
            if (auto peerMetric = parsePeerNetworkMetricPayload(payload)) {
                hint.peerMetrics.push_back(*peerMetric);
            }
            metadata.runtimeHint = hint;
        }
        auto leaseIt = fields.find("leaseId");
        if (leaseIt != fields.end() && !leaseIt->second.empty()) {
            GenericAdmissionLease lease;
            lease.leaseId = leaseIt->second;
            lease.providerName = nameFieldOrDefault(fields, "leaseProvider");
            lease.requesterName = nameFieldOrDefault(fields, "leaseRequester");
            lease.serviceName = nameFieldOrDefault(fields, "leaseService");
            lease.expiresAtMs = uintFieldOrDefault(fields, "leaseExpiresAtMs");
            metadata.leaseOffers.push_back(std::move(lease));
        }
        if (const auto it = fields.find("servicePayloadSchema"); it != fields.end()) {
            metadata.servicePayloadSchema = it->second;
        }
        if (const auto it = fields.find("servicePayload"); it != fields.end()) {
            metadata.servicePayload = bufferFromText(it->second);
        }
        return metadata;
    }

    bool
    ServiceProvider::ProviderCapabilityHint::readyForNewRequest() const
    {
        std::string drain = drainState;
        std::transform(drain.begin(), drain.end(), drain.begin(),
                       [] (unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return ready &&
               (drain.empty() || drain == "ACTIVE" || drain == "READY");
    }

    ndn::Buffer
    ServiceProvider::makeDataProductReferencePayload(
        const DataProductReference& reference)
    {
        std::string payload;
        if (!reference.name.empty()) {
            payload += "dataProductName=" + reference.name.toUri() + ";";
        }
        if (!reference.producerName.empty()) {
            payload += "dataProductProducer=" + reference.producerName.toUri() + ";";
        }
        if (!reference.serviceName.empty()) {
            payload += "dataProductService=" + reference.serviceName.toUri() + ";";
        }
        if (!reference.objectClass.empty()) {
            payload += "dataProductObjectClass=" + reference.objectClass + ";";
        }
        if (!reference.contentType.empty()) {
            payload += "dataProductContentType=" + reference.contentType + ";";
        }
        if (!reference.digest.empty()) {
            payload += "dataProductDigest=" + reference.digest + ";";
        }
        if (reference.sizeBytes > 0) {
            payload += "dataProductSizeBytes=" + std::to_string(reference.sizeBytes) + ";";
        }
        if (reference.segmentCount > 0) {
            payload += "dataProductSegmentCount=" + std::to_string(reference.segmentCount) + ";";
        }
        if (reference.freshnessMs > 0) {
            payload += "dataProductFreshnessMs=" + std::to_string(reference.freshnessMs) + ";";
        }
        return bufferFromText(payload);
    }

    std::optional<ServiceProvider::DataProductReference>
    ServiceProvider::parseDataProductReferencePayload(const ndn::Buffer& payload)
    {
        const auto fields = parseSemicolonFields(payload);
        if (fields.find("dataProductName") == fields.end()) {
            return std::nullopt;
        }
        DataProductReference reference;
        reference.name = nameFieldOrDefault(fields, "dataProductName");
        reference.producerName = nameFieldOrDefault(fields, "dataProductProducer");
        reference.serviceName = nameFieldOrDefault(fields, "dataProductService");
        if (const auto it = fields.find("dataProductObjectClass"); it != fields.end()) {
            reference.objectClass = it->second;
        }
        if (const auto it = fields.find("dataProductContentType"); it != fields.end()) {
            reference.contentType = it->second;
        }
        if (const auto it = fields.find("dataProductDigest"); it != fields.end()) {
            reference.digest = it->second;
        }
        reference.sizeBytes = uintFieldOrDefault(fields, "dataProductSizeBytes");
        reference.segmentCount = uintFieldOrDefault(fields, "dataProductSegmentCount");
        reference.freshnessMs = uintFieldOrDefault(fields, "dataProductFreshnessMs");
        return reference;
    }

    ndn::Buffer
    ServiceProvider::makeServiceOperationStatusPayload(
        const ServiceOperationStatus& status)
    {
        std::string payload;
        if (!status.operationId.empty()) {
            payload += "operationId=" + status.operationId + ";";
        }
        if (!status.operation.empty()) {
            payload += "operation=" + status.operation + ";";
        }
        if (!status.serviceName.empty()) {
            payload += "operationService=" + status.serviceName.toUri() + ";";
        }
        if (!status.providerName.empty()) {
            payload += "operationProvider=" + status.providerName.toUri() + ";";
        }
        if (!status.requestId.empty()) {
            payload += "operationRequestId=" + status.requestId.toUri() + ";";
        }
        if (!status.role.empty()) {
            payload += "operationRole=" + status.role + ";";
        }
        payload += "operationAttempt=" + std::to_string(status.attempt) + ";";
        payload += "operationEpoch=" + std::to_string(status.epoch) + ";";
        payload += "operationSequence=" + std::to_string(status.sequence) + ";";
        payload += "operationState=" + status.state + ";";
        if (!status.reasonCode.empty()) {
            payload += "operationReasonCode=" + status.reasonCode + ";";
        }
        if (!status.message.empty()) {
            payload += "operationMessage=" + status.message + ";";
        }
        payload += "operationProgressKnown=" +
            std::string(status.progressKnown ? "1" : "0") + ";";
        payload += "operationProgress=" + numberToText(status.progress) + ";";
        if (status.retryAfterMs > 0) {
            payload += "operationRetryAfterMs=" + std::to_string(status.retryAfterMs) + ";";
        }
        if (status.createdAtMs > 0) {
            payload += "operationCreatedAtMs=" + std::to_string(status.createdAtMs) + ";";
        }
        if (status.updatedAtMs > 0) {
            payload += "operationUpdatedAtMs=" + std::to_string(status.updatedAtMs) + ";";
        }
        if (status.expiresAtMs > 0) {
            payload += "operationExpiresAtMs=" + std::to_string(status.expiresAtMs) + ";";
        }
        if (!status.detailsSchema.empty()) {
            payload += "operationDetailsSchema=" + status.detailsSchema + ";";
        }
        if (!status.detailsPayload.empty()) {
            if (status.detailsPayload.size() > 4096) {
                throw std::invalid_argument(
                    "service operation details exceed 4096 bytes");
            }
            payload += "operationDetailsHex=" + hexEncode(status.detailsPayload) + ";";
        }
        if (status.resultReference) {
            const auto referencePayload =
                makeDataProductReferencePayload(*status.resultReference);
            payload += std::string(reinterpret_cast<const char*>(referencePayload.data()),
                                   referencePayload.size());
        }
        return bufferFromText(payload);
    }

    std::optional<ServiceProvider::ServiceOperationStatus>
    ServiceProvider::parseServiceOperationStatusPayload(const ndn::Buffer& payload)
    {
        const auto fields = parseSemicolonFields(payload);
        if (fields.find("operationId") == fields.end() &&
            fields.find("operation") == fields.end()) {
            return std::nullopt;
        }
        ServiceOperationStatus status;
        if (const auto it = fields.find("operationId"); it != fields.end()) {
            status.operationId = it->second;
        }
        if (const auto it = fields.find("operation"); it != fields.end()) {
            status.operation = it->second;
        }
        status.serviceName = nameFieldOrDefault(fields, "operationService");
        status.providerName = nameFieldOrDefault(fields, "operationProvider");
        status.requestId = nameFieldOrDefault(fields, "operationRequestId");
        if (const auto it = fields.find("operationRole"); it != fields.end()) {
            status.role = it->second;
        }
        status.attempt = uintFieldOrDefault(fields, "operationAttempt");
        status.epoch = uintFieldOrDefault(fields, "operationEpoch");
        status.sequence = uintFieldOrDefault(fields, "operationSequence");
        // Legacy payloads omitted these monotonic fields.
        status.attempt = status.attempt == 0 ? 1 : status.attempt;
        status.epoch = status.epoch == 0 ? 1 : status.epoch;
        status.sequence = status.sequence == 0 ? 1 : status.sequence;
        if (const auto it = fields.find("operationState"); it != fields.end()) {
            status.state = it->second;
        }
        if (const auto it = fields.find("operationReasonCode"); it != fields.end()) {
            status.reasonCode = it->second;
        }
        if (const auto it = fields.find("operationMessage"); it != fields.end()) {
            status.message = it->second;
        }
        status.progressKnown = fields.find("operationProgressKnown") != fields.end() &&
                               fields.at("operationProgressKnown") == "1";
        status.progress = doubleFieldOrDefault(fields, "operationProgress");
        status.retryAfterMs = uintFieldOrDefault(fields, "operationRetryAfterMs");
        status.createdAtMs = uintFieldOrDefault(fields, "operationCreatedAtMs");
        status.updatedAtMs = uintFieldOrDefault(fields, "operationUpdatedAtMs");
        status.expiresAtMs = uintFieldOrDefault(fields, "operationExpiresAtMs");
        if (const auto it = fields.find("operationDetailsSchema"); it != fields.end()) {
            status.detailsSchema = it->second;
        }
        if (const auto it = fields.find("operationDetailsHex"); it != fields.end()) {
            status.detailsPayload = hexDecode(it->second);
            if (status.detailsPayload.size() > 4096) {
                return std::nullopt;
            }
        }
        if (status.progress < 0.0 || status.progress > 1.0) {
            return std::nullopt;
        }
        status.resultReference = parseDataProductReferencePayload(payload);
        return status;
    }

    ndn::Buffer
    ServiceProvider::makeProviderCapabilityHintPayload(
        const ProviderCapabilityHint& hint)
    {
        std::string payload;
        if (!hint.providerName.empty()) {
            payload += "capabilityProvider=" + hint.providerName.toUri() + ";";
        }
        if (!hint.serviceName.empty()) {
            payload += "capabilityService=" + hint.serviceName.toUri() + ";";
        }
        payload += std::string("capabilityReady=") + (hint.ready ? "1" : "0") + ";";
        if (!hint.drainState.empty()) {
            payload += "capabilityDrainState=" + hint.drainState + ";";
        }
        if (!hint.reasonCode.empty()) {
            payload += "capabilityReasonCode=" + hint.reasonCode + ";";
        }
        if (!hint.message.empty()) {
            payload += "capabilityMessage=" + hint.message + ";";
        }
        if (hint.runtimeHint || !hint.leaseOffers.empty() ||
            !hint.servicePayloadSchema.empty() || !hint.servicePayload.empty()) {
            GenericAckMetadata metadata;
            metadata.runtimeHint = hint.runtimeHint;
            metadata.leaseOffers = hint.leaseOffers;
            metadata.servicePayloadSchema = hint.servicePayloadSchema;
            metadata.servicePayload = hint.servicePayload;
            const auto metadataPayload = makeGenericAckMetadataPayload(metadata);
            payload += std::string(reinterpret_cast<const char*>(metadataPayload.data()),
                                   metadataPayload.size());
        }
        if (hint.operationStatus) {
            const auto statusPayload =
                makeServiceOperationStatusPayload(*hint.operationStatus);
            payload += std::string(reinterpret_cast<const char*>(statusPayload.data()),
                                   statusPayload.size());
        }
        return bufferFromText(payload);
    }

    std::optional<ServiceProvider::ProviderCapabilityHint>
    ServiceProvider::parseProviderCapabilityHintPayload(const ndn::Buffer& payload)
    {
        const auto fields = parseSemicolonFields(payload);
        if (fields.find("capabilityProvider") == fields.end() &&
            fields.find("capabilityService") == fields.end()) {
            return std::nullopt;
        }
        ProviderCapabilityHint hint;
        hint.providerName = nameFieldOrDefault(fields, "capabilityProvider");
        hint.serviceName = nameFieldOrDefault(fields, "capabilityService");
        const auto readyIt = fields.find("capabilityReady");
        hint.ready = readyIt == fields.end() ||
                     !(readyIt->second == "0" || readyIt->second == "false");
        if (const auto it = fields.find("capabilityDrainState"); it != fields.end()) {
            hint.drainState = it->second;
        }
        if (const auto it = fields.find("capabilityReasonCode"); it != fields.end()) {
            hint.reasonCode = it->second;
        }
        if (const auto it = fields.find("capabilityMessage"); it != fields.end()) {
            hint.message = it->second;
        }
        const auto metadata = parseGenericAckMetadataPayload(payload);
        hint.runtimeHint = metadata.runtimeHint;
        hint.leaseOffers = metadata.leaseOffers;
        hint.servicePayloadSchema = metadata.servicePayloadSchema;
        hint.servicePayload = metadata.servicePayload;
        hint.operationStatus = parseServiceOperationStatusPayload(payload);
        return hint;
    }

    void ServiceProvider::setGenericAdmissionLeaseValidator(
        const ndn::Name& serviceName,
        GenericAdmissionLeaseValidator validator,
        bool required)
    {
        auto& service = m_services[serviceName];
        service.genericAdmissionLeaseValidator = std::move(validator);
        service.genericAdmissionLeaseRequired = required;
        NDN_LOG_WARN("Generic admission lease validation "
                     << (required ? "required" : "optional")
                     << " for service " << serviceName.toUri());
    }

    void ServiceProvider::setGenericAdmissionLeaseRequired(
        const ndn::Name& serviceName,
        bool required)
    {
        auto& service = m_services[serviceName];
        service.genericAdmissionLeaseRequired = required;
        NDN_LOG_WARN("Generic admission lease validation "
                     << (required ? "required" : "disabled")
                     << " for service " << serviceName.toUri());
    }

    void ServiceProvider::grantGenericAdmissionLease(GenericAdmissionLease lease)
    {
        m_genericAdmissionLeases.grant(std::move(lease));
    }

    ServiceProvider::GenericLeaseValidationResult
    ServiceProvider::validateGenericAdmissionLeaseForSelection(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage,
        const ServiceSelectionMessage& selectionMessage,
        const ndn::Buffer& assignmentPayload)
    {
        GenericLeaseValidationResult result;
        result.status = true;
        result.reasonCode = "NOT_REQUIRED";
        auto serviceIt = m_services.find(serviceName);
        if (serviceIt == m_services.end() ||
            !serviceIt->second.genericAdmissionLeaseRequired) {
            return result;
        }

        const GenericAdmissionLeaseValidationRequest request{
            requesterName,
            providerName,
            serviceName,
            requestId,
            requestMessage,
            selectionMessage,
            assignmentPayload,
        };
        if (serviceIt->second.genericAdmissionLeaseValidator) {
            result = serviceIt->second.genericAdmissionLeaseValidator(request);
            if (result.reasonCode.empty()) {
                result.reasonCode = result.status ? "OK" : "LEASE_REJECTED";
            }
            return result;
        }

        const auto fields = parseSemicolonFields(assignmentPayload);
        auto leaseIt = fields.find("leaseId");
        if (leaseIt == fields.end()) {
            leaseIt = fields.find("admissionLeaseId");
        }
        if (leaseIt == fields.end()) {
            leaseIt = fields.find("genericAdmissionLeaseId");
        }
        if (leaseIt == fields.end() || leaseIt->second.empty()) {
            result.status = false;
            result.reasonCode = "LEASE_ID_MISSING";
            return result;
        }
        auto proofIt = fields.find("resourceBindingProof");
        if (proofIt == fields.end()) {
            proofIt = fields.find("leaseResourceBinding");
        }
        const ndn::Buffer proof =
            proofIt == fields.end() ? ndn::Buffer() : bufferFromText(proofIt->second);
        return m_genericAdmissionLeases.consume(leaseIt->second,
                                                requesterName,
                                                providerName,
                                                serviceName,
                                                proof,
                                                nowMilliseconds());
    }

    void ServiceProvider::publishServiceInfo(
        const ndn::Name& serviceName,
        int serviceLifetimeSeconds,
        std::map<std::string, std::string> serviceMetaInfo)
    {
        if (!m_ServiceDiscovery.isEnabled()) {
            NDN_LOG_DEBUG("[ServiceProvider] NDNSD disabled; skip service info publish for "
                          << serviceName);
            return;
        }
        ndnsd::discovery::Details details;
        details.serviceName = serviceName;
        details.applicationPrefix = identity;
        details.serviceLifetime = serviceLifetimeSeconds;
        details.publishTimestamp = std::time(nullptr);
        details.serviceMetaInfo = std::move(serviceMetaInfo);
        m_ServiceDiscovery.publishServiceDetail(details);
        NDN_LOG_INFO("[ServiceProvider] NDNSD service info published identity="
                     << identity << " service=" << serviceName
                     << " lifetime=" << serviceLifetimeSeconds);
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     LegacyAckStrategyHandler ackHandler,
                                     RequestHandler requestHandler)
    {
        addService(serviceName,
                   wrapLegacyAckStrategyHandler(std::move(ackHandler)),
                   std::move(requestHandler));
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     RequestHandler requestHandler)
    {
        addService(serviceName, AckStrategyHandler{}, std::move(requestHandler));
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     AckStrategyHandler ackHandler,
                                     SimpleRequestHandler requestHandler)
    {
        addService(serviceName,
                   std::move(ackHandler),
                   [handler = std::move(requestHandler)](
                       const ndn::Name&,
                       const ndn::Name&,
                       const ndn::Name&,
                       const ndn::Name&,
                       const RequestMessage& requestMessage) {
                       return handler(requestMessage);
                   });
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     AckStrategyHandler ackHandler,
                                     SimpleRequestHandler requestHandler,
                                     ServiceInvocationMode invocationMode)
    {
        addService(serviceName,
                   std::move(ackHandler),
                   [handler = std::move(requestHandler)](
                       const ndn::Name&,
                       const ndn::Name&,
                       const ndn::Name&,
                       const ndn::Name&,
                       const RequestMessage& requestMessage) {
                       return handler(requestMessage);
                   },
                   invocationMode);
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     LegacyAckStrategyHandler ackHandler,
                                     SimpleRequestHandler requestHandler)
    {
        addService(serviceName,
                   wrapLegacyAckStrategyHandler(std::move(ackHandler)),
                   std::move(requestHandler));
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     SimpleAckStrategyHandler ackHandler,
                                     RequestHandler requestHandler)
    {
        AckStrategyHandler wrappedAckHandler;
        if (ackHandler) {
            wrappedAckHandler = [handler = std::move(ackHandler)](
                                    const RequestMessage& requestMessage) {
                AckDecision decision;
                decision.status = handler(requestMessage);
                decision.message = decision.status ? "Permission Granted" : "Permission Denied";
                return decision;
            };
        }

        addService(serviceName, std::move(wrappedAckHandler), std::move(requestHandler));
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     SimpleAckStrategyHandler ackHandler,
                                     SimpleRequestHandler requestHandler)
    {
        addService(serviceName,
                   std::move(ackHandler),
                   [handler = std::move(requestHandler)](
                       const ndn::Name&,
                       const ndn::Name&,
                       const ndn::Name&,
                       const ndn::Name&,
                       const RequestMessage& requestMessage) {
                       return handler(requestMessage);
                   });
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     SimpleAckStrategyHandler ackHandler,
                                     SimpleRequestHandler requestHandler,
                                     ServiceInvocationMode invocationMode)
    {
        AckStrategyHandler wrappedAckHandler;
        if (ackHandler) {
            wrappedAckHandler = [handler = std::move(ackHandler)](
                                    const RequestMessage& requestMessage) {
                AckDecision decision;
                decision.status = handler(requestMessage);
                decision.message = decision.status ? "Permission Granted" : "Permission Denied";
                return decision;
            };
        }

        addService(serviceName,
                   std::move(wrappedAckHandler),
                   std::move(requestHandler),
                   invocationMode);
    }

    void ServiceProvider::addTargetedService(const ndn::Name& serviceName,
                                             RequestHandler requestHandler)
    {
        std::lock_guard<std::mutex> regLock(m_registrationControl->mutex);
        if (!allowLegacyServiceTakeover(serviceName)) {
            NDN_LOG_WARN("[ServiceProvider] legacy targeted registration refused "
                         "over active scoped registration service="
                         << serviceName.toUri());
            return;
        }
        auto& service = m_services[serviceName];
        service.targetedRequestHandler = std::move(requestHandler);
        if (!service.requestHandler) {
            service.mode = ServiceMode::Targeted;
        }
        const auto serviceUri = serviceName.toUri();
        if (std::find(m_serviceNames.begin(), m_serviceNames.end(), serviceUri) ==
            m_serviceNames.end()) {
            m_serviceNames.push_back(serviceUri);
        }
        NDN_LOG_WARN("[ServiceProvider] registered service prefix="
                  << serviceUri);
        NDN_LOG_WARN("Registered service handler for " << serviceUri
                     << " mode=Targeted");
    }

    void ServiceProvider::addTargetedService(const ndn::Name& serviceName,
                                             SimpleRequestHandler requestHandler)
    {
        addTargetedService(
            serviceName,
            [handler = std::move(requestHandler)](const ndn::Name&,
                                                  const ndn::Name&,
                                                  const ndn::Name&,
                                                  const ndn::Name&,
                                                  const RequestMessage& requestMessage) {
                return handler(requestMessage);
            });
    }

    void ServiceProvider::addCollaborationHandler(const ndn::Name& serviceName,
                                                  AckStrategyHandler ackHandler,
                                                  CollaborationHandler handler)
    {
        addCollaborationHandler(serviceName,
                                std::vector<CollaborationRole>{},
                                std::move(ackHandler),
                                std::move(handler));
    }

    void ServiceProvider::addCollaborationHandler(const ndn::Name& serviceName,
                                                  std::vector<CollaborationRole> allowedRoles,
                                                  AckStrategyHandler ackHandler,
                                                  CollaborationHandler handler)
    {
        std::lock_guard<std::mutex> regLock(m_registrationControl->mutex);
        // A live scoped registration of either kind owns this service name;
        // legacy collaboration registration must not overwrite it.  A closed
        // parked state is taken over by the whole-value replacement below
        // (the state pointer is dropped from the new record).
        auto& collaboration = m_collaborationServices[serviceName];
        if (collaboration.registrationState != nullptr &&
            !collaboration.registrationState->closed) {
            NDN_LOG_WARN("[ServiceProvider] legacy collaboration registration "
                         "refused over active scoped registration service="
                         << serviceName.toUri());
            return;
        }
        {
            auto servicesIt = m_services.find(serviceName);
            if (servicesIt != m_services.end() &&
                servicesIt->second.registrationState != nullptr &&
                !servicesIt->second.registrationState->closed) {
                NDN_LOG_WARN("[ServiceProvider] legacy collaboration registration "
                             "refused over active scoped service service="
                             << serviceName.toUri());
                return;
            }
        }
        m_collaborationServices[serviceName] =
            {std::move(ackHandler), std::move(handler), std::move(allowedRoles),
             nullptr};
        // Collaboration work is long-running by design. Registration therefore
        // enables the existing signed SELECTION-STATUS path by default.
        m_services[serviceName].selectionStatusQueryable = true;
        const auto serviceUri = serviceName.toUri();
        if (std::find(m_serviceNames.begin(), m_serviceNames.end(), serviceUri) ==
            m_serviceNames.end()) {
            m_serviceNames.push_back(serviceUri);
        }
        NDN_LOG_INFO("Registered collaboration handler for " << serviceUri);
    }

    void ServiceProvider::addCollaborationHandler(const ndn::Name& serviceName,
                                                  CollaborationHandler handler)
    {
        addCollaborationHandler(serviceName,
                                AckStrategyHandler{},
                                std::move(handler));
    }

    void ServiceProvider::addCollaborationHandler(const ndn::Name& serviceName,
                                                  std::vector<CollaborationRole> allowedRoles,
                                                  CollaborationHandler handler)
    {
        addCollaborationHandler(serviceName,
                                std::move(allowedRoles),
                                AckStrategyHandler{},
                                std::move(handler));
    }

    // ---- spec182 scoped registration ----
    //
    // A scoped registration binds a non-zero generation to the Core entry
    // through a RegistrationState shared with pending-request and
    // collaboration-selection binds (added by later lifecycle tasks).  All
    // registration-path code below runs on the Face event thread and
    // serializes against the Provider destructor through
    // m_registrationControl->mutex.  Handler owners moved out of the maps
    // during cleanup are destroyed only after that lock is released, so
    // application-side destructors may safely re-enter registration or
    // close().

    ServiceProvider::ServiceRegistration::ServiceRegistration(
        std::shared_ptr<RegistrationState> state,
        std::weak_ptr<RegistrationControl> control)
        : m_state(std::move(state)), m_control(std::move(control))
    {
    }

    ServiceProvider::ServiceRegistration::ServiceRegistration(
        ServiceRegistration&& other) noexcept
        : m_state(std::move(other.m_state)), m_control(std::move(other.m_control))
    {
    }

    ServiceProvider::ServiceRegistration&
    ServiceProvider::ServiceRegistration::operator=(
        ServiceRegistration&& other) noexcept
    {
        if (this != &other) {
            close();
            m_state = std::move(other.m_state);
            m_control = std::move(other.m_control);
        }
        return *this;
    }

    ServiceProvider::ServiceRegistration::~ServiceRegistration()
    {
        close();
    }

    void
    ServiceProvider::ServiceRegistration::close() noexcept
    {
        auto state = std::move(m_state);
        m_state.reset();
        if (state == nullptr) {
            return;
        }
        // Closing becomes observable by any dispatch thread the moment the
        // atomic flips; only the Core-entry cleanup below is Face-bound.
        state->closed.store(true);
        auto control = m_control.lock();
        if (control == nullptr) {
            return; // provider is gone; its destructor already closed the state
        }
        std::lock_guard<std::mutex> lock(control->mutex);
        if (control->owner == nullptr) {
            return; // provider destruction closed the state and is tearing
                    // down its own entries; no post is possible or needed
        }
        // The cleanup closure re-checks ownership under the same lock, so a
        // provider destroyed before the post runs turns the closure into a
        // no-op instead of a use-after-free.
        ServiceProvider* owner = control->owner;
        boost::asio::post(owner->m_face.getIoContext(),
                          [control, state]() {
                              RegisteredService retiredService;
                              RegisteredCollaborationService retiredCollaboration;
                              {
                                  std::lock_guard<std::mutex> lock(control->mutex);
                                  if (control->owner == nullptr) {
                                      return;
                                  }
                                  control->owner->detachClosedRegistration(
                                      state, retiredService, retiredCollaboration);
                              }
                              // Handler owners are destroyed here, strictly
                              // after the control lock is released.
                          });
    }

    bool
    ServiceProvider::ServiceRegistration::closed() const noexcept
    {
        // An empty (moved-from or already closed) handle reports closed.
        return m_state == nullptr || m_state->closed.load();
    }

    bool
    ServiceProvider::ServiceRegistration::valid() const noexcept
    {
        return m_state != nullptr && !m_state->closed.load();
    }

    uint64_t
    ServiceProvider::ServiceRegistration::generation() const noexcept
    {
        return m_state == nullptr ? 0 : m_state->generation;
    }

    ndn::Name
    ServiceProvider::ServiceRegistration::serviceName() const
    {
        return m_state == nullptr ? ndn::Name() : m_state->serviceName;
    }

    uint64_t
    ServiceProvider::allocateRegistrationGeneration()
    {
        if (m_registrationGeneration == std::numeric_limits<uint64_t>::max()) {
            throw std::runtime_error(
                "scoped registration generation space exhausted");
        }
        return ++m_registrationGeneration;
    }

    bool
    ServiceProvider::allowLegacyServiceTakeover(const ndn::Name& serviceName)
    {
        auto it = m_services.find(serviceName);
        if (it == m_services.end() || it->second.registrationState == nullptr) {
            return true;
        }
        auto& state = it->second.registrationState;
        if (!state->closed) {
            return false;
        }
        // The closed record's cleanup post either already ran or will never
        // run (loop stopped / provider gone).  Legacy registration takes the
        // entry over by dropping the parked state; the state-identity check
        // in detachClosedRegistration keeps a late cleanup from erasing this
        // legacy record.
        state.reset();
        return true;
    }

    void
    ServiceProvider::closeAllRegistrationStates()
    {
        for (auto& item : m_services) {
            if (item.second.registrationState != nullptr) {
                item.second.registrationState->closed.store(true);
            }
        }
        for (auto& item : m_collaborationServices) {
            if (item.second.registrationState != nullptr) {
                item.second.registrationState->closed.store(true);
            }
        }
    }

    void
    ServiceProvider::drainClosedRegistrations(
        std::vector<RegisteredService>& retiredServices,
        std::vector<RegisteredCollaborationService>& retiredCollaborations)
    {
        // Collect first: detach erases map entries while it runs.  The same
        // state can appear twice (collaboration shell + collaboration
        // record); detach is idempotent per state.
        std::vector<std::shared_ptr<RegistrationState>> closedStates;
        for (const auto& item : m_services) {
            if (item.second.registrationState != nullptr &&
                item.second.registrationState->closed) {
                closedStates.push_back(item.second.registrationState);
            }
        }
        for (const auto& item : m_collaborationServices) {
            if (item.second.registrationState != nullptr &&
                item.second.registrationState->closed) {
                closedStates.push_back(item.second.registrationState);
            }
        }
        for (const auto& state : closedStates) {
            RegisteredService retiredService;
            RegisteredCollaborationService retiredCollaboration;
            detachClosedRegistration(state, retiredService, retiredCollaboration);
            // Moving (possibly empty) records into the out-vectors destroys
            // nothing; the caller releases the lock before the vectors do.
            retiredServices.push_back(std::move(retiredService));
            retiredCollaborations.push_back(std::move(retiredCollaboration));
        }
    }

    void
    ServiceProvider::detachClosedRegistration(
        const std::shared_ptr<RegistrationState>& state,
        RegisteredService& retiredService,
        RegisteredCollaborationService& retiredCollaboration)
    {
        // Runs Face-serialized with the control lock held (drain or a posted
        // close closure).  An entry is erased only while it still owns this
        // very state object, so a stale handle from an old generation can
        // never delete the successor registration that reused the name, and
        // the m_serviceNames entry is dropped only when no other record
        // (legacy or scoped) still owns the name.
        auto serviceIt = m_services.find(state->serviceName);
        if (serviceIt != m_services.end() &&
            serviceIt->second.registrationState.get() == state.get()) {
            retiredService = std::move(serviceIt->second);
            m_services.erase(serviceIt);
        }
        auto collabIt = m_collaborationServices.find(state->serviceName);
        if (collabIt != m_collaborationServices.end() &&
            collabIt->second.registrationState.get() == state.get()) {
            retiredCollaboration = std::move(collabIt->second);
            m_collaborationServices.erase(collabIt);
        }
        if (m_services.find(state->serviceName) == m_services.end() &&
            m_collaborationServices.find(state->serviceName) ==
                m_collaborationServices.end()) {
            const auto serviceUri = state->serviceName.toUri();
            const auto nameIt = std::find(m_serviceNames.begin(),
                                          m_serviceNames.end(), serviceUri);
            if (nameIt != m_serviceNames.end()) {
                m_serviceNames.erase(nameIt);
            }
        }
    }

    bool
    ServiceProvider::serviceEntryBusy(const RegisteredService& entry)
    {
        return entry.registrationState != nullptr || entry.ackHandler ||
               entry.requestHandler || entry.targetedRequestHandler ||
               entry.streamingHandler || entry.selectionStatusQueryable ||
               entry.genericAdmissionLeaseRequired ||
               entry.genericAdmissionLeaseValidator != nullptr;
    }

    ServiceProvider::ServiceRegistration
    ServiceProvider::addScopedService(const ndn::Name& serviceName,
                                      AckStrategyHandler ackHandler,
                                      RequestHandler requestHandler,
                                      ServiceInvocationMode invocationMode)
    {
        if (m_registrationControl == nullptr) {
            throw std::logic_error(
                "scoped service registration requires a constructed provider");
        }
        std::vector<RegisteredService> retiredServices;
        std::vector<RegisteredCollaborationService> retiredCollaborations;
        std::shared_ptr<RegistrationState> state;
        {
            std::lock_guard<std::mutex> lock(m_registrationControl->mutex);
            drainClosedRegistrations(retiredServices, retiredCollaborations);
            const auto it = m_services.find(serviceName);
            if ((it != m_services.end() && serviceEntryBusy(it->second)) ||
                m_collaborationServices.find(serviceName) !=
                    m_collaborationServices.end()) {
                throw std::logic_error(
                    "scoped service name is already registered: " +
                    serviceName.toUri());
            }
            state = std::make_shared<RegistrationState>(
                serviceName, allocateRegistrationGeneration());
            auto& service = m_services[serviceName];
            // Mirror the legacy addService(ServiceInvocationMode) field
            // layout exactly; the busy check above guarantees a fresh record.
            if (invocationMode == ServiceInvocationMode::NormalOnly ||
                invocationMode == ServiceInvocationMode::NormalAndTargeted) {
                service.ackHandler = std::move(ackHandler);
                service.requestHandler = requestHandler;
                service.mode = ServiceMode::Normal;
            }
            if (invocationMode == ServiceInvocationMode::TargetedOnly ||
                invocationMode == ServiceInvocationMode::NormalAndTargeted) {
                service.targetedRequestHandler = std::move(requestHandler);
                if (invocationMode == ServiceInvocationMode::TargetedOnly) {
                    service.mode = ServiceMode::Targeted;
                }
            }
            service.registrationState = state;
            const auto serviceUri = serviceName.toUri();
            if (std::find(m_serviceNames.begin(), m_serviceNames.end(),
                          serviceUri) == m_serviceNames.end()) {
                m_serviceNames.push_back(serviceUri);
            }
            registerRequestSubscription(serviceName);
            NDN_LOG_WARN("[ServiceProvider] registered scoped service prefix="
                         << serviceUri
                         << " generation=" << state->generation);
        }
        return ServiceRegistration(std::move(state), m_registrationControl);
    }

    ServiceProvider::ServiceRegistration
    ServiceProvider::addScopedService(const ndn::Name& serviceName,
                                      AckStrategyHandler ackHandler,
                                      RequestHandler requestHandler,
                                      ServiceMode mode)
    {
        // A scoped registration is guaranteed fresh, so the legacy
        // ServiceMode field merge collapses onto the two invocation modes.
        return addScopedService(
            serviceName, std::move(ackHandler), std::move(requestHandler),
            mode == ServiceMode::Targeted ? ServiceInvocationMode::TargetedOnly
                                          : ServiceInvocationMode::NormalOnly);
    }

    ServiceProvider::ServiceRegistration
    ServiceProvider::addScopedCollaborationHandler(
        const ndn::Name& serviceName,
        std::vector<CollaborationRole> allowedRoles,
        AckStrategyHandler ackHandler,
        CollaborationHandler handler)
    {
        if (m_registrationControl == nullptr) {
            throw std::logic_error(
                "scoped collaboration registration requires a constructed "
                "provider");
        }
        std::vector<RegisteredService> retiredServices;
        std::vector<RegisteredCollaborationService> retiredCollaborations;
        std::shared_ptr<RegistrationState> state;
        {
            std::lock_guard<std::mutex> lock(m_registrationControl->mutex);
            drainClosedRegistrations(retiredServices, retiredCollaborations);
            const auto it = m_services.find(serviceName);
            if (m_collaborationServices.find(serviceName) !=
                    m_collaborationServices.end() ||
                (it != m_services.end() && serviceEntryBusy(it->second))) {
                throw std::logic_error(
                    "scoped collaboration service name is already registered: "
                    + serviceName.toUri());
            }
            state = std::make_shared<RegistrationState>(
                serviceName, allocateRegistrationGeneration());
            RegisteredCollaborationService record;
            record.ackHandler = std::move(ackHandler);
            record.handler = std::move(handler);
            record.allowedRoles = std::move(allowedRoles);
            record.registrationState = state;
            m_collaborationServices[serviceName] = std::move(record);
            // The collaboration record shares its name with the m_services
            // shell used by the Selection path; the shell mirrors the state
            // so one scoped close cleans up both records in a single detach.
            auto& shell = m_services[serviceName];
            shell.registrationState = state;
            shell.selectionStatusQueryable = true;
            const auto serviceUri = serviceName.toUri();
            if (std::find(m_serviceNames.begin(), m_serviceNames.end(),
                          serviceUri) == m_serviceNames.end()) {
                m_serviceNames.push_back(serviceUri);
            }
            registerRequestSubscription(serviceName);
            NDN_LOG_WARN("[ServiceProvider] registered scoped collaboration "
                         "service prefix="
                         << serviceUri
                         << " generation=" << state->generation);
        }
        return ServiceRegistration(std::move(state), m_registrationControl);
    }

    ServiceProvider::CollaborationContext::CollaborationContext(
        ServiceProvider& provider,
        ndn::Name requesterName,
        ndn::Name requestId,
        RequestMessage requestMessage,
        CollaborationAssignment assignment)
        : m_provider(provider)
        , m_requesterName(std::move(requesterName))
        , m_requestId(std::move(requestId))
        , m_requestMessage(std::move(requestMessage))
        , m_assignment(std::move(assignment))
    {
        // A CollaborationAssignment is the authority for the request-scoped
        // data keys visible to this role.  Production selection normally
        // installs the same keys before constructing the context, but keeping
        // this invariant at the context boundary also makes delayed/context
        // callbacks safe and prevents a valid assignment from becoming a
        // missing-key publish/fetch failure.
        if (!m_assignment.scopeKeys.empty()) {
            std::lock_guard<std::mutex> lock(m_provider.m_collaborationMutex);
            auto& scopeKeys = m_provider.m_collaborationScopeKeysByRequest[m_requestId];
            for (const auto& entry : m_assignment.scopeKeys) {
                if (entry.second.size() == HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                    scopeKeys[entry.first] = entry.second;
                }
            }
        }
    }

    SessionId ServiceProvider::CollaborationContext::sessionId() const
    {
        return m_requestId.toUri();
    }

    ndn::Name ServiceProvider::CollaborationContext::requesterName() const
    {
        return m_requesterName;
    }

    CollaborationRole ServiceProvider::CollaborationContext::role() const
    {
        return m_assignment.role;
    }

    ndn::Name ServiceProvider::CollaborationContext::localProvider() const
    {
        return m_provider.identity;
    }

    const ServiceProvider::CollaborationAssignment&
    ServiceProvider::CollaborationContext::assignment() const
    {
        return m_assignment;
    }

    bool ServiceProvider::CollaborationContext::hasArtifact(const ndn::Name& artifactName) const
    {
        std::lock_guard<std::mutex> lock(m_provider.m_collaborationMutex);
        if (m_provider.m_collaborationArtifacts.count(
                m_assignment.assignedArtifact.toUri()) != 0) {
            return true;
        }
        return !m_assignment.artifactPayload.empty() &&
               !m_assignment.assignedArtifact.empty() &&
               m_assignment.assignedArtifact.equals(artifactName);
    }

    bool ServiceProvider::CollaborationContext::fetchArtifact(const ndn::Name& artifactName, int)
    {
        {
            std::lock_guard<std::mutex> lock(m_provider.m_collaborationMutex);
            if (m_provider.m_collaborationArtifacts.count(artifactName.toUri()) != 0) {
                return true;
            }
            if (m_assignment.assignedArtifact.equals(artifactName) &&
                !m_assignment.artifactPayload.empty()) {
                m_provider.m_collaborationArtifacts[artifactName.toUri()] =
                    m_assignment.artifactPayload;
                return true;
            }
        }

        if (!m_assignment.assignedArtifact.equals(artifactName) ||
            m_assignment.artifactDataName.empty()) {
            return false;
        }

        NDN_LOG_ERROR("Collaboration artifact " << artifactName.toUri()
                      << " was not prefetched before handler execution");
        return false;
    }

    std::optional<ndn::Buffer>
    ServiceProvider::CollaborationContext::getArtifact(const ndn::Name& artifactName) const
    {
        std::lock_guard<std::mutex> lock(m_provider.m_collaborationMutex);
        auto it = m_provider.m_collaborationArtifacts.find(artifactName.toUri());
        if (it == m_provider.m_collaborationArtifacts.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<ndn::Buffer>
    ServiceProvider::CollaborationContext::fetchEncryptedLargeData(
        const ndn::Name& dataName,
        const ndn::Name& serviceName)
    {
        auto result = m_provider.fetchAndDecryptLargeData(
            dataName,
            serviceName.empty() ? m_assignment.service.toUri() : serviceName.toUri());
        if (!result.success) {
            NDN_LOG_ERROR("Failed to fetch encrypted large Data "
                          << dataName.toUri() << ": " << result.errorMessage);
            return std::nullopt;
        }
        return ndn::Buffer(result.plaintext.begin(), result.plaintext.end());
    }

    void ServiceProvider::CollaborationContext::fail(const std::string& reason)
    {
        NDN_LOG_ERROR("Collaboration role " << m_assignment.role
                      << " failed: " << reason);
        m_provider.updateSelectionExecutionStatus(
            m_assignment.selectionDigest,
            SelectionExecutionState::Failed,
            m_provider.identity,
            m_assignment.service,
            m_requestId,
            reason);
    }

    void ServiceProvider::CollaborationContext::publish(
        KeyScope keyScope,
        Topic topic,
        const ndn::Buffer& payload)
    {
        m_provider.publishCollaborationData(m_requesterName,
                                            m_requestId,
                                            m_assignment.role,
                                            keyScope,
                                            topic,
                                            payload);
    }

    ndn::Name ServiceProvider::CollaborationContext::publishLarge(
        KeyScope keyScope,
        Topic topic,
        const ndn::Buffer& payload,
        size_t maxSegmentSize,
        int freshnessMs)
    {
        return m_provider.publishCollaborationLargeData(m_requesterName,
                                                        m_requestId,
                                                        m_assignment.role,
                                                        std::move(keyScope),
                                                        std::move(topic),
                                                        payload,
                                                        maxSegmentSize,
                                                        freshnessMs);
    }

    ndn::Name ServiceProvider::CollaborationContext::publishLargeNamed(
        KeyScope keyScope,
        const ndn::Name& dataName,
        const ndn::Buffer& payload,
        size_t maxSegmentSize,
        int freshnessMs)
    {
        return m_provider.publishCollaborationLargeDataNamed(m_requestId,
                                                             std::move(keyScope),
                                                             dataName,
                                                             payload,
                                                             maxSegmentSize,
                                                             freshnessMs);
    }

    std::optional<ndn::Buffer>
    ServiceProvider::CollaborationContext::fetchLarge(const ndn::Name& dataName,
                                                      KeyScope keyScope,
                                                      int timeoutMs)
    {
        return fetchLarge(dataName, std::move(keyScope), timeoutMs, 0);
    }

    std::optional<ndn::Buffer>
    ServiceProvider::CollaborationContext::fetchLarge(const ndn::Name& dataName,
                                                      KeyScope keyScope,
                                                      int timeoutMs,
                                                      std::size_t expectedSegments)
    {
        return m_provider.fetchCollaborationLargeData(m_requestId,
                                                      std::move(keyScope),
                                                      dataName,
                                                      timeoutMs,
                                                      expectedSegments);
    }

    bool
    ServiceProvider::CollaborationContext::publishDataV1Segments(
        KeyScope keyScope,
        const std::vector<std::pair<ndn::Name, ndn::Buffer>>& segments,
        int freshnessMs)
    {
        return m_provider.publishCollaborationDataV1Segments(
            m_requestId, std::move(keyScope), segments, freshnessMs);
    }

    std::optional<std::vector<ndn::Buffer>>
    ServiceProvider::CollaborationContext::fetchDataV1Segments(
        KeyScope keyScope,
        const ndn::Name& producerPrefix,
        std::uint64_t operationIndex,
        const std::string& producerRank,
        const std::string& tensorDigest,
        std::size_t expectedSegments,
        std::size_t maxSegments,
        int timeoutMs,
        std::function<std::size_t(const ndn::Buffer&)> segmentCountDecoder,
        DataV1SegmentNameFilter nameFilter)
    {
        return m_provider.fetchCollaborationDataV1Segments(
            m_requestId,
            std::move(keyScope),
            producerPrefix,
            operationIndex,
            producerRank,
            tensorDigest,
            expectedSegments,
            maxSegments,
            timeoutMs,
            std::move(segmentCountDecoder),
            std::move(nameFilter));
    }

    bool
    ServiceProvider::CollaborationContext::publishSignedExactData(
        KeyScope keyScope,
        const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
        int freshnessMs)
    {
        return m_provider.publishCollaborationSignedExactData(
            m_requestId, std::move(keyScope), objects, freshnessMs);
    }

    std::optional<ndn::Buffer>
    ServiceProvider::CollaborationContext::fetchSignedExactData(
        KeyScope keyScope,
        const ndn::Name& dataName,
        const ndn::Name& expectedProducer,
        int timeoutMs,
        std::function<bool()> shouldCancel)
    {
        return m_provider.fetchCollaborationSignedExactData(
            m_requestId, std::move(keyScope), dataName,
            expectedProducer, timeoutMs, std::move(shouldCancel));
    }

    void ServiceProvider::CollaborationContext::subscribe(
        KeyScope keyScope,
        Topic topicPrefix,
        std::function<void(const CollaborationData&)> onData)
    {
        m_provider.addCollaborationSubscription(m_requestId,
                                                std::move(keyScope),
                                                std::move(topicPrefix),
                                                std::move(onData));
    }

    void ServiceProvider::CollaborationContext::subscribe(
        KeyScope keyScope,
        Topic topicPrefix,
        std::function<void(CollaborationContext&, const CollaborationData&)> onData)
    {
        m_provider.addCollaborationSubscription(m_requesterName,
                                                m_requestId,
                                                m_requestMessage,
                                                m_assignment,
                                                std::move(keyScope),
                                                std::move(topicPrefix),
                                                std::move(onData));
    }

    void ServiceProvider::CollaborationContext::allowData(
        KeyScope keyScope,
        Topic topicPrefix)
    {
        m_provider.addCollaborationReceiveFilter(m_requestId,
                                                  std::move(keyScope),
                                                  std::move(topicPrefix));
    }

    std::optional<ServiceProvider::CollaborationData>
    ServiceProvider::CollaborationContext::waitOne(KeyScope keyScope,
                                                   Topic topicPrefix,
                                                   int timeoutMs)
    {
        auto data = waitFor(std::move(keyScope), std::move(topicPrefix), 1, timeoutMs);
        if (data.empty()) {
            return std::nullopt;
        }
        return data.front();
    }

    std::vector<ServiceProvider::CollaborationData>
    ServiceProvider::CollaborationContext::waitFor(KeyScope keyScope,
                                                   Topic topicPrefix,
                                                   size_t minCount,
                                                   int timeoutMs)
    {
        return m_provider.waitForCollaborationData(m_requestId,
                                                   keyScope,
                                                   topicPrefix,
                                                   minCount,
                                                   timeoutMs);
    }

    void ServiceProvider::CollaborationContext::reportOperationStatus(
        ServiceOperationStatus status)
    {
        status.providerName = m_provider.identity;
        status.serviceName = m_assignment.service;
        status.requestId = m_requestId;
        if (status.role.empty()) {
            status.role = m_assignment.role;
        }
        m_provider.reportSelectionOperationStatus(
            m_assignment.selectionDigest, std::move(status));
    }

    void ServiceProvider::CollaborationContext::publishFinalResponse(
        const ndn::Buffer& payload)
    {
        // A streamed collaboration must close through the stream publisher
        // so the End event and terminal Response share one lifecycle.  Keep
        // this fallback for legacy/native handlers that still call the unary
        // method, but never let that call silently bypass stream completion.
        if (isStreamed()) {
            if (!finishStream(payload, StreamFinishReason::ApplicationComplete)) {
                throw std::logic_error("stream terminal was already claimed or fenced");
            }
            return;
        }
        m_provider.publishCollaborationFinalResponse(m_requesterName,
                                                     m_assignment.service,
                                                     m_requestId,
                                                     m_requestMessage,
                                                     payload,
                                                     m_assignment.selectionDigest);
    }

    std::shared_ptr<StreamEventPublisher>
    ServiceProvider::CollaborationContext::streamPublisher() const
    {
        if (!m_requestMessage.hasStreamRequestOptions()) {
            return nullptr;
        }
        const auto pendingKey = ndn::Name(m_requesterName)
            .append(m_assignment.service).append(m_requestId);
        std::lock_guard<std::mutex> lock(m_provider.m_pendingRequestMutex);
        const auto it = m_provider.m_streamPublishers.find(pendingKey);
        return it == m_provider.m_streamPublishers.end() ? nullptr : it->second;
    }

    bool ServiceProvider::CollaborationContext::isStreamed() const
    {
        return static_cast<bool>(streamPublisher());
    }

    uint64_t ServiceProvider::CollaborationContext::publishStreamEvent(
        const ndn::Buffer& payload)
    {
        auto publisher = streamPublisher();
        if (!publisher) return 0;
        const auto published = publisher->publish(
            payload, std::chrono::steady_clock::now() + std::chrono::seconds(30));
        return published ? published->cursor : 0;
    }

    bool ServiceProvider::CollaborationContext::finishStream(
        const ndn::Buffer& payload, StreamFinishReason reason)
    {
        auto publisher = streamPublisher();
        if (!publisher) return false;
        if (m_streamTerminal) return false;
        const auto completion = publisher->finish(
            payload, reason, std::chrono::steady_clock::now() + std::chrono::seconds(30));
        if (!completion) return false;
        // The publisher is the authoritative terminal commit point.  Do not
        // fence the CollaborationContext before it accepts the End event:
        // a bounded queue/deadline rejection must still be reportable through
        // failStream instead of leaving the request with neither End nor
        // failure Response.
        m_streamTerminal = true;
        ResponseMessage response;
        response.setStatus(true);
        auto finalPayload = payload;
        response.setPayload(finalPayload, finalPayload.size());
        response.setStreamCompletion(*completion);
        boost::asio::post(m_provider.m_face.getIoContext(),
            [provider = &m_provider,
             requester = m_requesterName,
             service = m_assignment.service,
             requestId = m_requestId,
             request = m_requestMessage,
             response = std::move(response),
             selectionDigest = m_assignment.selectionDigest]() mutable {
                provider->finishRequestExecutionOnEventLoop(
                    requester, provider->identity, service, requestId,
                    request, std::move(response), selectionDigest);
            });
        return true;
    }

    bool ServiceProvider::CollaborationContext::failStream(
        StreamedInvocationErrorCode, const std::string& message)
    {
        auto publisher = streamPublisher();
        if (!publisher) return false;
        if (m_streamTerminal) return false;
        if (!publisher->fail(message)) return false;
        m_streamTerminal = true;
        boost::asio::post(m_provider.m_face.getIoContext(),
            [provider = &m_provider,
             requester = m_requesterName,
             service = m_assignment.service,
             requestId = m_requestId,
             request = m_requestMessage,
             message,
             selectionDigest = m_assignment.selectionDigest]() mutable {
                provider->publishExecutionFailureOnEventLoop(
                    requester, provider->identity, service, requestId,
                    request, message, selectionDigest);
            });
        return true;
    }

    bool ServiceProvider::CollaborationContext::completeRole()
    {
        // A role completion is deliberately distinct from stream terminal
        // ownership.  Non-final roles must release their provider-side
        // pending request without publishing End or a user-facing Response.
        if (m_streamTerminal) return false;
        m_streamTerminal = true;
        boost::asio::post(m_provider.m_face.getIoContext(),
            [provider = &m_provider,
             requester = m_requesterName,
             service = m_assignment.service,
             requestId = m_requestId,
             selectionDigest = m_assignment.selectionDigest]() mutable {
                provider->completeCollaborationRoleOnEventLoop(
                    requester, provider->identity, service, requestId,
                    std::move(selectionDigest));
            });
        return true;
    }

    bool ServiceProvider::CollaborationContext::streamCancelled() const
    {
        const auto pendingKey = ndn::Name(m_requesterName)
            .append(m_assignment.service).append(m_requestId);
        const auto lifecycle = m_provider.getStreamLifecycle(pendingKey);
        return !lifecycle || lifecycle->terminalAuthority()->isTerminal() ||
               lifecycle->terminalAuthority()->isFenced();
    }

    std::chrono::milliseconds
    ServiceProvider::CollaborationContext::streamRemainingDeadline() const
    {
        if (!m_requestMessage.hasStreamRequestOptions()) {
            return std::chrono::milliseconds(0);
        }
        const auto deadlineMs =
            m_requestMessage.getStreamRequestOptions().deadlineEpochMs;
        const auto nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (deadlineMs <= nowMs) {
            return std::chrono::milliseconds(0);
        }
        const auto delta = deadlineMs - nowMs;
        using Rep = std::chrono::milliseconds::rep;
        const auto bounded = std::min<uint64_t>(
            delta, static_cast<uint64_t>(std::numeric_limits<Rep>::max()));
        return std::chrono::milliseconds(static_cast<Rep>(bounded));
    }

    void ServiceProvider::setAckStrategyHandler(const ndn::Name& serviceName,
                                                AckStrategyHandler ackHandler)
    {
        m_services[serviceName].ackHandler = std::move(ackHandler);
        const auto serviceUri = serviceName.toUri();
        if (std::find(m_serviceNames.begin(), m_serviceNames.end(), serviceUri) ==
            m_serviceNames.end()) {
            m_serviceNames.push_back(serviceUri);
        }
    }

    void ServiceProvider::setLocalPublicationHandler(LocalPublicationHandler handler)
    {
        m_localPublicationHandler = std::move(handler);
    }

    void ServiceProvider::setLegacyAckStrategyHandler(
        const ndn::Name& serviceName,
        LegacyAckStrategyHandler ackHandler)
    {
        setAckStrategyHandler(serviceName,
                              wrapLegacyAckStrategyHandler(std::move(ackHandler)));
    }

    bool ServiceProvider::hasService(const ndn::Name& serviceName) const
    {
        return m_services.find(serviceName) != m_services.end();
    }

    size_t ServiceProvider::getPendingRequestCountForTesting() const
    {
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        return pendingRequests.size();
    }

    size_t ServiceProvider::getSelectedOutstandingRequestCountForTesting() const
    {
        return m_selectedOutstandingRequests.load(std::memory_order_relaxed);
    }

    size_t ServiceProvider::getPendingProviderTokenCountForTesting() const
    {
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        return pendingProviderTokens.size();
    }

    size_t ServiceProvider::getCleanupInvocationCountForTesting() const
    {
        return m_cleanupInvocationCount;
    }

    size_t ServiceProvider::getTokenConsumeCountForTesting() const
    {
        return m_tokenConsumeCount;
    }

    void ServiceProvider::setPendingRequestTimeoutGrace(ndn::time::milliseconds grace)
    {
        m_pendingRequestTimeoutGrace = std::max(ndn::time::milliseconds(0), grace);
    }

    void ServiceProvider::setPerformanceMode(bool enabled)
    {
        m_performanceMode = enabled;
    }

    void ServiceProvider::setHandlerThreads(size_t n)
    {
        m_handlerPool.setThreadCount(n);
        NDN_LOG_WARN("NDNSF provider worker threads: " << n);
    }

    size_t ServiceProvider::getHandlerThreads() const
    {
        return m_handlerPool.getThreadCount();
    }

    size_t ServiceProvider::getHandlerQueueDepth() const
    {
        return m_handlerPool.getQueueSize();
    }

    void ServiceProvider::setAckThreads(size_t n)
    {
        m_ackPool.setThreadCount(n);
        NDN_LOG_WARN("NDNSF provider ACK worker threads: " << n);
    }

    size_t ServiceProvider::getAckThreads() const
    {
        return m_ackPool.getThreadCount();
    }

    size_t ServiceProvider::getAckQueueDepth() const
    {
        return m_ackPool.getQueueSize();
    }

    void ServiceProvider::setUseTokens(bool enabled)
    {
        m_useTokens = enabled;
        NDN_LOG_WARN("UserToken/ProviderToken runtime mode: "
                     << (m_useTokens ? "enabled" : "disabled for controlled experiment"));
    }

    bool ServiceProvider::getUseTokens() const
    {
        return m_useTokens;
    }

    void ServiceProvider::setTimelineTrace(bool enabled)
    {
        m_timelineTrace = enabled;
        if (enabled) {
            setenv("NDNSF_TIMELINE_TRACE", "1", 1);
        }
    }

    HybridCryptoCounters& ServiceProvider::getHybridCryptoCounters()
    {
        return m_hybridCryptoCounters;
    }

    void ServiceProvider::setAdaptiveAckAdmission(bool enabled)
    {
        m_adaptiveAckAdmission = enabled;
    }

    void ServiceProvider::setProviderAckMaxPending(size_t maxPending)
    {
        m_providerAckMaxPending = maxPending;
    }

    void ServiceProvider::setProviderAckMaxEventLoopLag(ndn::time::milliseconds maxLag)
    {
        m_providerAckMaxEventLoopLag = std::max(ndn::time::milliseconds(0), maxLag);
    }

    void ServiceProvider::setProviderAckMaxSelectionLag(ndn::time::milliseconds maxLag)
    {
        m_providerAckMaxSelectionLag = std::max(ndn::time::milliseconds(0), maxLag);
    }

    void ServiceProvider::setProviderRequestLifecycleCallback(
        ProviderRequestLifecycleCallback callback)
    {
        m_providerRequestLifecycleCallback = std::move(callback);
    }

    const char* ServiceProvider::providerRequestLifecycleStateToString(
        ProviderRequestLifecycleState state)
    {
        switch (state) {
        case ProviderRequestLifecycleState::REQUEST_OBSERVED: return "REQUEST_OBSERVED";
        case ProviderRequestLifecycleState::ACK_ADMISSION_CHECKED: return "ACK_ADMISSION_CHECKED";
        case ProviderRequestLifecycleState::ACK_SUPPRESSED_OVERLOAD: return "ACK_SUPPRESSED_OVERLOAD";
        case ProviderRequestLifecycleState::ACK_PUBLISHED: return "ACK_PUBLISHED";
        case ProviderRequestLifecycleState::SELECTION_RECEIVED: return "SELECTION_RECEIVED";
        case ProviderRequestLifecycleState::EXECUTION_STARTED: return "EXECUTION_STARTED";
        case ProviderRequestLifecycleState::EXECUTION_DONE: return "EXECUTION_DONE";
        case ProviderRequestLifecycleState::RESPONSE_PUBLISHED: return "RESPONSE_PUBLISHED";
        case ProviderRequestLifecycleState::PROVIDER_REQUEST_EXPIRED: return "PROVIDER_REQUEST_EXPIRED";
        }
        return "UNKNOWN";
    }

    std::optional<ServiceProvider::ProviderRequestLifecycleStatus>
    ServiceProvider::getProviderRequestStatus(const ndn::Name& requestId) const
    {
        auto it = m_providerRequestLifecycleStatuses.find(requestId);
        if (it == m_providerRequestLifecycleStatuses.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<ServiceProvider::ProviderRequestLifecycleStatus>
    ServiceProvider::getActiveProviderRequestStatuses() const
    {
        std::vector<ProviderRequestLifecycleStatus> statuses;
        for (const auto& item : m_providerRequestLifecycleStatuses) {
            if (item.second.finalStatus.empty()) {
                statuses.push_back(item.second);
            }
        }
        return statuses;
    }

    std::string ServiceProvider::encodeSelectionExecutionStatus(
        const SelectionExecutionStatus& status)
    {
        std::ostringstream os;
        os << "state=" << selectionExecutionStateToString(status.state) << "\n"
           << "provider=" << status.providerName.toUri() << "\n"
           << "service=" << status.serviceName.toUri() << "\n"
           << "request_id=" << status.requestId.toUri() << "\n"
           << "selection_digest=" << status.selectionDigest << "\n"
           << "message=" << status.message << "\n"
           << "response_name=" << status.responseName.toUri() << "\n"
           << "received_at_us=" << status.receivedAtUs << "\n"
           << "queued_at_us=" << status.queuedAtUs << "\n"
           << "running_at_us=" << status.runningAtUs << "\n"
           << "completed_at_us=" << status.completedAtUs << "\n"
           << "updated_at_us=" << status.updatedAtUs << "\n"
           << "decision_receipt_hex=" << hexEncode(status.decisionReceipt) << "\n"
           << "member_count=" << status.memberStatuses.size() << "\n";
        for (size_t i = 0; i < status.memberStatuses.size(); ++i) {
            const auto& member = status.memberStatuses[i];
            const std::string prefix = "member." + std::to_string(i) + ".";
            os << prefix << "provider=" << member.providerName.toUri() << "\n"
               << prefix << "service=" << member.serviceName.toUri() << "\n"
               << prefix << "request_id=" << member.requestId.toUri() << "\n"
               << prefix << "selection_digest=" << member.selectionDigest << "\n"
               << prefix << "role=" << member.role << "\n"
               << prefix << "operation_id=" << member.operationId << "\n"
               << prefix << "operation=" << member.operation << "\n"
               << prefix << "state=" << member.state << "\n"
               << prefix << "reason_code=" << member.reasonCode << "\n"
               << prefix << "message=" << member.message << "\n"
               << prefix << "attempt=" << member.attempt << "\n"
               << prefix << "epoch=" << member.epoch << "\n"
               << prefix << "sequence=" << member.sequence << "\n"
               << prefix << "progress_known=" << (member.progressKnown ? 1 : 0) << "\n"
               << prefix << "progress=" << member.progress << "\n"
               << prefix << "created_at_ms=" << member.createdAtMs << "\n"
               << prefix << "updated_at_ms=" << member.updatedAtMs << "\n"
               << prefix << "expires_at_ms=" << member.expiresAtMs << "\n"
               << prefix << "details_schema=" << member.detailsSchema << "\n"
               << prefix << "details_hex=" << hexEncode(member.detailsPayload) << "\n";
        }
        return os.str();
    }

    void ServiceProvider::reportSelectionOperationStatus(
        const std::string& selectionDigest,
        ServiceOperationStatus status)
    {
        if (selectionDigest.empty() || status.operationId.empty() ||
            status.operation.empty() || status.attempt == 0 || status.epoch == 0 ||
            status.sequence == 0 || status.progress < 0.0 || status.progress > 1.0 ||
            status.detailsPayload.size() > 4096) {
            throw std::invalid_argument("invalid collaboration operation status");
        }
        std::lock_guard<std::mutex> lock(m_selectionExecutionStatusMutex);
        auto found = m_selectionExecutionStatuses.find(selectionDigest);
        if (found == m_selectionExecutionStatuses.end()) {
            throw std::invalid_argument("selection status binding is unknown");
        }
        auto& parent = found->second;
        status.providerName = status.providerName.empty() ? identity : status.providerName;
        status.serviceName = status.serviceName.empty() ? parent.serviceName : status.serviceName;
        status.requestId = status.requestId.empty() ? parent.requestId : status.requestId;
        if (!status.providerName.equals(identity) ||
            !status.serviceName.equals(parent.serviceName) ||
            !status.requestId.equals(parent.requestId)) {
            throw std::invalid_argument("collaboration operation status binding mismatch");
        }
        CollaborationMemberStatus snapshot;
        snapshot.providerName = status.providerName;
        snapshot.serviceName = status.serviceName;
        snapshot.requestId = status.requestId;
        snapshot.selectionDigest = selectionDigest;
        snapshot.role = status.role;
        snapshot.operationId = status.operationId;
        snapshot.operation = status.operation;
        snapshot.state = status.state;
        snapshot.reasonCode = status.reasonCode;
        snapshot.message = status.message;
        snapshot.attempt = status.attempt;
        snapshot.epoch = status.epoch;
        snapshot.sequence = status.sequence;
        snapshot.progressKnown = status.progressKnown;
        snapshot.progress = status.progress;
        snapshot.createdAtMs = status.createdAtMs;
        snapshot.updatedAtMs = status.updatedAtMs;
        snapshot.expiresAtMs = status.expiresAtMs;
        snapshot.detailsSchema = status.detailsSchema;
        snapshot.detailsPayload = status.detailsPayload;
        auto& members = parent.memberStatuses;
        auto existing = std::find_if(members.begin(), members.end(),
            [&snapshot](const CollaborationMemberStatus& item) {
                return item.role == snapshot.role &&
                       item.operationId == snapshot.operationId;
            });
        if (existing != members.end()) {
            if (snapshot.epoch < existing->epoch ||
                (snapshot.epoch == existing->epoch &&
                 snapshot.sequence <= existing->sequence)) {
                throw std::invalid_argument("stale collaboration operation status");
            }
            *existing = std::move(snapshot);
        }
        else {
            if (members.size() >= 64) {
                throw std::length_error("collaboration status member bound exceeded");
            }
            members.push_back(std::move(snapshot));
        }
        parent.updatedAtUs = nowMicroseconds();
    }

    SelectionExecutionStatus
    ServiceProvider::makeUnknownSelectionExecutionStatus(
        const ndn::Name& providerName,
        const std::string& selectionDigest)
    {
        SelectionExecutionStatus status;
        status.providerName = providerName;
        status.selectionDigest = selectionDigest;
        status.state = SelectionExecutionState::Unknown;
        status.message = "selection status not found";
        status.updatedAtUs = nowMicroseconds();
        return status;
    }

    std::optional<SelectionExecutionStatus>
    ServiceProvider::getSelectionExecutionStatus(
        const std::string& selectionDigest) const
    {
        std::lock_guard<std::mutex> lock(m_selectionExecutionStatusMutex);
        auto it = m_selectionExecutionStatuses.find(selectionDigest);
        if (it == m_selectionExecutionStatuses.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void ServiceProvider::updateSelectionExecutionStatus(
        const std::string& selectionDigest,
        SelectionExecutionState state,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const std::string& message,
        const ndn::Name& responseName)
    {
        if (selectionDigest.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_selectionExecutionStatusMutex);
        auto& status = m_selectionExecutionStatuses[selectionDigest];
        status.providerName = providerName;
        status.serviceName = serviceName;
        status.requestId = requestId;
        status.selectionDigest = selectionDigest;
        status.state = state;
        if (!message.empty()) {
            status.message = message;
        }
        if (!responseName.empty()) {
            status.responseName = responseName;
        }
        const auto nowUs = nowMicroseconds();
        status.updatedAtUs = nowUs;
        NDN_LOG_INFO("NDNSF_SELECTION_STATUS digest=" << selectionDigest
                     << " state=" << static_cast<int>(state)
                     << " provider=" << providerName.toUri()
                     << " service=" << serviceName.toUri()
                     << " requestId=" << requestId.toUri()
                     << " message=" << status.message);
        switch (state) {
        case SelectionExecutionState::Received:
            if (status.receivedAtUs == 0) {
                status.receivedAtUs = nowUs;
            }
            break;
        case SelectionExecutionState::Queued:
            if (status.queuedAtUs == 0) {
                status.queuedAtUs = nowUs;
            }
            break;
        case SelectionExecutionState::Running:
            if (status.runningAtUs == 0) {
                status.runningAtUs = nowUs;
            }
            break;
        case SelectionExecutionState::Completed:
        case SelectionExecutionState::Failed:
        case SelectionExecutionState::Rejected:
        case SelectionExecutionState::Expired:
        case SelectionExecutionState::Cancelled:
            if (status.completedAtUs == 0) {
                status.completedAtUs = nowUs;
            }
            break;
        case SelectionExecutionState::Unknown:
            break;
        }
    }

    bool ServiceProvider::replySelectionExecutionStatus(const ndn::Interest& interest)
    {
        const auto parsed = parseSelectionStatusQueryName(interest.getName());
        if (!parsed || !parsed->providerName.equals(identity)) {
            return false;
        }
        auto service = m_services.find(parsed->serviceName);
        if (service == m_services.end() ||
            !service->second.selectionStatusQueryable) {
            return false;
        }

        auto status = getSelectionExecutionStatus(parsed->selectionDigest);
        const SelectionExecutionStatus reply =
            status ? *status :
                     makeUnknownSelectionExecutionStatus(identity,
                                                         parsed->selectionDigest);
        const auto payload = encodeSelectionExecutionStatus(reply);
        auto data = std::make_shared<ndn::Data>(interest.getName());
        data->setFreshnessPeriod(ndn::time::milliseconds(1000));
        data->setContent(payload);
        if (m_svsps == nullptr) {
            (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain)
                .sign(*data, ndn::security::signingWithSha256());
        }
        else {
            (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain)
                .sign(*data, m_signingInfo);
        }
        m_face.put(*data);
        return true;
    }

    std::map<std::string, uint64_t>
    ServiceProvider::getProviderAdmissionCounters() const
    {
        return m_providerAdmissionCounters;
    }

    void ServiceProvider::updateProviderRequestLifecycleState(
        const ndn::Name& requestId,
        const ndn::Name& serviceName,
        ProviderRequestLifecycleState state,
        const std::string& suppressionReason,
        const std::string& finalStatus)
    {
        const auto nowUs = nowMicroseconds();
        auto& status = m_providerRequestLifecycleStatuses[requestId];
        status.requestId = requestId;
        if (!serviceName.empty()) {
            status.serviceName = serviceName;
        }
        status.providerName = identity;
        status.state = state;
        ++m_providerAdmissionCounters[providerRequestLifecycleStateToString(state)];
        switch (state) {
        case ProviderRequestLifecycleState::REQUEST_OBSERVED:
            if (status.requestObservedTimestampUs == 0) {
                status.requestObservedTimestampUs = nowUs;
            }
            break;
        case ProviderRequestLifecycleState::ACK_ADMISSION_CHECKED:
            status.ackAdmissionDecisionTimestampUs = nowUs;
            status.providerPendingCountAtDecision = pendingRequests.size();
            break;
        case ProviderRequestLifecycleState::ACK_SUPPRESSED_OVERLOAD:
            status.ackPublishedOrSuppressedTimestampUs = nowUs;
            status.providerPendingCountAtDecision = pendingRequests.size();
            status.suppressionReason = suppressionReason;
            status.finalStatus = finalStatus.empty() ? "ack_suppressed" : finalStatus;
            ++m_providerAdmissionCounters["ACK_SUPPRESSION_REASON_" + suppressionReason];
            break;
        case ProviderRequestLifecycleState::ACK_PUBLISHED:
            status.ackPublishedOrSuppressedTimestampUs = nowUs;
            break;
        case ProviderRequestLifecycleState::SELECTION_RECEIVED:
            status.selectionReceivedTimestampUs = nowUs;
            if (status.ackPublishedOrSuppressedTimestampUs != 0 &&
                nowUs >= status.ackPublishedOrSuppressedTimestampUs) {
                status.selectionLagUs = nowUs - status.ackPublishedOrSuppressedTimestampUs;
            }
            break;
        case ProviderRequestLifecycleState::EXECUTION_STARTED:
            status.executionStartTimestampUs = nowUs;
            break;
        case ProviderRequestLifecycleState::EXECUTION_DONE:
            status.executionDoneTimestampUs = nowUs;
            break;
        case ProviderRequestLifecycleState::RESPONSE_PUBLISHED:
            status.responsePublishedTimestampUs = nowUs;
            status.finalStatus = finalStatus.empty() ? "response_published" : finalStatus;
            break;
        case ProviderRequestLifecycleState::PROVIDER_REQUEST_EXPIRED:
            status.finalStatus = finalStatus.empty() ? "expired" : finalStatus;
            break;
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PROVIDER_LIFECYCLE_STATE timestamp_us="
                  << nowUs
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << (status.serviceName.empty() ? "-" : status.serviceName.toUri())
                  << " providerName=" << identity.toUri()
                  << " state=" << providerRequestLifecycleStateToString(state)
                  << " suppressionReason="
                  << (status.suppressionReason.empty() ? "-" : status.suppressionReason)
                  << " pendingAtDecision=" << status.providerPendingCountAtDecision
                  << " selectionLagUs=" << status.selectionLagUs
                  << " finalStatus="
                  << (status.finalStatus.empty() ? "-" : status.finalStatus));
        if (m_providerRequestLifecycleCallback) {
            m_providerRequestLifecycleCallback(status);
        }
        logControlTiming("provider",
                         providerRequestLifecycleStateToString(state),
                         requestId,
                         {{"serviceName", status.serviceName.empty() ? "-" : status.serviceName.toUri()},
                          {"providerName", identity.toUri()},
                          {"suppressionReason", status.suppressionReason.empty() ? "-" : status.suppressionReason},
                          {"pendingAtDecision", std::to_string(status.providerPendingCountAtDecision)},
                          {"selectionLagUs", std::to_string(status.selectionLagUs)},
                          {"eventLoopLagUs", std::to_string(status.eventLoopLagUs)},
                          {"finalStatus", status.finalStatus.empty() ? "-" : status.finalStatus}});
    }

    std::shared_ptr<StreamInvocationLifecycle>
    ServiceProvider::attachStreamLifecycle(const ndn::Name& pendingKey)
    {
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        if (pendingRequests.find(pendingKey) == pendingRequests.end()) {
            throw std::invalid_argument(
                "cannot attach streamed lifecycle to an unknown pending request");
        }
        auto& lifecycle = m_streamLifecycles[pendingKey];
        if (!lifecycle) {
            lifecycle = std::make_shared<StreamInvocationLifecycle>();
        }
        return lifecycle;
    }

    std::shared_ptr<StreamInvocationLifecycle>
    ServiceProvider::getStreamLifecycle(const ndn::Name& pendingKey) const
    {
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        const auto lifecycle = m_streamLifecycles.find(pendingKey);
        return lifecycle == m_streamLifecycles.end() ? nullptr : lifecycle->second;
    }

    void
    ServiceProvider::publishStreamEventOnFaceEventLoop(
        PublishedStreamEvent event,
        uint64_t retentionMs)
    {
        if (!m_svsps) {
            throw std::runtime_error("SVS is not attached");
        }

        // Stream handlers run in the Provider worker pool. Face, Scheduler,
        // IMS satisfaction, and SVSPubSub sequence/mapping state are all owned
        // by the Face io_context. Keep the synchronous writer contract while
        // transferring the complete network-side commit to that owner. Asio
        // dispatch executes inline when this method is already called by the
        // Face loop and queues otherwise, so neither path races SVS state.
        auto completion = std::make_shared<std::promise<void>>();
        auto future = completion->get_future();
        m_face.getIoContext().dispatch(
            [this, completion, event = std::move(event), retentionMs] () mutable {
                try {
                    // ndn-cxx's IMS retains Data through enable_shared_from_this;
                    // keep the decoded packet heap-owned while inserting it.
                    auto data = std::make_shared<ndn::Data>();
                    data->wireDecode(ndn::Block(event.signedWire));

                    // A stream event is a protected Provider transition, not
                    // merely an SVS publication.  The handler may have
                    // produced the signed event before a newer Controller
                    // status arrived; check the authoritative local status at
                    // the Face commit point so a revoked Provider cannot put
                    // another event on the wire.  Throwing here propagates
                    // through StreamEventPublisher::publish/finish and fences
                    // the stream instead of reporting a false publication.
                    const auto parsedEvent = parseInvocationEventName(data->getName());
                    if (!parsedEvent ||
                        !authorizeControllerTransition(parsedEvent->serviceName,
                                                       ProtectedTransition::STREAM_EVENT)) {
                        throw std::runtime_error(
                            "stream event rejected by Controller revocation state");
                    }

                    StreamRetentionInterceptorForTest retentionInterceptor;
                    {
                        std::lock_guard<std::mutex> lock(
                            m_streamPublicationInterceptorMutex);
                        retentionInterceptor = m_streamRetentionInterceptorForTest;
                    }
                    if (retentionInterceptor && !retentionInterceptor(*data)) {
                        NDN_LOG_INFO("NDNSF_STREAM_TEST_RETENTION_SUPPRESSED name="
                                     << data->getName());
                        completion->set_value();
                        return;
                    }

                    // Keep the exact signed Data in the Provider IMS for bounded
                    // retransmission. The IMS is the only source permitted to
                    // satisfy an exact retry and never re-encrypts or re-signs.
                    insertDataIntoIMS(
                        *data,
                        ndn::time::milliseconds(retentionMs));
                    const auto retainedEventName = data->getName();
                    m_scheduler.schedule(
                        ndn::time::milliseconds(retentionMs),
                        [this, retainedEventName] {
                            {
                                std::lock_guard<std::mutex> lock(_cache_mutex);
                                m_IMS.erase(retainedEventName);
                            }
                            StreamRetentionExpiryObserverForTest observer;
                            {
                                std::lock_guard<std::mutex> lock(
                                    m_streamPublicationInterceptorMutex);
                                observer = m_streamRetentionExpiryObserverForTest;
                            }
                            if (observer) {
                                observer(retainedEventName);
                            }
                            NDN_LOG_INFO("NDNSF_STREAM_RETENTION_EXPIRED name="
                                         << retainedEventName);
                        });

                    StreamPublicationInterceptorForTest interceptor;
                    {
                        std::lock_guard<std::mutex> lock(
                            m_streamPublicationInterceptorMutex);
                        interceptor = m_streamPublicationInterceptorForTest;
                    }
                    if (interceptor && !interceptor(*data)) {
                        NDN_LOG_INFO("NDNSF_STREAM_TEST_PUBLICATION_SUPPRESSED name="
                                     << data->getName());
                        completion->set_value();
                        return;
                    }
                    const auto seqNo = m_svsps->publishPacket(*data);
                    if (seqNo == 0) {
                        throw std::runtime_error("SVS rejected streamed event publication");
                    }
                    if (std::getenv("SPEC175_TRACE") != nullptr) {
                        NDN_LOG_INFO("SPEC175_TRACE stream-event-committed name="
                                     << data->getName()
                                     << " cursor=" << event.cursor
                                     << " wireBytes=" << event.signedWire.size()
                                     << " svsSeq=" << seqNo);
                    }
                    completion->set_value();
                }
                catch (...) {
                    try {
                        completion->set_exception(std::current_exception());
                    }
                    catch (...) {
                    }
                }
            });

        if (future.wait_for(std::chrono::seconds(5)) !=
            std::future_status::ready) {
            throw std::runtime_error(
                "stream event publication timed out on Provider Face event loop");
        }
        future.get();
    }

    bool
    ServiceProvider::initializeStreamPublisher(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage,
        const ServiceSelectionMessage& selectionMessage,
        const std::string& selectionDigest)
    {
        if (!requestMessage.hasStreamRequestOptions()) return true;
        // LocalMock fixtures bind their identities to the fixture KeyChain via
        // useSigningKeyChainForTest().  Use that same TPM for the recipient
        // unwrap; production providers continue to use their private chain.
        auto& activeKeyChain = m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain;
        const auto& options = requestMessage.getStreamRequestOptions();
        if (options.controllerVersion && requestMessage.hasControllerVersion() &&
            *options.controllerVersion != requestMessage.getControllerVersion()) {
            NDN_LOG_WARN("Reject streamed request with mismatched ControllerVersion requestId="
                         << requestId.toUri());
            return false;
        }
        if (options.controllerVersion && selectionMessage.hasControllerVersion() &&
            *options.controllerVersion != selectionMessage.getControllerVersion()) {
            NDN_LOG_WARN("Reject streamed Selection with mismatched ControllerVersion requestId="
                         << requestId.toUri());
            return false;
        }
        ndn::Block grantBlock;
        if (selectionMessage.hasStreamEventKeyGrant()) {
            auto wrapper = selectionMessage.getStreamEventKeyGrant();
            wrapper.parse();
            if (wrapper.elements().size() != 1) return false;
            grantBlock = wrapper.elements().front();
        }
        else if (options.eventKeyGrant) {
            grantBlock = *options.eventKeyGrant;
        }
        else {
            NDN_LOG_WARN("Reject streamed selection without Provider-specific key grant requestId="
                         << requestId.toUri());
            return false;
        }
        HybridMessageEnvelope grant;
        if (!grant.WireDecode(grantBlock) || grant.getMessageType() != "STREAM-GRANT" ||
            grant.getAlgorithm() != "RSA-OAEP" || !grant.hasWrappedMessageKey() ||
            grant.getEpochId() != std::to_string(options.streamEpoch)) {
            return false;
        }
        ndn::Buffer eventKey;
        try {
            eventKey = unwrapSelectionGatedInputKey(
                grant.getWrappedMessageKey(), identityCert.getName(), activeKeyChain);
        }
        catch (const std::exception&) {
            return false;
        }
        if (eventKey.size() != 32 ||
            computeStreamSha256(ndn::span<const uint8_t>(eventKey.data(), eventKey.size())) !=
                options.eventKeyCommitment) {
            return false;
        }
        StreamBinding binding;
        binding.requestId = requestId;
        binding.requester = requesterName;
        binding.serviceName = serviceName;
        binding.producer = providerName;
        binding.producerBootId = providerName.toUri() + ":" +
                                 std::to_string(m_processStartedAtUs);
        binding.attemptEpoch = options.attemptEpoch;
        binding.planDigest = computeStreamSha256(
            ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(selectionDigest.data()),
                                     selectionDigest.size()));
        binding.generationId = options.generationId;
        binding.streamEpoch = options.streamEpoch;
        binding.eventKeyCommitment = options.eventKeyCommitment;
        binding.userToken = ndn::Buffer(
            reinterpret_cast<const uint8_t*>(requestMessage.getUserToken().data()),
            requestMessage.getUserToken().size());
        binding.policyEpoch = selectionMessage.getPolicyEpoch();
        binding.controllerVersion = selectionMessage.hasControllerVersion() ?
            std::optional<ControllerVersion>(selectionMessage.getControllerVersion()) :
            options.controllerVersion;
        binding.deadlineEpochMs = options.deadlineEpochMs;
        try { binding.validate(); }
        catch (const std::exception&) {
            return false;
        }
        const auto expectedGrantBinding = computeStreamGrantBindingDigest(binding);
        if (grant.getKeyId() != selectionGatedHex(ndn::span<const uint8_t>(
                expectedGrantBinding.data(), expectedGrantBinding.size()))) {
            NDN_LOG_WARN("Reject streamed grant with mismatched Provider binding requestId="
                         << requestId.toUri());
            return false;
        }

        const auto pendingKey = ndn::Name(requesterName).append(serviceName).append(requestId);
        auto lifecycle = getStreamLifecycle(pendingKey);
        if (!lifecycle) {
            lifecycle = std::make_shared<StreamInvocationLifecycle>();
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            m_streamLifecycles[pendingKey] = lifecycle;
        }
        auto publisher = std::make_shared<StreamEventPublisher>(
            binding, options, eventKey, lifecycle, activeKeyChain, m_signingInfo,
            [this, retentionMs = options.retentionMs](const PublishedStreamEvent& event) {
                publishStreamEventOnFaceEventLoop(event, retentionMs);
            });
        publisher->start();
        {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            m_streamBindings[pendingKey] = binding;
            m_streamPublishers[pendingKey] = std::move(publisher);
        }
        return true;
    }

    bool ServiceProvider::shouldSuppressAdaptiveAck(const ndn::Name& requesterIdentity,
                                                    const ndn::Name& serviceName,
                                                    const ndn::Name& requestId)
    {
        updateProviderRequestLifecycleState(
            requestId, serviceName,
            ProviderRequestLifecycleState::ACK_ADMISSION_CHECKED);
        if (!m_adaptiveAckAdmission) {
            return false;
        }

        if (m_providerAckMaxPending > 0 &&
            pendingRequests.size() >= m_providerAckMaxPending) {
            updateProviderRequestLifecycleState(
                requestId, serviceName,
                ProviderRequestLifecycleState::ACK_SUPPRESSED_OVERLOAD,
                "max_pending", "ack_suppressed_overload");
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=ACK_SUPPRESSED timestamp_us="
                      << nowMicroseconds()
                      << " providerName=" << identity.toUri()
                      << " requestId=" << requestId.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " requesterName=" << requesterIdentity.toUri()
                      << " reason=max_pending"
                      << " pendingRequests=" << pendingRequests.size()
                      << " threshold=" << m_providerAckMaxPending);
            return true;
        }

        return false;
    }

    bool ServiceProvider::dispatchAckDecisionAsync(
        const ndn::Name& requesterIdentity,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        RequestMessage requestMessage,
        AckStrategyHandler ackHandler,
        std::shared_ptr<RegistrationState> registrationState)
    {
        if (m_ackPool.getThreadCount() == 0 || !ackHandler) {
            return false;
        }

        const bool queued = m_ackPool.post(
            [this,
             requesterIdentity,
             serviceName,
             requestId,
             requestMessage,
             ackHandler = std::move(ackHandler),
             registrationState]() mutable {
                if (m_timelineTrace) {
                    logTimelineTrace("provider", "ack_handler_start", requestId,
                                     {{"providerName", identity.toUri()},
                                      {"requesterName", requesterIdentity.toUri()},
                                      {"serviceName", serviceName.toUri()},
                                      {"queueDepth", std::to_string(
                                           m_ackPool.getQueueSize())}});
                }
                logControlTiming("provider", "ack_handler_start", requestId,
                                 {{"providerName", identity.toUri()},
                                  {"requesterName", requesterIdentity.toUri()},
                                  {"serviceName", serviceName.toUri()},
                                  {"queueDepth", std::to_string(
                                       m_ackPool.getQueueSize())}});
                AckDecision decision;
                if (registrationState && registrationState->closed) {
                    // spec182: the scoped registration retired while the ack
                    // decision was queued; never commit a positive decision
                    // in its name.
                    decision.status = false;
                    decision.message = "Service registration closed before ACK committed";
                }
                else {
                    try {
                        decision = ackHandler(requestMessage);
                        if (decision.message.empty()) {
                            decision.message =
                                decision.status ? "Permission Granted" : "Permission Denied";
                        }
                    }
                    catch (const std::exception& e) {
                        decision.status = false;
                        decision.message = std::string("ACK handler failed: ") + e.what();
                    }
                    catch (...) {
                        decision.status = false;
                        decision.message = "ACK handler failed";
                    }
                }

                if (m_timelineTrace) {
                    logTimelineTrace("provider", "ack_handler_done", requestId,
                                     {{"providerName", identity.toUri()},
                                      {"serviceName", serviceName.toUri()},
                                      {"status", decision.status ? "true" : "false"}});
                }
                logControlTiming("provider", "ack_handler_done", requestId,
                                 {{"providerName", identity.toUri()},
                                  {"serviceName", serviceName.toUri()},
                                  {"status", decision.status ? "true" : "false"}});

                boost::asio::post(m_face.getIoContext(),
                    [this,
                     requesterIdentity,
                     serviceName,
                     requestId,
                     requestMessage,
                     decision = std::move(decision),
                     registrationState]() mutable {
                        finishAckDecisionOnEventLoop(requesterIdentity,
                                                     serviceName,
                                                     requestId,
                                                     std::move(requestMessage),
                                                     std::move(decision),
                                                     std::move(registrationState));
                    });
                if (m_timelineTrace) {
                    logTimelineTrace("provider", "ack_finish_posted", requestId,
                                     {{"providerName", identity.toUri()},
                                      {"serviceName", serviceName.toUri()}});
                }
                logControlTiming("provider", "ack_finish_posted", requestId,
                                 {{"providerName", identity.toUri()},
                                  {"serviceName", serviceName.toUri()}});
            });

        if (!queued) {
            AckDecision decision;
            decision.status = false;
            decision.message = "ACK handler queue full";
            finishAckDecisionOnEventLoop(requesterIdentity,
                                         serviceName,
                                         requestId,
                                         std::move(requestMessage),
                                         std::move(decision),
                                         std::move(registrationState));
        }
        return true;
    }

    void ServiceProvider::finishAckDecisionOnEventLoop(
        const ndn::Name& requesterIdentity,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        RequestMessage requestMessage,
        AckDecision decision,
        std::shared_ptr<RegistrationState> registrationState)
    {
        if (m_timelineTrace) {
            logTimelineTrace("provider", "ack_finish_enter", requestId,
                             {{"providerName", identity.toUri()},
                              {"requesterName", requesterIdentity.toUri()},
                              {"serviceName", serviceName.toUri()},
                              {"status", decision.status ? "true" : "false"}});
        }
        logControlTiming("provider", "ack_finish_enter", requestId,
                         {{"providerName", identity.toUri()},
                          {"requesterName", requesterIdentity.toUri()},
                          {"serviceName", serviceName.toUri()},
                          {"status", decision.status ? "true" : "false"}});
        ndn::Name pendingKey = ndn::Name(requesterIdentity.toUri())
                                   .append(serviceName)
                                   .append(requestId);
        if (decision.suppressAck) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=ACK_SUPPRESSED timestamp_us="
                      << nowMicroseconds()
                      << " providerName=" << identity.toUri()
                      << " requestId=" << requestId.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " reason=" << decision.message);
            updateProviderRequestLifecycleState(
                requestId, serviceName,
                ProviderRequestLifecycleState::ACK_SUPPRESSED_OVERLOAD,
                decision.message.empty() ? "ACK suppressed" : decision.message);
            return;
        }
        const bool requiresReservation =
            requestMessage.hasRequestCapabilities() &&
            requestMessage.getRequestCapabilities().hasField(
                "DIReservationSelectionV1") &&
            requestMessage.getRequestCapabilities().getField(
                "DIReservationSelectionV1") == "required";
        if (decision.status && requiresReservation && !decision.reservationLease) {
            decision.status = false;
            decision.message = "DI_RESERVATION_REQUIRED";
        }
        if (decision.status && registrationState && registrationState->closed) {
            // spec182: the scoped registration retired before the decision
            // landed on the Face thread.  Degrade to the negative path so no
            // pending state is stored and no positive ACK is published in the
            // retired registration's name.
            decision.status = false;
            decision.message = "Service registration closed before ACK committed";
        }
        std::string providerToken;
        if (decision.status) {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            pendingRequests[pendingKey] =
                std::make_shared<RequestMessage>(requestMessage);
            // spec182: bind the pending acceptance to the registration the
            // decision was taken against.  Like pendingRequests itself, a
            // newer acceptance for the same pendingKey supersedes the earlier
            // binding.
            if (registrationState)
                m_pendingRegistrationStates[pendingKey] = registrationState;
            if (decision.reservationLease)
                pendingReservationLeases[pendingKey] = *decision.reservationLease;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PENDING_REQUEST_STORED timestamp_us="
                      << nowMicroseconds()
                      << " providerName=" << identity.toUri()
                      << " requestId=" << requestId.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " pendingKey=" << pendingKey.toUri());
            constexpr uint64_t MAX_PENDING_STATE_TTL_MS =
                60ULL * 60ULL * 1000ULL;
            const auto requestedTtlMs = std::min(
                decision.pendingStateTtlMs, MAX_PENDING_STATE_TTL_MS);
            schedulePendingRequestCleanup(
                pendingKey,
                requestedTtlMs > 0
                    ? ndn::time::milliseconds(requestedTtlMs)
                    : ndn::time::seconds(30),
                requestedTtlMs > 0);
            if (m_useTokens) {
                auto tokenIt = pendingProviderTokens.find(pendingKey);
                if (tokenIt != pendingProviderTokens.end()) {
                    providerToken = tokenIt->second;
                }
                else {
                    providerToken = makeOneTimeToken();
                    pendingProviderTokens[pendingKey] = providerToken;
                }
            }
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=ACK_DECISION timestamp_us="
                  << nowMicroseconds()
                  << " providerName=" << identity.toUri()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " status=" << decision.status
                  << " payloadBytes=" << decision.payload.size()
                  << " providerTokenPresent=" << !providerToken.empty()
                  << " ackQueueDepth=" << m_ackPool.getQueueSize()
                  << " handlerQueueDepth=" << m_handlerPool.getQueueSize());
        PublishRequestAckMessageV2(requesterIdentity,
                                   serviceName,
                                   requestId,
                                   decision.status,
                                   decision.message,
                                   decision.payload,
                                   m_useTokens ? requestMessage.getUserToken() : "",
                                   providerToken,
                                   &requestMessage,
                                   &decision);
    }

    bool ServiceProvider::consumeTargetedProviderToken(
        const ndn::Name& requesterIdentity,
        const ndn::Name& serviceName,
        const RequestMessage& requestMessage,
        std::string& error) const
    {
        if (!m_useTokens) {
            return true;
        }
        if (requestMessage.getProviderToken().empty()) {
            error = "Targeted request missing ProviderToken";
            return false;
        }
        if (requestMessage.getUserToken().empty()) {
            error = "Targeted request missing UserToken";
            return false;
        }

        const std::string tokenHash =
            replayTokenHash("TARGETED", requesterIdentity,
                            serviceName, requestMessage.getProviderToken());
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        if (m_consumedTargetedProviderTokenHashes.find(tokenHash) !=
            m_consumedTargetedProviderTokenHashes.end()) {
            error = "Targeted ProviderToken replayed";
            return false;
        }
        auto tokenIt = m_targetedProviderTokens.find(tokenHash);
        if (tokenIt == m_targetedProviderTokens.end()) {
            error = "Targeted ProviderToken is unknown or expired";
            return false;
        }
        const auto state = tokenIt->second;
        if (!state.requesterIdentity.equals(requesterIdentity) ||
            !state.serviceName.equals(serviceName) ||
            state.userToken != requestMessage.getUserToken()) {
            error = "Targeted token pair mismatch";
            return false;
        }
        m_targetedProviderTokens.erase(tokenIt);
        m_consumedTargetedProviderTokenHashes.insert(tokenHash);
        return true;
    }

    void ServiceProvider::attachTargetedTokenBatch(
        const ndn::Name& requesterIdentity,
        const ndn::Name& serviceName,
        const RequestMessage& requestMessage,
        ResponseMessage& response) const
    {
        if (!m_useTokens || !response.getStatus()) {
            return;
        }

        const size_t configuredBatch = static_cast<size_t>(std::clamp(
            intEnvOrDefault("NDNSF_TARGETED_TOKEN_BATCH_SIZE", 256), 1, 256));
        size_t tokenPairCount = configuredBatch;
        const auto& requestTokens = requestMessage.getTokens();
        const auto hintIt = requestTokens.find("targeted.batch_hint");
        if (hintIt != requestTokens.end()) {
            tokenPairCount = parseTargetedTokenBatch(hintIt->second, configuredBatch);
        }
        std::map<std::string, std::string> tokens = response.getTokens();
        const auto publicKey = identityCert.getPublicKey();
        ndn::Buffer publicKeyBuffer(publicKey.begin(), publicKey.end());
        // Targeted streamed calls reuse this recipient offer after the token
        // bootstrap.  It contains public metadata only; each request still
        // generates a fresh event key and binding digest.
        tokens["targeted.recipientPublicKey"] = selectionGatedHex(publicKeyBuffer);
        tokens["targeted.recipientCertName"] = identityCert.getName().toUri();
        tokens["targeted.recipientCertDigest"] = sha256DigestString(publicKeyBuffer);
        tokens["targeted.providerBootEpoch"] =
            identity.toUri() + ":" + std::to_string(m_processStartedAtUs);
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        for (size_t i = 0; i < tokenPairCount; ++i) {
            const auto providerToken = makeOneTimeToken();
            const auto userToken = makeOneTimeToken();
            const auto tokenHash =
                replayTokenHash("TARGETED", requesterIdentity,
                                serviceName, providerToken);
            m_targetedProviderTokens[tokenHash] =
                TargetedProviderTokenState{requesterIdentity, serviceName, userToken};
            tokens["targeted." + std::to_string(i) + ".provider"] = providerToken;
            tokens["targeted." + std::to_string(i) + ".user"] = userToken;
        }
        tokens["targeted.count"] = std::to_string(tokenPairCount);
        response.setTokens(tokens);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=TARGETED_TOKEN_BATCH_ATTACHED timestamp_us="
                  << nowMicroseconds()
                  << " requesterName=" << requesterIdentity.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " count=" << tokenPairCount
                  << " configuredBatch=" << configuredBatch
                  << " requestedBatch=" << (hintIt == requestTokens.end() ?
                                               configuredBatch : tokenPairCount));
    }

    bool ServiceProvider::finishTargetedRequestOnEventLoop(
        const ndn::Name& requesterIdentity,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        RequestMessage requestMessage)
    {
        const auto targetProvider = requestMessage.getTargetProvider();
        if (targetProvider.empty()) {
            publishExecutionFailureOnEventLoop(requesterIdentity,
                                               identity,
                                               serviceName,
                                               requestId,
                                               requestMessage,
                                               "Targeted request missing target provider");
            return true;
        }
        if (!targetProvider.equals(identity)) {
            NDN_LOG_DEBUG("Ignore targeted request for different provider target="
                          << targetProvider.toUri()
                          << " local=" << identity.toUri()
                          << " requestId=" << requestId.toUri());
            return true;
        }

        auto service = m_services.find(serviceName);
        if (service == m_services.end() || !service->second.targetedRequestHandler) {
            publishExecutionFailureOnEventLoop(requesterIdentity,
                                               identity,
                                               serviceName,
                                               requestId,
                                               requestMessage,
                                               "Targeted service has no handler");
            return true;
        }
        std::string tokenError;
        if (!consumeTargetedProviderToken(requesterIdentity,
                                          serviceName,
                                          requestMessage,
                                          tokenError)) {
            publishExecutionFailureOnEventLoop(requesterIdentity,
                                               identity,
                                               serviceName,
                                               requestId,
                                               requestMessage,
                                               tokenError);
            return true;
        }

        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=TARGETED_REQUEST_ACCEPTED timestamp_us="
                  << nowMicroseconds()
                  << " providerName=" << identity.toUri()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri());
        // spec182: fence the targeted acceptance against the registration
        // this entry carried.  A retired (closed) registration refuses
        // execution outright; the pool-0 inline path additionally binds the
        // request so a later re-registration cannot run a successor handler
        // for this acceptance.
        auto registrationState = service->second.registrationState;
        if (registrationState && registrationState->closed) {
            publishExecutionFailureOnEventLoop(requesterIdentity,
                                               identity,
                                               serviceName,
                                               requestId,
                                               requestMessage,
                                               "Service registration closed before execution");
            return true;
        }
        if (!dispatchRequestExecutionAsync(requesterIdentity,
                                           identity,
                                           serviceName,
                                           requestId,
                                           requestMessage,
                                           "",
                                           &registrationState)) {
            const ndn::Name pendingKey = ndn::Name(requesterIdentity)
                                             .append(serviceName).append(requestId);
            const std::string fenceError = registrationState
                ? fencePendingRegistrationExecution(pendingKey, registrationState)
                : std::string();
            if (!fenceError.empty()) {
                publishExecutionFailureOnEventLoop(requesterIdentity,
                                                   identity,
                                                   serviceName,
                                                   requestId,
                                                   requestMessage,
                                                   fenceError);
                return true;
            }
            ResponseMessage response;
            try {
                response = service->second.targetedRequestHandler(requesterIdentity,
                                                                  identity,
                                                                  serviceName,
                                                                  requestId,
                                                                  requestMessage);
            }
            catch (const std::exception& e) {
                response = makeErrorResponse(
                    std::string("Targeted request handler failed: ") + e.what());
            }
            catch (...) {
                response = makeErrorResponse("Targeted request handler failed");
            }
            finishRequestExecutionOnEventLoop(requesterIdentity,
                                              identity,
                                              serviceName,
                                              requestId,
                                              requestMessage,
                                              std::move(response),
                                              "",
                                              registrationState);
        }
        return true;
    }

    bool ServiceProvider::dispatchRequestExecutionAsync(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        RequestMessage requestMessage,
        std::string selectionDigest,
        std::shared_ptr<RegistrationState>* registrationStateOut)
    {
        if (m_handlerPool.getThreadCount() == 0) {
            return false;
        }

        auto service = m_services.find(serviceName);
        if (service == m_services.end()) {
            return false;
        }
        const bool targetedMode =
            requestMessage.getRequestMode() == tlv::TargetedRequest ||
            requestMessage.getRequestMode() == tlv::TargetedBootstrapRequest;
        const bool streamedMode = requestMessage.hasStreamRequestOptions() &&
                                  static_cast<bool>(service->second.streamingHandler);
        auto requestHandler =
            targetedMode
                ? service->second.targetedRequestHandler
                : service->second.requestHandler;
        if (!requestHandler && !streamedMode) {
            return false;
        }
        auto streamingHandler = service->second.streamingHandler;

        // spec182: fence the execution dispatch against the registration the
        // entry carried.  A retired registration refuses execution; the
        // pending binding (established at acceptance for request/selection
        // flows) refuses execution against a successor registration.  Pool-0
        // inline fallback callers receive the state through
        // registrationStateOut so they can apply the same fence.
        auto registrationState = service->second.registrationState;
        if (registrationStateOut) {
            *registrationStateOut = registrationState;
        }
        if (registrationState && registrationState->closed) {
            publishExecutionFailureOnEventLoop(requesterName,
                                               providerName,
                                               serviceName,
                                               requestId,
                                               requestMessage,
                                               "Service registration closed before execution",
                                               std::move(selectionDigest));
            return true;
        }
        if (registrationState) {
            const ndn::Name pendingKey = ndn::Name(requesterName)
                                             .append(serviceName).append(requestId);
            const std::string fenceError =
                fencePendingRegistrationExecution(pendingKey, registrationState);
            if (!fenceError.empty()) {
                publishExecutionFailureOnEventLoop(requesterName,
                                                   providerName,
                                                   serviceName,
                                                   requestId,
                                                   requestMessage,
                                                   fenceError,
                                                   std::move(selectionDigest));
                return true;
            }
        }

        const bool queued = m_handlerPool.post(
            [this,
             requesterName,
             providerName,
             serviceName,
             requestId,
             requestMessage,
             requestHandler = std::move(requestHandler),
             streamingHandler = std::move(streamingHandler),
             targetedMode,
             streamedMode,
             selectionDigest,
             registrationState]() mutable {
                if (registrationState && registrationState->closed) {
                    // spec182: the scoped registration retired while this
                    // task was queued; the worker never runs a retired
                    // registration's handler.
                    publishExecutionFailureOnEventLoop(
                        requesterName, providerName, serviceName, requestId,
                        requestMessage,
                        "Service registration closed before execution",
                        std::move(selectionDigest));
                    return;
                }
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Running,
                                               providerName,
                                               serviceName,
                                               requestId,
                                               "handler running");
                if (streamedMode) {
                    const auto pendingKey = ndn::Name(requesterName)
                        .append(serviceName).append(requestId);
                    std::shared_ptr<StreamEventPublisher> publisher;
                    {
                        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
                        const auto it = m_streamPublishers.find(pendingKey);
                        if (it != m_streamPublishers.end()) publisher = it->second;
                    }
                    if (!publisher && targetedMode &&
                        requestMessage.getStreamRequestOptions().eventKeyGrant) {
                        // Targeted streaming has no Selection packet.  Reuse
                        // the exact grant/binding verifier with a synthetic
                        // one-provider Selection projection so the fast path
                        // cannot bypass stream authorization or replay checks.
                        ServiceSelectionMessage targetedBinding;
                        targetedBinding.setRequestIDs({requestId.toUri()});
                        targetedBinding.setPolicyEpoch(requestMessage.getPolicyEpoch());
                        targetedBinding.setStreamEventKeyGrant(
                            *requestMessage.getStreamRequestOptions().eventKeyGrant);
                        SelectionProviderEntry providerEntry;
                        providerEntry.providerName = providerName;
                        targetedBinding.addProviderEntry(providerEntry);
                        const auto targetedDigest = computeSelectionDigest(targetedBinding);
                        if (!initializeStreamPublisher(
                                requesterName, providerName, serviceName, requestId,
                                requestMessage, targetedBinding, targetedDigest)) {
                            publishExecutionFailureOnEventLoop(
                                requesterName, providerName, serviceName, requestId,
                                requestMessage, "Targeted stream grant rejected",
                                std::move(selectionDigest));
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
                            const auto it = m_streamPublishers.find(pendingKey);
                            if (it != m_streamPublishers.end()) publisher = it->second;
                        }
                    }
                    if (!publisher) {
                        publishExecutionFailureOnEventLoop(
                            requesterName, providerName, serviceName, requestId,
                            requestMessage, "stream publisher unavailable",
                            std::move(selectionDigest));
                        return;
                    }
                    auto lifecycle = getStreamLifecycle(pendingKey);
                    auto core = std::make_shared<StreamedResponseWriterCore>(
                        [publisher](const ndn::Buffer& payload, uint64_t& cursor) {
                            const auto result = publisher->publish(
                                payload, std::chrono::steady_clock::now() +
                                         std::chrono::seconds(30));
                            if (!result) return false;
                            cursor = result->cursor;
                            return true;
                        },
                        [this, publisher, requesterName, providerName, serviceName,
                         requestId, requestMessage, selectionDigest, registrationState]
                        (const ndn::Buffer& payload, StreamFinishReason reason) {
                            const auto completion = publisher->finish(
                                payload, reason, std::chrono::steady_clock::now() +
                                                   std::chrono::seconds(30));
                            if (!completion) return false;
                            ResponseMessage response;
                            response.setStatus(true);
                            auto finalPayload = payload;
                            response.setPayload(finalPayload, finalPayload.size());
                            response.setStreamCompletion(*completion);
                            boost::asio::post(m_face.getIoContext(),
                                [this, requesterName, providerName, serviceName,
                                 requestId, requestMessage, response = std::move(response),
                                 selectionDigest, registrationState]() mutable {
                                    finishRequestExecutionOnEventLoop(
                                        requesterName, providerName, serviceName,
                                        requestId, requestMessage, std::move(response),
                                        selectionDigest, registrationState);
                                });
                            return true;
                        },
                        [this, publisher, requesterName, providerName, serviceName,
                         requestId, requestMessage, selectionDigest]
                        (StreamedInvocationErrorCode, const std::string& message) {
                            publisher->fail(message);
                            boost::asio::post(m_face.getIoContext(),
                                [this, requesterName, providerName, serviceName,
                                 requestId, requestMessage, message, selectionDigest]() mutable {
                                    publishExecutionFailureOnEventLoop(
                                        requesterName, providerName, serviceName, requestId,
                                        requestMessage, message, selectionDigest);
                                });
                            return true;
                        },
                        [lifecycle] {
                            return !lifecycle || lifecycle->terminalAuthority()->isTerminal() ||
                                   lifecycle->terminalAuthority()->isFenced();
                        },
                        [] { return std::chrono::milliseconds(30000); });
                    StreamedResponseWriter<ndn::Buffer, ndn::Buffer> writer(core);
                    try {
                        streamingHandler(requesterName, providerName, serviceName,
                                         requestId, requestMessage, writer);
                        if (!core->isTerminal() && !writer.isCancelled()) {
                            core->fail(StreamedInvocationErrorCode::ProviderFailure,
                                       "stream handler returned without terminal result");
                        }
                    }
                    catch (const std::exception& error) {
                        if (!core->isTerminal()) {
                            core->fail(StreamedInvocationErrorCode::ApplicationCallbackFailed,
                                       error.what());
                        }
                    }
                    catch (...) {
                        if (!core->isTerminal()) {
                            core->fail(StreamedInvocationErrorCode::ApplicationCallbackFailed,
                                       "stream handler threw an unknown exception");
                        }
                    }
                    core->invalidate();
                    return;
                }

                ResponseMessage response;
                try {
                    response = requestHandler(requesterName,
                                              providerName,
                                              serviceName,
                                              requestId,
                                              requestMessage);
                }
                catch (const std::exception& e) {
                    response = makeErrorResponse(
                        std::string("Request handler failed: ") + e.what());
                }
                catch (...) {
                    response = makeErrorResponse("Request handler failed");
                }

                boost::asio::post(m_face.getIoContext(),
                    [this,
                     requesterName,
                     providerName,
                     serviceName,
                     requestId,
                     requestMessage,
                     selectionDigest,
                     response = std::move(response),
                     registrationState]() mutable {
                        finishRequestExecutionOnEventLoop(requesterName,
                                                          providerName,
                                                          serviceName,
                                                          requestId,
                                                          requestMessage,
                                                          std::move(response),
                                                          std::move(selectionDigest),
                                                          std::move(registrationState));
                    });
            });

        if (!queued) {
            publishExecutionFailureOnEventLoop(requesterName,
                                               providerName,
                                               serviceName,
                                               requestId,
                                               requestMessage,
                                               "Request handler queue full",
                                               std::move(selectionDigest));
        }
        return true;
    }

    bool ServiceProvider::dispatchCollaborationExecutionAsync(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        RequestMessage requestMessage,
        CollaborationAssignment assignment,
        std::string selectionDigest)
    {
        auto service = m_collaborationServices.find(serviceName);
        if (service == m_collaborationServices.end() || !service->second.handler) {
            return false;
        }

        // spec182: a scoped collaboration registration that retired refuses
        // dispatch (never run a successor handler for an acceptance made
        // against an earlier generation, and never run a retired handler).
        const auto registrationState = service->second.registrationState;
        if (registrationState && registrationState->closed) {
            publishExecutionFailureOnEventLoop(requesterName, providerName, serviceName,
                requestId, requestMessage,
                "collaboration registration closed before execution",
                selectionDigest);
            return true;
        }
        if (registrationState) {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            const auto boundIt = m_collaborationRegistrationStates.find(requestId);
            if (boundIt != m_collaborationRegistrationStates.end() &&
                boundIt->second != registrationState) {
                publishExecutionFailureOnEventLoop(requesterName, providerName, serviceName,
                    requestId, requestMessage,
                    "collaboration registration generation changed before execution",
                    selectionDigest);
                return true;
            }
        }

        const auto handler = service->second.handler;
        const auto requestVersion = requestMessage.hasControllerVersion()
            ? std::optional<ControllerVersion>(requestMessage.getControllerVersion())
            : std::nullopt;
        if (!isAcceptableControllerVersion(serviceName, requestVersion) ||
            !authorizeControllerTransition(serviceName, ProtectedTransition::PROVIDER_EXECUTION)) {
            publishExecutionFailureOnEventLoop(requesterName, providerName, serviceName,
                requestId, requestMessage, "collaboration admission authority expired",
                selectionDigest);
            return true;
        }
        if (!service->second.allowedRoles.empty() &&
            std::find(service->second.allowedRoles.begin(),
                      service->second.allowedRoles.end(),
                      assignment.role) == service->second.allowedRoles.end()) {
            NDN_LOG_WARN("Reject collaboration assignment for "
                         << serviceName.toUri()
                         << ": role " << assignment.role
                         << " is not registered on provider "
                         << identity.toUri());
            publishExecutionFailureOnEventLoop(
                requesterName,
                providerName,
                serviceName,
                requestId,
                requestMessage,
                "Provider is not authorized for collaboration role " + assignment.role,
                selectionDigest);
            return true;
        }
        if (!service->second.allowedRoles.empty() &&
            !hasProviderCollaborationRolePermission(identity, serviceName,
                                                    assignment.role, m_authorizations)) {
            NDN_LOG_WARN("Reject collaboration assignment for "
                         << serviceName.toUri()
                         << ": role " << assignment.role
                         << " is not authorized by provider permission for "
                         << identity.toUri());
            publishExecutionFailureOnEventLoop(
                requesterName,
                providerName,
                serviceName,
                requestId,
                requestMessage,
                "Provider lacks controller-authorized collaboration role " +
                    assignment.role,
                selectionDigest);
            return true;
        }
        const auto workFence = makeCollaborationWorkFence(
            requesterName, requestId, serviceName,
            requestMessage.hasControllerVersion()
                ? std::optional<ControllerVersion>(requestMessage.getControllerVersion())
                : std::nullopt,
            registrationState);
        const auto current = [workFence, requestMessage] {
            return workFence.current() &&
                (!requestMessage.hasStreamRequestOptions() ||
                 requestMessage.getStreamRequestOptions().deadlineEpochMs > nowMilliseconds());
        };
        prepareCollaborationAssignmentAsync(
            requesterName,
            requestId,
            std::move(assignment),
            [this,
             requesterName,
             providerName,
             serviceName,
             requestId,
             requestMessage,
             selectionDigest,
             current,
             handler](bool ready, std::string error,
                      CollaborationAssignment assignment) mutable {
                const bool traceAssignmentFetch =
                    isTruthyEnv("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE");
                if (traceAssignmentFetch) {
                    NDN_LOG_WARN("NDNSF_COLLAB_HANDLER"
                                 << " event=assignment_ready_callback"
                                 << " requestId=" << requestId.toUri()
                                 << " role=" << assignment.role
                                 << " service=" << serviceName.toUri()
                                 << " ready=" << (ready ? "true" : "false")
                                 << " error=\"" << error << "\"");
                }
                if (!ready || !current()) {
                    if (ready) {
                        error = requestMessage.hasStreamRequestOptions() &&
                            requestMessage.getStreamRequestOptions().deadlineEpochMs <= nowMilliseconds()
                            ? "REQUEST_DEADLINE: collaboration request expired before execution"
                            : "collaboration execution authority or deadline expired";
                    }
                    publishExecutionFailureOnEventLoop(
                        requesterName,
                        providerName,
                        serviceName,
                        requestId,
                        requestMessage,
                        "Collaboration assignment preparation failed: " + error,
                        selectionDigest);
                    return;
                }

                auto runHandler =
                    [this,
                     requesterName,
                     serviceName,
                     requestId,
                     requestMessage,
                     selectionDigest,
                     current,
                     stopping = m_fetchStopping,
                     assignment = std::move(assignment),
                     handler]() mutable {
                        if (stopping->load()) return;
                        if (!current()) {
                            if (stopping->load()) return;
                            boost::asio::post(m_face.getIoContext(),
                                [this, stopping, requesterName, serviceName, requestId,
                                 requestMessage, selectionDigest] {
                                    if (stopping->load()) return;
                                    publishExecutionFailureOnEventLoop(
                                        requesterName, identity, serviceName, requestId,
                                        requestMessage,
                                        requestMessage.hasStreamRequestOptions() &&
                                            requestMessage.getStreamRequestOptions().deadlineEpochMs <= nowMilliseconds()
                                            ? "REQUEST_DEADLINE: collaboration request expired before execution"
                                            : "collaboration execution authority or deadline expired",
                                        selectionDigest);
                                });
                            return;
                        }
                        const bool traceAssignmentFetch =
                            isTruthyEnv("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE");
                        if (traceAssignmentFetch) {
                            NDN_LOG_WARN("NDNSF_COLLAB_HANDLER"
                                         << " event=run_start"
                                         << " requestId=" << requestId.toUri()
                                         << " role=" << assignment.role
                                         << " service=" << serviceName.toUri());
                        }
                        updateSelectionExecutionStatus(selectionDigest,
                                                       SelectionExecutionState::Running,
                                                       identity,
                                                       serviceName,
                                                       requestId,
                                                       "collaboration handler running");
                        try {
                            CollaborationContext ctx(*this,
                                                     requesterName,
                                                     requestId,
                                                     requestMessage,
                                                     std::move(assignment));
                            handler(ctx, requestMessage);
                            if (traceAssignmentFetch) {
                                NDN_LOG_WARN("NDNSF_COLLAB_HANDLER"
                                             << " event=run_done"
                                             << " requestId=" << requestId.toUri()
                                             << " service=" << serviceName.toUri());
                            }
                        }
                        catch (const std::exception& e) {
                            NDN_LOG_ERROR("Collaboration handler failed for "
                                          << serviceName.toUri() << ": " << e.what());
                            updateSelectionExecutionStatus(
                                selectionDigest,
                                SelectionExecutionState::Failed,
                                identity,
                                serviceName,
                                requestId,
                                std::string("Collaboration handler failed: ") +
                                    e.what());
                        }
                        catch (...) {
                            NDN_LOG_ERROR("Collaboration handler failed for "
                                          << serviceName.toUri());
                            updateSelectionExecutionStatus(
                                selectionDigest,
                                SelectionExecutionState::Failed,
                                identity,
                                serviceName,
                                requestId,
                                "Collaboration handler failed");
                        }
                        if (stopping->load()) return;
                        boost::asio::post(m_face.getIoContext(),
                            [this, stopping, requestId, serviceName] {
                                if (stopping->load()) return;
                                updateProviderRequestLifecycleState(
                                    requestId, serviceName,
                                    ProviderRequestLifecycleState::EXECUTION_DONE);
                            });
                    };

                if (m_handlerPool.getThreadCount() == 0) {
                    runHandler();
                    return;
                }

                const bool queued = m_handlerPool.post(std::move(runHandler));
                if (traceAssignmentFetch) {
                    NDN_LOG_WARN("NDNSF_COLLAB_HANDLER"
                                 << " event=queue_post"
                                 << " requestId=" << requestId.toUri()
                                 << " role=" << assignment.role
                                 << " service=" << serviceName.toUri()
                                 << " queued=" << (queued ? "true" : "false"));
                }
                if (!queued) {
                    publishExecutionFailureOnEventLoop(
                        requesterName,
                        providerName,
                        serviceName,
                        requestId,
                        requestMessage,
                        "Collaboration handler queue full",
                        selectionDigest);
                }
            },
            registrationState);
        return true;
    }

    LargeDataReferenceResponseResult
    ServiceProvider::makeRequestScopedResponseWithLargeDataOptimization(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        ResponseMessage response,
        const RequestKeyBundle& keys,
        RequestSecurityBinding binding,
        size_t thresholdBytes,
        ndn::time::milliseconds freshness)
    {
        LargeDataReferenceResponseResult result;
        const auto threshold = thresholdBytes == 0 ?
            responseLargeDataThresholdBytes() : thresholdBytes;
        const auto payload = response.getPayload();
        if (!response.getStatus() ||
            threshold == 0 ||
            payload.size() <= threshold ||
            isLargeDataReferencePayload(payload)) {
            result.responseMessage = std::move(response);
            result.success = true;
            return result;
        }

        if (requesterName.empty() || providerName.empty() || serviceName.empty() ||
            requestId.empty() || !keys.isValid(nowMilliseconds()) ||
            !binding.isValid()) {
            result.errorMessage =
                "request-scoped large response requires valid names, binding, and keys";
            return result;
        }
        if (binding.serviceName != serviceName ||
            binding.requestId != requestId ||
            binding.providerEncryptionCertName.empty()) {
            result.errorMessage = "request-scoped large response binding mismatch";
            return result;
        }
        try {
            if (ndn::security::extractIdentityFromCertName(
                    binding.providerEncryptionCertName) != providerName) {
                result.errorMessage =
                    "request-scoped large response provider certificate mismatch";
                return result;
            }
        }
        catch (const std::exception&) {
            result.errorMessage =
                "request-scoped large response provider certificate is invalid";
            return result;
        }

        result.largeData.objectId = sanitizeLargeDataObjectId(
            "response-" + requestId.toUri());
        ndn::Name encryptedDataName = makeLargeResponseDataName(
            providerName, requesterName, serviceName, requestId,
            result.largeData.objectId);
        encryptedDataName.appendVersion();

        const auto chunkBytes = requestScopedResponseChunkBytes(threshold);
        result.largeData.digest = sha256DigestString(payload);
        size_t segmentCount = 0;
        std::string publicationStage = "initialization";
        try {
            for (size_t offset = 0; offset < payload.size(); offset += chunkBytes) {
                const auto length = std::min(chunkBytes, payload.size() - offset);
                auto segmentBinding = binding;
                segmentBinding.segmentOrEventId =
                    "response/" + std::to_string(segmentCount);
                publicationStage = "encrypt";
                const auto envelope = encryptRequestContent(
                    keys.responseKey,
                    keys.keyId,
                    segmentBinding,
                    ndn::span<const uint8_t>(payload.data() + offset, length));
                const auto envelopeWire = envelope.wireEncode();
                // InMemoryStorageFifo retains the Data through
                // enable_shared_from_this; a stack Data would throw
                // std::bad_weak_ptr on insert.  Keep the signed segment in a
                // shared owner for both IMS retention and subsequent fetches.
                auto data = std::make_shared<ndn::Data>(
                    ndn::Name(encryptedDataName).appendSegment(segmentCount));
                data->setContent(envelopeWire);
                data->setFreshnessPeriod(freshness);
                publicationStage = "sign";
                auto& activeKeyChain = m_testSigningKeyChain ?
                    *m_testSigningKeyChain : m_keyChain;
                activeKeyChain.sign(*data, m_signingInfo);
                publicationStage = "insert-ims";
                insertDataIntoIMS(*data, freshness);
                ++segmentCount;
            }
        }
        catch (const std::exception& e) {
            result.errorMessage = std::string(
                "request-scoped large response publication failed at ") +
                publicationStage + ": " + e.what();
            return result;
        }
        if (segmentCount == 0) {
            result.errorMessage = "request-scoped large response produced no segments";
            return result;
        }

        result.largeData.encryptedDataName = encryptedDataName;
        LargeDataReference reference;
        reference.dataName = encryptedDataName;
        reference.objectType = "ndnsf-response";
        reference.objectId = result.largeData.objectId;
        reference.plaintextSize = payload.size();
        reference.encrypted = true;
        reference.digest = result.largeData.digest;
        reference.keyScope = "request";
        auto referencePayload = encodeLargeDataReferencePayload(reference);
        response.setPayload(referencePayload, referencePayload.size());
        response.clearAeadEnvelope();
        response.setControllerVersion(binding.controllerVersion);

        result.responseMessage = std::move(response);
        result.usedLargeDataReference = true;
        result.success = true;
        NDN_LOG_INFO("NDNSF_REQUEST_SCOPED_LARGE_RESPONSE_PUBLISHED"
                     << " name=" << encryptedDataName.toUri()
                     << " requestId=" << requestId.toUri()
                     << " serviceName=" << serviceName.toUri()
                     << " plaintextBytes=" << payload.size()
                     << " chunkBytes=" << chunkBytes
                     << " segments=" << segmentCount);
        return result;
    }

    void ServiceProvider::finishRequestExecutionOnEventLoop(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage,
        ResponseMessage response,
        std::string selectionDigest,
        std::shared_ptr<RegistrationState> registrationState)
    {
        auto releaseR1Reservation = [this, &requesterName, &serviceName,
                                     &requestId](const std::string& cause) {
            const ndn::Name key = ndn::Name(requesterName).append(serviceName).append(requestId);
            std::string reservationId;
            {
                std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
                const auto found = m_r1ReservationByRequest.find(key);
                if (found == m_r1ReservationByRequest.end()) return;
                reservationId = found->second;
                m_r1ReservationByRequest.erase(found);
            }
            const auto handler = m_r1ReservationTerminalHandlers.find(serviceName);
            if (handler != m_r1ReservationTerminalHandlers.end()) {
                try { handler->second(reservationId, cause); }
                catch (const std::exception& e) {
                    NDN_LOG_ERROR("R1 reservation terminal release failed reservation="
                                  << reservationId << " cause=" << cause
                                  << " error=" << e.what());
                }
            }
        };
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PROVIDER_EXECUTE_DONE timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " requesterName=" << requesterName.toUri()
                  << " providerName=" << providerName.toUri()
                  << " status=" << response.getStatus()
                  << " handlerQueueDepth=" << m_handlerPool.getQueueSize());
        if (m_timelineTrace) {
            logTimelineTrace("provider", "service_execution_done", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"providerName", providerName.toUri()},
                              {"status", response.getStatus() ? "true" : "false"}});
        }
        updateProviderRequestLifecycleState(
            requestId, serviceName,
            ProviderRequestLifecycleState::EXECUTION_DONE);
        if (registrationState && registrationState->closed &&
            response.getStatus()) {
            // spec182: the scoped registration retired while the handler ran
            // or the response was in flight.  A retired registration must
            // never commit positive work in its name; degrade to the same
            // error the negative paths publish.
            response = makeErrorResponse(
                "Service registration closed before response committed");
        }
        if (m_useTokens) {
            response.setUserToken(requestMessage.getUserToken());
        }
        auto registeredService = m_services.find(serviceName);
        if (requestMessage.getRequestMode() == tlv::TargetedBootstrapRequest &&
            registeredService != m_services.end() &&
            registeredService->second.targetedRequestHandler) {
            attachTargetedTokenBatch(requesterName, serviceName, requestMessage, response);
        }
        const ndn::Name pendingKey = ndn::Name(requesterName)
            .append(serviceName).append(requestId);
        std::optional<RequestScopedInvocationState> requestScopedState;
        {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            const auto stateIt = m_requestScopedInvocations.find(pendingKey);
            if (stateIt != m_requestScopedInvocations.end()) {
                requestScopedState = stateIt->second;
            }
        }
        const bool requestScopedResponse = requestScopedState.has_value();
        LargeDataReferenceResponseResult optimizedResponse;
        if (requestScopedResponse) {
            auto binding = requestScopedState->binding;
            const auto payload = response.getPayload();
            const auto threshold = responseLargeDataThresholdBytes();
            if (response.getStatus() && threshold > 0 &&
                payload.size() > threshold &&
                !isLargeDataReferencePayload(payload)) {
                optimizedResponse = makeRequestScopedResponseWithLargeDataOptimization(
                    requesterName,
                    providerName,
                    serviceName,
                    requestId,
                    std::move(response),
                    requestScopedState->keys,
                    std::move(binding));
            }
            else {
                binding.segmentOrEventId = "response";
                const auto envelope = encryptRequestContent(
                    requestScopedState->keys.responseKey,
                    requestScopedState->keys.keyId,
                    binding,
                    ndn::span<const uint8_t>(payload.data(), payload.size()));
                const auto encoded = envelope.wireEncode();
                ndn::Block emptyPayload(tlv::PayloadType);
                emptyPayload.encode();
                response.setPayloadBlock(emptyPayload);
                response.setAeadEnvelope(encoded);
                response.setControllerVersion(binding.controllerVersion);
                optimizedResponse.success = true;
                optimizedResponse.responseMessage = std::move(response);
                NDN_LOG_INFO("NDNSF_REQUEST_SCOPED_RESPONSE_ENCRYPTED requestId="
                             << requestId.toUri()
                             << " providerName=" << providerName.toUri()
                             << " plaintextBytes=" << payload.size()
                             << " keyId=" << requestScopedState->keys.keyId);
            }
        }
        else {
            // Spec179 migration (T012): the old service-wide response-key
            // carrier was removed once the request-scoped default and its
            // MiniNDN gate passed.  A large response with no request-scoped
            // invocation state can no longer be delivered confidentially;
            // fail closed with a typed error instead of silently falling
            // back to plaintext or resurrecting a service-wide ABE carrier.
            const auto legacyPayload = response.getPayload();
            const auto legacyThreshold = responseLargeDataThresholdBytes();
            if (response.getStatus() && legacyThreshold > 0 &&
                legacyPayload.size() > legacyThreshold &&
                !isLargeDataReferencePayload(legacyPayload)) {
                optimizedResponse.success = false;
                optimizedResponse.errorMessage =
                    "large response requires request-scoped confidentiality "
                    "(service-wide response-key carrier removed)";
            }
            else {
                optimizedResponse.success = true;
                optimizedResponse.responseMessage = std::move(response);
            }
        }
        if (!optimizedResponse.success) {
            NDN_LOG_ERROR("Failed to prepare large response reference requestId="
                          << requestId.toUri()
                          << " serviceName=" << serviceName.toUri()
                          << " error=" << optimizedResponse.errorMessage);
            response = makeErrorResponse("large response reference preparation failed: " +
                                         optimizedResponse.errorMessage);
            if (m_useTokens) {
                response.setUserToken(requestMessage.getUserToken());
            }
        }
        else {
            response = std::move(optimizedResponse.responseMessage);
        }
        response.setPolicyEpoch(getCurrentPolicyEpoch(serviceName));
        if (requestScopedResponse) {
            // The Response metadata is part of the authenticated invocation
            // contract.  Do not overwrite the version used in the AEAD AAD
            // with a newer process-wide status observed while the handler was
            // running; the User must either accept this exact invocation
            // version or reject it as stale.
            response.setControllerVersion(requestScopedState->binding.controllerVersion);
        }
        else if (const auto version = getControllerVersion(serviceName)) {
            response.setControllerVersion(*version);
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=RESPONSE_DISPATCHED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " requesterName=" << requesterName.toUri()
                  << " providerName=" << providerName.toUri()
                  << " status=" << response.getStatus());
        ndn::Name responseName = makeResponseNameV2(providerName,
                                                    requesterName,
                                                    serviceName,
                                                    requestId);
        ndn::Name responseNameWithoutPrefix =
            makeResponseNameWithoutPrefixV2(requesterName,
                                            serviceName,
                                            requestId);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=RESPONSE_PUBLISH_ATTEMPT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " responseName=" << responseName.toUri());
        try {
            PublishMessage(responseName, responseNameWithoutPrefix, response);
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=RESPONSE_PUBLISHED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " responseName=" << responseName.toUri());
            updateProviderRequestLifecycleState(
                requestId, serviceName,
                ProviderRequestLifecycleState::RESPONSE_PUBLISHED);
            updateSelectionExecutionStatus(selectionDigest,
                                           response.getStatus() ?
                                               SelectionExecutionState::Completed :
                                               SelectionExecutionState::Failed,
                                           providerName,
                                           serviceName,
                                           requestId,
                                           response.getStatus() ?
                                               "response published" :
                                               response.getErrorInfo(),
                                           responseName);
            size_t selectedOutstanding =
                m_selectedOutstandingRequests.load(std::memory_order_relaxed);
            while (selectedOutstanding > 0 &&
                   !m_selectedOutstandingRequests.compare_exchange_weak(
                       selectedOutstanding,
                       selectedOutstanding - 1,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
            releaseR1Reservation(response.getStatus() ? "LOCAL_COMPLETE" :
                                                        "EXECUTION_FAILED");
        }
        catch (const std::exception& e) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=RESPONSE_PUBLISH_FAILED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " responseName=" << responseName.toUri()
                      << " error=" << e.what());
            size_t selectedOutstanding =
                m_selectedOutstandingRequests.load(std::memory_order_relaxed);
            while (selectedOutstanding > 0 &&
                   !m_selectedOutstandingRequests.compare_exchange_weak(
                       selectedOutstanding,
                       selectedOutstanding - 1,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
            updateSelectionExecutionStatus(selectionDigest,
                                           SelectionExecutionState::Failed,
                                           providerName,
                                           serviceName,
                                           requestId,
                                           std::string("response publish failed: ") + e.what(),
                                           responseName);
            releaseR1Reservation("RESPONSE_PUBLISH_FAILED");
            throw;
        }
        if (requestScopedResponse) {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            const auto stateIt = m_requestScopedInvocations.find(pendingKey);
            if (stateIt != m_requestScopedInvocations.end()) {
                stateIt->second.keys.zeroize();
                m_requestScopedInvocations.erase(stateIt);
            }
            m_requestScopedNonceRegistry.invalidate(requestScopedState->keys.keyId);
        }
    }

    void ServiceProvider::fetchRequestScopedInputAndDispatch(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        RequestMessage requestMessage,
        const ServiceSelectionMessage& selectionMessage,
        const ndn::Buffer& assignmentPayload,
        const std::string& selectionDigest)
    {
        const ndn::Name pendingKey = ndn::Name(requesterName)
            .append(serviceName).append(requestId);
        auto completed = std::make_shared<std::atomic_bool>(false);
        const ndn::Buffer assignmentPayloadCopy = assignmentPayload;

        const auto finishFailure =
            [this, requesterName, providerName, serviceName, requestId,
             requestMessage, selectionDigest, completed](std::string reason) {
                if (completed->exchange(true)) {
                    return;
                }
                NDN_LOG_WARN("NDNSF_REQUEST_SCOPED_INPUT_REJECTED requestId="
                             << requestId.toUri()
                             << " providerName=" << providerName.toUri()
                             << " reason=" << reason);
                boost::asio::post(m_face.getIoContext(),
                    [this, requesterName, providerName, serviceName, requestId,
                     requestMessage, selectionDigest,
                     reason = std::move(reason)]() mutable {
                        publishExecutionFailureOnEventLoop(
                            requesterName, providerName, serviceName, requestId,
                            requestMessage,
                            "request-scoped input unavailable: " + reason,
                            selectionDigest);
                    });
            };

        if (!requestMessage.hasRequestCapabilities() ||
            !requestMessage.getRequestCapabilities().hasField(
                "RequestScopedConfidentialityV1") ||
            requestMessage.getRequestCapabilities().getField(
                "RequestScopedConfidentialityV1") != "required" ||
            !selectionMessage.hasSelectionKeyEnvelope() ||
            !selectionMessage.hasControllerVersion() ||
            !requestMessage.hasUserEncryptionCertificate()) {
            finishFailure("missing request-scoped capability, certificate, version, or envelope");
            return;
        }

        const auto& userAdvertisement =
            requestMessage.getUserEncryptionCertificate();
        const auto nowMs = nowMilliseconds();
        if (!userAdvertisement.isValid(nowMs)) {
            finishFailure("invalid or expired User encryption certificate advertisement");
            return;
        }

        const auto publicKey = identityCert.getPublicKey();
        ndn::Buffer publicKeyBuffer(publicKey.begin(), publicKey.end());
        RequestSecurityBinding expected;
        expected.serviceName = serviceName;
        expected.requestId = requestId;
        expected.attempt = selectionMessage.getAttempt();
        expected.controllerVersion = selectionMessage.getControllerVersion();
        expected.userEncryptionCertName = userAdvertisement.certificateName;
        expected.userEncryptionCertDigest = userAdvertisement.certificateDigest;
        expected.providerEncryptionCertName = identityCert.getName();
        expected.providerEncryptionCertDigest = sha256DigestString(publicKeyBuffer);
        expected.selectionDigest =
            computeSelectionDigestWithoutKeyEnvelope(selectionMessage);
        expected.inputDataName = ndn::Name(requesterName)
            .append("NDNSF").append("DI").append("REQUEST-INPUT")
            .append(serviceName).append(requestId)
            .appendNumber(expected.attempt)
            // Request-scoped keys and AAD are Provider-specific.  Mirror the
            // User's Provider component in the exact input name so a
            // collaboration cannot collide several encrypted inputs.
            .append("PROVIDER").append(providerName);
        expected.segmentOrEventId = "request-input";

        SelectionKeyEnvelope envelope;
        if (!envelope.wireDecode(selectionMessage.getSelectionKeyEnvelope())) {
            finishFailure("malformed SelectionKeyEnvelope");
            return;
        }
        RequestKeyBundle keys;
        RequestCryptoFailure failure = RequestCryptoFailure::NONE;
        auto& activeKeyChain = m_testSigningKeyChain ?
            *m_testSigningKeyChain : m_keyChain;
        if (!unwrapSelectionKeyEnvelope(
                envelope, expected, identityCert.getName(), activeKeyChain,
                nowMs, keys, &failure)) {
            finishFailure(std::string("Selection key envelope rejected: ") +
                          requestCryptoFailureName(failure));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            if (m_requestScopedInvocations.find(pendingKey) !=
                m_requestScopedInvocations.end()) {
                finishFailure("request-scoped key bundle replayed");
                return;
            }
            m_requestScopedInvocations.emplace(
                pendingKey, RequestScopedInvocationState{keys, expected});
        }

        ndn::Interest interest(expected.inputDataName);
        interest.setCanBePrefix(false);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::milliseconds(1000));
        NDN_LOG_INFO("NDNSF_REQUEST_SCOPED_INPUT_FETCH requestId="
                     << requestId.toUri()
                     << " providerName=" << providerName.toUri()
                     << " dataName=" << expected.inputDataName.toUri());
        m_face.expressInterest(
            interest,
            [this, requesterName, providerName, serviceName, requestId,
             requestMessage, selectionMessage, expected, keys, selectionDigest, completed,
             assignmentPayloadCopy,
             finishFailure](const ndn::Interest&, const ndn::Data& data) mutable {
                if (completed->load()) {
                    return;
                }
                if (data.getName() != expected.inputDataName) {
                    finishFailure("exact input Data name mismatch");
                    return;
                }
                validator->validate(
                    data,
                    [this, requesterName, providerName, serviceName, requestId,
                     requestMessage, selectionMessage, expected, keys, selectionDigest, completed,
                     assignmentPayloadCopy,
                     finishFailure](const ndn::Data& validated) mutable {
                        if (validated.getName() != expected.inputDataName ||
                            !isSignedByIdentity(validated, requesterName)) {
                            finishFailure("input Data signer identity mismatch");
                            return;
                        }
                        AeadEnvelope envelope;
                        const auto& content = validated.getContent();
                        bool decoded = false;
                        try {
                            auto [ok, block] = ndn::Block::fromBuffer(
                                ndn::span<const uint8_t>(content.value(),
                                                          content.value_size()));
                            decoded = ok && envelope.wireDecode(block);
                        }
                        catch (const std::exception&) {
                            decoded = false;
                        }
                        if (!decoded) {
                            finishFailure("malformed input AEAD envelope");
                            return;
                        }
                        const auto nowMs = nowMilliseconds();
                        const auto lifetimeMs = std::chrono::milliseconds(
                            std::max<uint64_t>(1, keys.expiresAtMs > nowMs ?
                                keys.expiresAtMs - nowMs : 1));
                        ndn::Buffer plaintext;
                        RequestCryptoFailure failure = RequestCryptoFailure::NONE;
                        if (!decryptRequestContent(
                                keys.inputKey, keys.keyId, expected, envelope,
                                plaintext, &failure)) {
                            finishFailure(std::string("input AEAD rejected: ") +
                                          requestCryptoFailureName(failure));
                            return;
                        }
                        // Only authenticated input may consume replay state;
                        // otherwise a forged packet can poison the nonce and
                        // reject the later valid publication.
                        if (!m_requestScopedNonceRegistry.reserve(
                                keys.keyId,
                                ndn::span<const uint8_t>(envelope.nonce.data(),
                                                         envelope.nonce.size()),
                                lifetimeMs)) {
                            finishFailure("input AEAD nonce replayed");
                            return;
                        }
                        if (completed->exchange(true)) {
                            return;
                        }
                        boost::asio::post(m_face.getIoContext(),
                            [this, requesterName, providerName, serviceName,
                             requestId, requestMessage, selectionMessage,
                             assignmentPayloadCopy,
                             plaintext = std::move(plaintext), selectionDigest]() mutable {
                                RequestMessage readyRequest(requestMessage);
                                readyRequest.setPayload(plaintext, plaintext.size());
                                if ((!hasService(serviceName) &&
                                     m_collaborationServices.find(serviceName) ==
                                         m_collaborationServices.end()) ||
                                    readyRequest.hasDeploymentIntent()) {
                                    publishExecutionFailureOnEventLoop(
                                        requesterName, providerName, serviceName,
                                        requestId, readyRequest,
                                        "request-scoped confidentiality requires a unary service handler",
                                        selectionDigest);
                                    return;
                                }
                                if (readyRequest.hasStreamRequestOptions()) {
                                    // The request-scoped input has now been
                                    // authenticated.  Initialize the normal
                                    // stream publisher from the already
                                    // authenticated Selection grant before
                                    // entering the worker handler; this keeps
                                    // event-key delivery bound to the same
                                    // request/selection as the encrypted input.
                                    if (!initializeStreamPublisher(
                                            requesterName, providerName, serviceName,
                                            requestId, readyRequest, selectionMessage,
                                            selectionDigest)) {
                                        publishExecutionFailureOnEventLoop(
                                            requesterName, providerName, serviceName,
                                            requestId, readyRequest,
                                            "request-scoped stream grant rejected",
                                            selectionDigest);
                                        return;
                                    }
                                }
                                const auto collaboration =
                                    m_collaborationServices.find(serviceName);
                                if (collaboration != m_collaborationServices.end()) {
                                    try {
                                        auto assignment = parseCollaborationAssignment(
                                            serviceName, assignmentPayloadCopy);
                                        assignment.selectionDigest = selectionDigest;
                                        if (dispatchCollaborationExecutionAsync(
                                                requesterName, providerName, serviceName,
                                                requestId, readyRequest,
                                                std::move(assignment), selectionDigest)) {
                                            return;
                                        }
                                    }
                                    catch (const std::exception& error) {
                                        publishExecutionFailureOnEventLoop(
                                            requesterName, providerName, serviceName,
                                            requestId, readyRequest,
                                            std::string("request-scoped collaboration assignment rejected: ") +
                                                error.what(),
                                            selectionDigest);
                                        return;
                                    }
                                    publishExecutionFailureOnEventLoop(
                                        requesterName, providerName, serviceName,
                                        requestId, readyRequest,
                                        "request-scoped collaboration handler unavailable",
                                        selectionDigest);
                                    return;
                                }
                                std::shared_ptr<RegistrationState>
                                    inlineRegistrationState;
                                if (dispatchRequestExecutionAsync(
                                        requesterName, providerName, serviceName,
                                        requestId, readyRequest, selectionDigest,
                                        &inlineRegistrationState)) {
                                    return;
                                }
                                // spec182: a pool-0 inline dispatch applies
                                // the same generation fence as the async
                                // path; refusal already published its
                                // failure.
                                if (!gateInlineRequestExecution(
                                        requesterName, providerName, serviceName,
                                        requestId, readyRequest, selectionDigest,
                                        inlineRegistrationState)) {
                                    return;
                                }
                                auto response = dispatchRequest(
                                    requesterName, providerName, serviceName,
                                    requestId, readyRequest);
                                finishRequestExecutionOnEventLoop(
                                    requesterName, providerName, serviceName,
                                    requestId, readyRequest, std::move(response),
                                    selectionDigest, inlineRegistrationState);
                            });
                    },
                    [finishFailure](const ndn::Data&,
                                    const ndn::security::ValidationError& error) {
                        finishFailure("input Data signature validation failed: " +
                                      error.getInfo());
                    });
            },
            [finishFailure](const ndn::Interest&, const ndn::lp::Nack& nack) {
                finishFailure("input Data Nack: " +
                              std::to_string(static_cast<int>(nack.getReason())));
            },
            [finishFailure](const ndn::Interest&) {
                finishFailure("input Data fetch timeout");
            });
    }

    void ServiceProvider::publishExecutionFailureOnEventLoop(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage,
        const std::string& error,
        std::string selectionDigest)
    {
        ResponseMessage response = makeErrorResponse(error);
        finishRequestExecutionOnEventLoop(requesterName,
                                          providerName,
                                          serviceName,
                                          requestId,
                                          requestMessage,
                                          std::move(response),
                                          std::move(selectionDigest));
    }

    void ServiceProvider::completeCollaborationRoleOnEventLoop(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        std::string selectionDigest)
    {
        const auto pendingKey = ndn::Name(requesterName)
            .append(serviceName).append(requestId);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=COLLAB_ROLE_COMPLETE "
                      << "timestamp_us=" << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " providerName=" << providerName.toUri());
        updateProviderRequestLifecycleState(
            requestId, serviceName,
            ProviderRequestLifecycleState::EXECUTION_DONE,
            {}, "role complete; final response owned by another role");
        updateSelectionExecutionStatus(
            selectionDigest,
            SelectionExecutionState::Completed,
            providerName,
            serviceName,
            requestId,
            "collaboration role complete; no terminal response");
        size_t selectedOutstanding =
            m_selectedOutstandingRequests.load(std::memory_order_relaxed);
        while (selectedOutstanding > 0 &&
               !m_selectedOutstandingRequests.compare_exchange_weak(
                   selectedOutstanding,
                   selectedOutstanding - 1,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        cleanupPendingRequestState(pendingKey);
    }

    void ServiceProvider::publishCollaborationData(
        const ndn::Name& requesterName,
        const ndn::Name& requestId,
        const std::string& producerRole,
        const std::string& keyScope,
        const ndn::Name& topic,
        const ndn::Buffer& payload)
    {
        const uint64_t sequence =
            m_collaborationSequence.fetch_add(1, std::memory_order_relaxed);
        CollaborationDataMessage message;
        message.setKeyScope(keyScope);
        message.setTopic(topic);
        message.setProducerRole(producerRole);
        message.setSequence(sequence);
        message.setPayload(payload);

        ndn::Name name = makeCollaborationDataName(identity,
                                                   requesterName,
                                                   requestId,
                                                   keyScope,
                                                   topic,
                                                   sequence);
        ndn::Buffer scopeKey;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            auto requestIt = m_collaborationScopeKeysByRequest.find(requestId);
            if (requestIt != m_collaborationScopeKeysByRequest.end()) {
                auto keyIt = requestIt->second.find(keyScope);
                if (keyIt != requestIt->second.end()) {
                    scopeKey = keyIt->second;
                }
            }
        }
        if (scopeKey.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
            NDN_LOG_ERROR("Missing collaboration scope key for request "
                          << requestId.toUri() << " scope=" << keyScope);
            return;
        }

        const bool collaborationAuthTrace =
            isTruthyEnv("NDNSF_COLLAB_AUTH_TRACE");
        if (collaborationAuthTrace) {
            NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=publish_queued"
                         << " provider=" << identity.toUri()
                         << " requestId=" << requestId.toUri()
                         << " dataName=" << name.toUri()
                         << " keyScope=" << keyScope
                         << " topic=" << topic.toUri()
                         << " producerRole=" << producerRole
                         << " sequence=" << sequence
                         << " payloadBytes=" << payload.size());
        }

        auto encryptAndPublish = [this,
                                  name,
                                  requestId,
                                  scopeKey = std::move(scopeKey),
                                  plaintext = payload,
                                  message = std::move(message),
                                  collaborationAuthTrace]() mutable {
            HybridMessageEnvelope envelope;
            const std::string keyId = "collab|" + requestId.toUri() + "|" +
                                      message.getKeyScope();
            const std::string epochId = "session";
            envelope.setKeyId(keyId);
            envelope.setEpochId(epochId);
            envelope.setMessageType("COLLAB");

            std::string error;
            ndn::Buffer encoded;
            try {
                auto ad = collaborationAssociatedData(name, requestId,
                                                      message, keyId, epochId);
                if (isTruthyEnv("NDNSF_COLLAB_AUTH_TRACE")) {
                    NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=encrypt"
                                 << " provider=" << identity.toUri()
                                 << " requestId=" << requestId.toUri()
                                 << " dataName=" << name.toUri()
                                 << " keyScope=" << message.getKeyScope()
                                 << " producerRole=" << message.getProducerRole()
                                 << " sequence=" << message.getSequence()
                                 << " keyDigest=" << sha256DigestString(scopeKey)
                                 << " adDigest=" << sha256DigestString(ad)
                                 << " keyId=" << keyId
                                 << " epochId=" << epochId);
                }
                auto encrypted = hybridAesGcmEncrypt(
                    scopeKey,
                    ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                    ndn::span<const uint8_t>(ad.data(), ad.size()));
                envelope.setNonce(encrypted.nonce);
                envelope.setCipherText(encrypted.ciphertext);
                envelope.setAuthTag(encrypted.tag);
                auto envelopeBlock = envelope.WireEncode();
                message.setPayload(ndn::Buffer(envelopeBlock.begin(),
                                               envelopeBlock.end()));
                auto block = message.WireEncode();
                encoded = ndn::Buffer(block.begin(), block.end());
                if (collaborationAuthTrace) {
                    NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=encrypt_done"
                                 << " provider=" << identity.toUri()
                                 << " requestId=" << requestId.toUri()
                                 << " dataName=" << name.toUri()
                                 << " encodedBytes=" << encoded.size());
                }
            }
            catch (const std::exception& e) {
                error = e.what();
            }

            boost::asio::post(m_face.getIoContext(),
                [this, name, encoded = std::move(encoded),
                 requestId, collaborationAuthTrace,
                 error = std::move(error)]() mutable {
                    if (!error.empty()) {
                        NDN_LOG_ERROR("Collaboration data encryption failed for "
                                      << name.toUri() << ": " << error);
                        return;
                    }
                    ndn::Block block(encoded);
                    if (collaborationAuthTrace) {
                        NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=svs_publish_begin"
                                     << " provider=" << identity.toUri()
                                     << " requestId=" << requestId.toUri()
                                     << " dataName=" << name.toUri());
                    }
                    publishSvs(m_svsps, name, block);
                    if (collaborationAuthTrace) {
                        NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=svs_publish_done"
                                     << " provider=" << identity.toUri()
                                     << " requestId=" << requestId.toUri()
                                     << " dataName=" << name.toUri());
                    }
                });
        };
        // Control-plane records are commonly published by a collaboration
        // handler that then waits for the next control message.  Do not queue
        // their small bounded encryption job on the same handler pool: with
        // one worker that would make the publication wait behind the caller
        // itself, delaying the receipt until the handler times out.  The
        // encrypted packet is still handed to the Face event loop below.
        encryptAndPublish();
    }

    ndn::Name ServiceProvider::publishCollaborationLargeData(
        const ndn::Name& requesterName,
        const ndn::Name& requestId,
        const std::string& producerRole,
        const std::string& keyScope,
        const ndn::Name& topic,
        const ndn::Buffer& payload,
        size_t maxSegmentSize,
        int freshnessMs)
    {
        const uint64_t sequence =
            m_collaborationSequence.fetch_add(1, std::memory_order_relaxed);
        ndn::Name name = makeCollaborationDataName(identity,
                                                   requesterName,
                                                   requestId,
                                                   keyScope,
                                                   topic,
                                                   sequence);
        name.append("large").appendVersion();

        ndn::Buffer scopeKey;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            auto requestIt = m_collaborationScopeKeysByRequest.find(requestId);
            if (requestIt != m_collaborationScopeKeysByRequest.end()) {
                auto keyIt = requestIt->second.find(keyScope);
                if (keyIt != requestIt->second.end()) {
                    scopeKey = keyIt->second;
                }
            }
        }
        if (scopeKey.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
            NDN_LOG_ERROR("Missing collaboration scope key for large Data "
                          << requestId.toUri() << " scope=" << keyScope);
            return {};
        }

        HybridMessageEnvelope envelope;
        const std::string keyId = "collab-large|" + requestId.toUri() + "|" + keyScope;
        envelope.setKeyId(keyId);
        envelope.setEpochId("session");
        envelope.setMessageType("COLLAB-LARGE");
        const std::string adText = name.toUri() + "|COLLAB-LARGE|" +
                                   requestId.toUri() + "|" + keyScope;
        const ndn::Buffer ad(reinterpret_cast<const uint8_t*>(adText.data()), adText.size());
        auto encrypted = hybridAesGcmEncrypt(
            scopeKey,
            ndn::span<const uint8_t>(payload.data(), payload.size()),
            ndn::span<const uint8_t>(ad.data(), ad.size()));
        envelope.setNonce(encrypted.nonce);
        envelope.setCipherText(encrypted.ciphertext);
        envelope.setAuthTag(encrypted.tag);
        auto block = envelope.WireEncode();
        ndn::Buffer encoded(block.begin(), block.end());

        auto& activeKeyChain = m_testSigningKeyChain ?
            *m_testSigningKeyChain : m_keyChain;
        ndn::Segmenter segmenter(activeKeyChain, m_signingInfo);
        auto segments = segmenter.segment(
            ndn::span<const uint8_t>(encoded.data(), encoded.size()),
            name,
            maxSegmentSize == 0 ? 7000 : maxSegmentSize,
            ndn::time::milliseconds(freshnessMs <= 0 ? 60000 : freshnessMs));

        const bool activePut =
            boolEnvOrDefault("NDNSF_COLLAB_LARGE_ACTIVE_PUT", true);
        const bool fetchTimingEnabled = isTruthyEnv("NDNSF_COLLAB_LARGE_FETCH_TIMING");
        for (const auto& data : segments) {
            insertDataIntoIMS(*data, ndn::time::milliseconds(freshnessMs <= 0 ? 60000 : freshnessMs));
            if (activePut) {
                m_face.put(*data);
                if (fetchTimingEnabled) {
                    NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING"
                                 << " event=segment_active_put"
                                 << " mode=producer-active-put"
                                 << " timestamp_us=" << nowMicroseconds()
                                 << " requestId=" << requestId.toUri()
                                 << " keyScope=" << keyScope
                                 << " dataName=" << name.toUri()
                                 << " segmentName=" << data->getName().toUri()
                                 << " wire_bytes=" << data->wireEncode().size());
                }
            }
        }
        NDN_LOG_DEBUG("COLLAB_LARGE_PUBLISHED name=" << name.toUri()
                      << " plaintextBytes=" << payload.size()
                      << " segments=" << segments.size()
                      << " activePut=" << activePut);
        return name;
    }

    ndn::Name ServiceProvider::publishCollaborationLargeDataNamed(
        const ndn::Name& requestId,
        const std::string& keyScope,
        const ndn::Name& dataName,
        const ndn::Buffer& payload,
        size_t maxSegmentSize,
        int freshnessMs)
    {
        if (dataName.empty()) {
            NDN_LOG_ERROR("Cannot publish collaboration large Data with empty name");
            return {};
        }
        if (!identity.isPrefixOf(dataName)) {
            NDN_LOG_ERROR("Collaboration large Data name " << dataName.toUri()
                          << " is outside provider identity " << identity.toUri());
            return {};
        }

        ndn::Buffer scopeKey;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            auto requestIt = m_collaborationScopeKeysByRequest.find(requestId);
            if (requestIt != m_collaborationScopeKeysByRequest.end()) {
                auto keyIt = requestIt->second.find(keyScope);
                if (keyIt != requestIt->second.end()) {
                    scopeKey = keyIt->second;
                }
            }
        }
        if (scopeKey.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
            NDN_LOG_ERROR("Missing collaboration scope key for named large Data "
                          << requestId.toUri() << " scope=" << keyScope);
            return {};
        }

        HybridMessageEnvelope envelope;
        const std::string keyId = "collab-large|" + requestId.toUri() + "|" + keyScope;
        envelope.setKeyId(keyId);
        envelope.setEpochId("session");
        envelope.setMessageType("COLLAB-LARGE");
        const std::string adText = dataName.toUri() + "|COLLAB-LARGE|" +
                                   requestId.toUri() + "|" + keyScope;
        const ndn::Buffer ad(reinterpret_cast<const uint8_t*>(adText.data()), adText.size());
        auto encrypted = hybridAesGcmEncrypt(
            scopeKey,
            ndn::span<const uint8_t>(payload.data(), payload.size()),
            ndn::span<const uint8_t>(ad.data(), ad.size()));
        envelope.setNonce(encrypted.nonce);
        envelope.setCipherText(encrypted.ciphertext);
        envelope.setAuthTag(encrypted.tag);
        auto block = envelope.WireEncode();
        ndn::Buffer encoded(block.begin(), block.end());

        auto& activeKeyChain = m_testSigningKeyChain ?
            *m_testSigningKeyChain : m_keyChain;
        ndn::Segmenter segmenter(activeKeyChain, m_signingInfo);
        auto segments = segmenter.segment(
            ndn::span<const uint8_t>(encoded.data(), encoded.size()),
            dataName,
            maxSegmentSize == 0 ? 7000 : maxSegmentSize,
            ndn::time::milliseconds(freshnessMs <= 0 ? 60000 : freshnessMs));

        const bool activePut =
            boolEnvOrDefault("NDNSF_COLLAB_LARGE_ACTIVE_PUT", true);
        const bool fetchTimingEnabled = isTruthyEnv("NDNSF_COLLAB_LARGE_FETCH_TIMING");
        for (const auto& data : segments) {
            insertDataIntoIMS(*data, ndn::time::milliseconds(freshnessMs <= 0 ? 60000 : freshnessMs));
            if (activePut) {
                m_face.put(*data);
                if (fetchTimingEnabled) {
                    NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING"
                                 << " event=segment_active_put"
                                 << " mode=producer-active-put"
                                 << " timestamp_us=" << nowMicroseconds()
                                 << " requestId=" << requestId.toUri()
                                 << " keyScope=" << keyScope
                                 << " dataName=" << dataName.toUri()
                                 << " segmentName=" << data->getName().toUri()
                                 << " wire_bytes=" << data->wireEncode().size());
                }
            }
        }
        NDN_LOG_DEBUG("COLLAB_LARGE_NAMED_PUBLISHED name=" << dataName.toUri()
                      << " plaintextBytes=" << payload.size()
                      << " segments=" << segments.size()
                      << " activePut=" << activePut);
        return dataName;
    }

    bool
    ServiceProvider::publishCollaborationDataV1Segments(
        const ndn::Name& requestId,
        const std::string& keyScope,
        const std::vector<std::pair<ndn::Name, ndn::Buffer>>& segments,
        int freshnessMs)
    {
        if (m_svsps == nullptr || segments.empty()) {
            NDN_LOG_ERROR("NDNSF_DATA_V1 SVS publication unavailable or empty"
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope);
            return false;
        }
        const auto freshness = freshnessMs <= 0 ? 60000 : freshnessMs;
        for (const auto& publication : segments) {
            if (publication.first.empty() || publication.second.empty()) {
                NDN_LOG_ERROR("NDNSF_DATA_V1 SVS publication contains an empty"
                              << " name or segment requestId=" << requestId.toUri());
                return false;
            }
        }

        // Collaboration handlers execute on worker threads, while Face and
        // SVSPubSub state is owned by the Face io_context.  Calling publish()
        // directly from a handler races the mapping/sync state and previously
        // made this compatibility path depend on logging-induced timing.
        // dispatch() executes inline when already on the Face event loop and
        // queues otherwise; the bounded future keeps the synchronous API.
        auto completion = std::make_shared<std::promise<bool>>();
        auto future = completion->get_future();
        auto publications = segments;
        m_face.getIoContext().dispatch(
            [this, completion, publications = std::move(publications),
             requestId, keyScope, freshness] {
                try {
                    for (const auto& publication : publications) {
                        if (publishSvsBytes(m_svsps, publication.first,
                                            publication.second, freshness) == 0) {
                            NDN_LOG_ERROR("NDNSF_DATA_V1 SVS publication failed"
                                          << " requestId=" << requestId.toUri()
                                          << " dataName=" << publication.first.toUri());
                            completion->set_value(false);
                            return;
                        }
                        NDN_LOG_DEBUG("NDNSF_DATA_V1_SVS_SEGMENT_PUBLISHED"
                                      << " requestId=" << requestId.toUri()
                                      << " dataName=" << publication.first.toUri()
                                      << " bytes=" << publication.second.size());
                    }
                    NDN_LOG_DEBUG("NDNSF_DATA_V1_SVS_PUBLISHED"
                                  << " requestId=" << requestId.toUri()
                                  << " keyScope=" << keyScope
                                  << " segments=" << publications.size());
                    completion->set_value(true);
                }
                catch (const std::exception& error) {
                    NDN_LOG_ERROR("NDNSF_DATA_V1 SVS publication threw"
                                  << " requestId=" << requestId.toUri()
                                  << " reason=" << error.what());
                    completion->set_value(false);
                }
                catch (...) {
                    NDN_LOG_ERROR("NDNSF_DATA_V1 SVS publication threw"
                                  << " requestId=" << requestId.toUri()
                                  << " reason=unknown");
                    completion->set_value(false);
                }
            });
        if (future.wait_for(std::chrono::seconds(5)) !=
            std::future_status::ready) {
            NDN_LOG_ERROR("NDNSF_DATA_V1 SVS publication event-loop timeout"
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope);
            return false;
        }
        return future.get();
    }

    std::optional<std::vector<ndn::Buffer>>
    ServiceProvider::fetchCollaborationDataV1Segments(
        const ndn::Name& requestId,
        const std::string& keyScope,
        const ndn::Name& producerPrefix,
        std::uint64_t operationIndex,
        const std::string& producerRank,
        const std::string& tensorDigest,
        std::size_t expectedSegments,
        std::size_t maxSegments,
        int timeoutMs,
        std::function<std::size_t(const ndn::Buffer&)> segmentCountDecoder,
        DataV1SegmentNameFilter nameFilter)
    {
        const bool manifestProbe = expectedSegments == 0;
        if (m_svsps == nullptr || producerPrefix.empty() || maxSegments == 0 ||
            (manifestProbe && !segmentCountDecoder) ||
            expectedSegments > maxSegments) {
            NDN_LOG_ERROR("NDNSF_DATA_V1 SVS fetch arguments are invalid"
                          << " requestId=" << requestId.toUri()
                          << " producer=" << producerPrefix.toUri()
                          << " expectedSegments=" << expectedSegments
                          << " maxSegments=" << maxSegments);
            return std::nullopt;
        }

        struct FetchState
        {
            std::vector<ndn::Buffer> wires;
            std::vector<bool> received;
            std::size_t targetSegments = 0;
            std::size_t remaining = 0;
            bool targetKnown = false;
            uint32_t subscriptionHandle = 0;
            std::atomic<bool> failed{false};
        };

        const int fetchTimeoutMs = timeoutMs <= 0 ? 5000 : timeoutMs;
        const auto configuredCatchUpPublications = static_cast<std::size_t>(
            std::clamp(intEnvOrDefault(
                           "NDNSF_DATA_V1_SVS_CATCH_UP_PUBLICATIONS", 64),
                       1, 4096));
        const auto catchUpPublications = std::max(
            configuredCatchUpPublications,
            std::min<std::size_t>(expectedSegments, 4096));
        const int catchUpAgeMs = std::clamp(
            intEnvOrDefault("NDNSF_DATA_V1_SVS_CATCH_UP_AGE_MS", 5000),
            1, std::min(fetchTimeoutMs, 30000));
        auto state = std::make_shared<FetchState>();
        state->targetSegments = manifestProbe ? 0 : expectedSegments;
        state->remaining = state->targetSegments;
        state->targetKnown = !manifestProbe;
        state->wires.resize(manifestProbe ? maxSegments : expectedSegments);
        state->received.resize(state->wires.size(), false);
        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto mutex = std::make_shared<std::mutex>();
        auto cv = std::make_shared<std::condition_variable>();
        auto result = std::make_shared<std::vector<ndn::Buffer>>();

        auto finish = [this, state, completed, mutex, cv, result] {
            if (state->failed || !state->targetKnown || state->remaining != 0 ||
                completed->load()) {
                return;
            }
            if (state->subscriptionHandle != 0) {
                m_svsps->unsubscribe(state->subscriptionHandle);
                state->subscriptionHandle = 0;
            }
            {
                std::lock_guard<std::mutex> lock(*mutex);
                result->assign(
                    state->wires.begin(),
                    state->wires.begin() +
                      static_cast<std::ptrdiff_t>(state->targetSegments));
                completed->store(true);
            }
            cv->notify_one();
        };

        boost::asio::post(m_face.getIoContext(),
            [this, state, completed, cv, finish, requestId, keyScope,
             producerPrefix, operationIndex, producerRank, tensorDigest,
             maxSegments, manifestProbe, catchUpPublications, catchUpAgeMs,
             segmentCountDecoder = std::move(segmentCountDecoder),
             nameFilter = std::move(nameFilter)] {
                // Data-V1 publication is request-scoped and may be published
                // just before the dependent Provider installs its
                // subscription.  The Experimental NDN-SVS API provides a
                // bounded catch-up operation for this race; prefetch alone
                // only fetches future state-vector updates and cannot recover
                // an already-observed publication.
                state->subscriptionHandle = m_svsps->subscribeToProducerWithCatchUp(
                    producerPrefix,
                    [state, completed, finish, requestId, keyScope,
                     producerPrefix, operationIndex, producerRank, tensorDigest,
                     maxSegments, manifestProbe,
                     segmentCountDecoder, nameFilter]
                    (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
                        if (state->failed || completed->load() || publication.data.empty()) {
                            return;
                        }
                        try {
                            const ndn::Name publicationName(publication.name);
                            if (nameFilter.predicate &&
                                !nameFilter.predicate(publicationName)) {
                                return;
                            }
                            const std::vector<std::uint8_t> wire(
                                publication.data.begin(), publication.data.end());
                            const auto segmentNumber = parseDataV1SegmentNumber(
                                publicationName,
                                producerPrefix,
                                requestId,
                                operationIndex,
                                producerRank,
                                tensorDigest,
                                maxSegments);
                            if (!segmentNumber) {
                                return;
                            }
                            const auto index = *segmentNumber;
                            if (index >= state->wires.size()) {
                                state->failed = true;
                                return;
                            }
                            if (state->targetKnown && index >= state->targetSegments) {
                                state->failed = true;
                                return;
                            }
                            if (state->received[index]) {
                                if (state->wires[index] != wire) {
                                    state->failed = true;
                                }
                                return;
                            }
                            state->wires[index] = ndn::Buffer(wire.begin(), wire.end());
                            state->received[index] = true;
                            if (manifestProbe && index == 0 && !state->targetKnown) {
                                const auto discovered = segmentCountDecoder(
                                    state->wires[index]);
                                if (discovered == 0 || discovered > maxSegments) {
                                    state->failed = true;
                                    return;
                                }
                                state->targetSegments = discovered;
                                state->targetKnown = true;
                                state->remaining = discovered;
                                for (std::size_t segment = 0;
                                     segment < discovered; ++segment) {
                                    if (state->received[segment]) {
                                        --state->remaining;
                                    }
                                }
                                for (std::size_t segment = discovered;
                                     segment < state->received.size(); ++segment) {
                                    if (state->received[segment]) {
                                        state->failed = true;
                                        return;
                                    }
                                }
                            }
                            else if (state->targetKnown && index < state->targetSegments &&
                                     state->remaining > 0) {
                                --state->remaining;
                            }
                            finish();
                        }
                        catch (const std::exception&) {
                            // The producer subscription is shared by all
                            // collaboration traffic.  Non-V1 or unrelated
                            // publications are ignored; matching packets are
                            // authenticated by ProviderGroupCoordinator after
                            // this transport stage completes.
                        }
                    },
                    catchUpPublications,
                    ndn::time::milliseconds(catchUpAgeMs),
                    true,
                    false);
                if (nameFilter.subscriptionReady) {
                    try {
                        nameFilter.subscriptionReady();
                    }
                    catch (const std::exception& error) {
                        NDN_LOG_WARN("NDNSF_DATA_V1 subscription-ready observer failed"
                                     << " requestId=" << requestId.toUri()
                                     << " reason=" << error.what());
                    }
                    catch (...) {
                        NDN_LOG_WARN("NDNSF_DATA_V1 subscription-ready observer failed"
                                     << " requestId=" << requestId.toUri()
                                     << " reason=unknown");
                    }
                }
                NDN_LOG_DEBUG("NDNSF_DATA_V1_SVS_FETCH_SUBSCRIBED"
                              << " requestId=" << requestId.toUri()
                              << " keyScope=" << keyScope
                              << " producer=" << producerPrefix.toUri()
                              << " operation=" << operationIndex
                              << " maxSegments=" << maxSegments
                              << " catchUpPublications=" << catchUpPublications
                              << " catchUpAgeMs=" << catchUpAgeMs);
            });

        std::unique_lock<std::mutex> lock(*mutex);
        if (!cv->wait_for(lock, std::chrono::milliseconds(fetchTimeoutMs),
                          [completed] { return completed->load(); })) {
            boost::asio::post(m_face.getIoContext(), [this, state, completed] {
                state->failed = true;
                if (state->subscriptionHandle != 0) {
                    m_svsps->unsubscribe(state->subscriptionHandle);
                    state->subscriptionHandle = 0;
                }
                completed->store(true);
            });
            NDN_LOG_ERROR("NDNSF_DATA_V1 SVS fetch timed out"
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope
                          << " producer=" << producerPrefix.toUri());
            return std::nullopt;
        }
        if (state->failed || result->empty()) {
            NDN_LOG_ERROR("NDNSF_DATA_V1 SVS fetch failed"
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope
                          << " producer=" << producerPrefix.toUri());
            return std::nullopt;
        }
        return *result;
    }

    bool
    ServiceProvider::publishCollaborationSignedExactData(
        const ndn::Name& requestId,
        const std::string& keyScope,
        const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
        int freshnessMs)
    {
        if (objects.empty()) {
            NDN_LOG_ERROR("Exact collaboration publication is empty"
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope);
            return false;
        }
        const auto freshness = ndn::time::milliseconds(
            freshnessMs <= 0 ? 60000 : freshnessMs);
        std::set<std::string> names;
        for (const auto& object : objects) {
            if (object.first.empty() || object.second.empty() ||
                !names.insert(object.first.toUri()).second) {
                NDN_LOG_ERROR("Exact collaboration publication contains an"
                              " empty or duplicated object"
                              << " requestId=" << requestId.toUri()
                              << " keyScope=" << keyScope
                              << " dataName=" << object.first.toUri());
                return false;
            }
        }
        struct PreparedData
        {
            std::shared_ptr<ndn::Data> data;
            std::size_t wireSize = 0;
        };
        std::vector<PreparedData> prepared;
        prepared.reserve(objects.size());
        for (const auto& object : objects) {
            // ndn-cxx IMS retains Data through enable_shared_from_this; a
            // stack-allocated Data triggers std::bad_weak_ptr in insert().
            auto data = std::make_shared<ndn::Data>(object.first);
            data->setFreshnessPeriod(freshness);
            data->setContent(object.second);
            (m_testSigningKeyChain ? *m_testSigningKeyChain : m_keyChain)
                .sign(*data, m_signingInfo);
            const auto wireSize = data->wireEncode().size();
            if (wireSize > ndn::MAX_NDN_PACKET_SIZE) {
                NDN_LOG_ERROR("Exact collaboration Data exceeds NDN wire limit"
                              << " requestId=" << requestId.toUri()
                              << " keyScope=" << keyScope
                              << " dataName=" << object.first.toUri()
                              << " contentBytes=" << object.second.size()
                              << " wireBytes=" << wireSize
                              << " limit=" << ndn::MAX_NDN_PACKET_SIZE);
                return false;
            }
            prepared.push_back({std::move(data), wireSize});
        }
        for (const auto& item : prepared) {
            insertDataIntoIMS(*item.data, freshness);
            NDN_LOG_DEBUG("NDNSF_DI_EXACT_DATA_PUBLISHED"
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope
                          << " dataName=" << item.data->getName().toUri()
                          << " bytes=" << item.data->getContent().value_size()
                          << " wireBytes=" << item.wireSize);
        }
        return true;
    }

    std::optional<ndn::Buffer>
    ServiceProvider::fetchCollaborationSignedExactData(
        const ndn::Name& requestId,
        const std::string& keyScope,
        const ndn::Name& dataName,
        const ndn::Name& expectedProducer,
        int timeoutMs,
        std::function<bool()> shouldCancel)
    {
        if (dataName.empty() || expectedProducer.empty() || timeoutMs <= 0) {
            NDN_LOG_ERROR("Exact collaboration fetch arguments are invalid"
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope
                          << " dataName=" << dataName.toUri()
                          << " producer=" << expectedProducer.toUri());
            return std::nullopt;
        }

        struct ExactFetchState
        {
            std::atomic<bool> completed{false};
            std::mutex mutex;
            std::condition_variable cv;
            ndn::Buffer content;
            std::string error;
            std::size_t attempts = 0;
            std::chrono::steady_clock::time_point deadline;
        };
        auto state = std::make_shared<ExactFetchState>();
        state->deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
        const int interestLifetimeMs = std::max(
            50, std::min(timeoutMs,
                         intEnvOrDefault(
                           "NDNSF_DI_EXACT_INTEREST_LIFETIME_MS", 500)));

        auto finish = [state](ndn::Buffer content, std::string error) {
            if (state->completed.exchange(true)) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->content = std::move(content);
                state->error = std::move(error);
            }
            state->cv.notify_one();
        };
        auto express = std::make_shared<std::function<void()>>();
        auto retry = std::make_shared<std::function<void(const char*)>>();
        std::weak_ptr<std::function<void()>> weakExpress = express;
        *retry = [this, state, finish, weakExpress, dataName, requestId, keyScope,
                  shouldCancel](const char* reason) {
            if (state->completed.load()) {
                return;
            }
            if (shouldCancel && shouldCancel()) {
                finish({}, "cancelled while fetching " + dataName.toUri());
                return;
            }
            if (std::chrono::steady_clock::now() >= state->deadline) {
                finish({}, std::string(reason) + " for " + dataName.toUri());
                return;
            }
            const auto retryUs = std::chrono::duration_cast<
                std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=COLLAB_DATA_RETRY"
                          << " timestamp_us=" << retryUs
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope
                          << " dataName=" << dataName.toUri()
                          << " reason=" << reason
                          << " nextAttempt=" << (state->attempts + 1));
            if (const auto next = weakExpress.lock()) {
                m_scheduler.schedule(ndn::time::milliseconds(5),
                                     [next] { (*next)(); });
            }
        };
        *express = [this, state, finish, retry, dataName, expectedProducer,
                    requestId, keyScope, interestLifetimeMs, shouldCancel] {
            if (state->completed.load()) {
                return;
            }
            if (shouldCancel && shouldCancel()) {
                finish({}, "cancelled while fetching " + dataName.toUri());
                return;
            }
            if (std::chrono::steady_clock::now() >= state->deadline) {
                finish({}, "hard deadline for " + dataName.toUri());
                return;
            }
            ++state->attempts;
            ndn::Interest interest(dataName);
            interest.setCanBePrefix(false);
            interest.setMustBeFresh(true);
            interest.setInterestLifetime(
                ndn::time::milliseconds(interestLifetimeMs));
            const auto issuedUs = std::chrono::duration_cast<
                std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=COLLAB_DATA_INTEREST_ISSUED"
                          << " timestamp_us=" << issuedUs
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope
                          << " dataName=" << dataName.toUri()
                          << " attempt=" << state->attempts
                          << " lifetimeMs=" << interestLifetimeMs);
            m_face.expressInterest(
                interest,
                [this, state, finish, dataName, expectedProducer, requestId,
                 keyScope]
                (const ndn::Interest&, const ndn::Data& data) {
                    if (state->completed.load()) {
                        return;
                    }
                    const auto receivedUs = std::chrono::duration_cast<
                        std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=COLLAB_DATA_RECEIVED"
                                  << " timestamp_us=" << receivedUs
                                  << " requestId=" << requestId.toUri()
                                  << " keyScope=" << keyScope
                                  << " requestedName=" << dataName.toUri()
                                  << " returnedName=" << data.getName().toUri()
                                  << " attempt=" << state->attempts);
                    if (data.getName() != dataName) {
                        finish({}, "exact Data name mismatch for " +
                                   dataName.toUri());
                        return;
                    }
                    validator->validate(
                        data,
                        [finish, dataName, expectedProducer, requestId, keyScope]
                        (const ndn::Data& validated) {
                            if (validated.getName() != dataName ||
                                !isSignedByIdentity(validated,
                                                    expectedProducer)) {
                                finish({}, "exact Data signer mismatch for " +
                                           dataName.toUri());
                                return;
                            }
                            const auto& content = validated.getContent();
                            const auto verifiedUs = std::chrono::duration_cast<
                                std::chrono::microseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=COLLAB_DATA_VERIFIED"
                                          << " timestamp_us=" << verifiedUs
                                          << " requestId=" << requestId.toUri()
                                          << " keyScope=" << keyScope
                                          << " dataName=" << dataName.toUri()
                                          << " producer=" << expectedProducer.toUri()
                                          << " bytes=" << content.value_size());
                            finish(ndn::Buffer(content.value_begin(),
                                               content.value_end()), {});
                        },
                        [finish, dataName]
                        (const ndn::Data&,
                         const ndn::security::ValidationError& error) {
                            finish({}, "exact Data signature validation failed for " +
                                       dataName.toUri() + ": " + error.getInfo());
                        });
                },
                [retry](const ndn::Interest&, const ndn::lp::Nack&) {
                    (*retry)("Nack");
                },
                [retry](const ndn::Interest&) {
                    (*retry)("timeout");
                });
        };

        auto cancelPoll = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weakCancelPoll = cancelPoll;
        *cancelPoll = [this, state, finish, shouldCancel, dataName,
                       weakCancelPoll] {
            if (state->completed.load() || !shouldCancel) {
                return;
            }
            if (shouldCancel()) {
                finish({}, "cancelled while fetching " + dataName.toUri());
                return;
            }
            m_scheduler.schedule(ndn::time::milliseconds(10),
                                 [weakCancelPoll] {
                if (const auto poll = weakCancelPoll.lock()) {
                    (*poll)();
                }
            });
        };
        boost::asio::post(m_face.getIoContext(), [express, cancelPoll] {
            (*express)();
            (*cancelPoll)();
        });
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs + 50),
                           [state] { return state->completed.load(); });
        if (!state->completed.load() || !state->error.empty() ||
            state->content.empty()) {
            NDN_LOG_ERROR("Exact collaboration fetch failed"
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope
                          << " dataName=" << dataName.toUri()
                          << " attempts=" << state->attempts
                          << " error=" << (state->error.empty() ?
                                             "deadline" : state->error));
            return std::nullopt;
        }
        return state->content;
    }

    std::optional<ndn::Buffer>
    ServiceProvider::fetchCollaborationLargeData(
        const ndn::Name& requestId,
        const std::string& keyScope,
        const ndn::Name& dataName,
        int timeoutMs,
        std::size_t expectedSegments)
    {
        // A /SERVICE scope is a request-input authorization scope, not a
        // collaboration tensor scope.  Such objects carry their own
        // NAC-wrapped hybrid key inside the envelope, so the service-
        // authorized large-data path fetches and decrypts them; no
        // collaboration scope key is ever distributed for them.
        static const std::string serviceMarker = "/SERVICE";
        if (keyScope.rfind(serviceMarker, 0) == 0) {
            std::string serviceName = keyScope.substr(serviceMarker.size());
            if (!serviceName.empty() && serviceName.front() == '/') {
                auto result = fetchAndDecryptLargeData(dataName, serviceName);
                if (!result.success) {
                    NDN_LOG_ERROR("Service-authorized large Data fetch failed"
                                  << " requestId=" << requestId.toUri()
                                  << " scope=" << keyScope
                                  << " dataName=" << dataName.toUri()
                                  << " error=" << result.errorMessage);
                    return std::nullopt;
                }
                return std::optional<ndn::Buffer>(
                    ndn::Buffer(result.plaintext.begin(), result.plaintext.end()));
            }
        }
        ndn::Buffer scopeKey;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            auto requestIt = m_collaborationScopeKeysByRequest.find(requestId);
            if (requestIt != m_collaborationScopeKeysByRequest.end()) {
                auto keyIt = requestIt->second.find(keyScope);
                if (keyIt != requestIt->second.end()) {
                    scopeKey = keyIt->second;
                }
            }
        }
        if (scopeKey.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
            NDN_LOG_ERROR("Missing collaboration scope key to fetch large Data "
                          << requestId.toUri() << " scope=" << keyScope);
            return std::nullopt;
        }

        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto mutex = std::make_shared<std::mutex>();
        auto cv = std::make_shared<std::condition_variable>();
        auto error = std::make_shared<std::string>();
        auto encoded = std::make_shared<ndn::Buffer>();

        const int fetchTimeoutMs = timeoutMs <= 0 ? 5000 : timeoutMs;
        // Planned dataflow users may issue Interests before the upstream
        // provider has published the corresponding activation segments. Keep
        // the default lifetime long enough for normal distributed inference
        // runs while still allowing experiments to override it explicitly.
        const int interestLifetimeMs =
            std::max(50, intEnvOrDefault("NDNSF_COLLAB_LARGE_INTEREST_LIFETIME_MS", 30000));
        const int exactSegmentInterestLifetimeMs =
            std::max(50, std::min(interestLifetimeMs,
                                  intEnvOrDefault(
                                      "NDNSF_COLLAB_LARGE_EXACT_SEGMENT_INTEREST_LIFETIME_MS",
                                      5000)));
        const double fetchInitCwnd = static_cast<double>(
            std::max(1, intEnvOrDefault("NDNSF_COLLAB_LARGE_FETCH_INIT_CWND", 8)));
        const size_t exactSegmentWindow = static_cast<size_t>(
            std::max(1, intEnvOrDefault("NDNSF_COLLAB_LARGE_EXACT_SEGMENT_WINDOW", 64)));
        const bool fetchTimingEnabled = isTruthyEnv("NDNSF_COLLAB_LARGE_FETCH_TIMING");
        const bool telemetryExportEnabled =
            isTruthyEnv("NDNSF_NETWORK_TELEMETRY_EXPORT");
        const bool exactSegmentFetch =
            expectedSegments > 0 &&
            boolEnvOrDefault("NDNSF_COLLAB_LARGE_EXACT_SEGMENT_FETCH", true);
        const auto fetchStart = std::chrono::steady_clock::now();
        auto fetchStats = std::make_shared<CollaborationLargeFetchTiming>();
        fetchStats->start = fetchStart;
        const auto dataValidator = validator;
        if (!dataValidator) {
            NDN_LOG_ERROR("Missing Data validator for collaboration large-data fetch"
                          << " requestId=" << requestId.toUri()
                          << " keyScope=" << keyScope
                          << " dataName=" << dataName.toUri());
            return std::nullopt;
        }

        boost::asio::post(m_face.getIoContext(), [this, dataValidator, dataName, completed, mutex, cv, error,
                                                  encoded, requestId, keyScope, fetchTimeoutMs,
                                                  interestLifetimeMs, fetchTimingEnabled,
                                                  exactSegmentInterestLifetimeMs,
                                                  fetchInitCwnd, exactSegmentWindow, fetchStart,
                                                  fetchStats, exactSegmentFetch,
                                                  expectedSegments] {
            if (exactSegmentFetch) {
                struct ExactFetchState {
                    std::vector<ndn::Buffer> contents;
                    std::vector<bool> received;
                    std::vector<bool> inFlight;
                    std::vector<size_t> attempts;
                    std::vector<std::chrono::steady_clock::time_point> interestIssued;
                    std::vector<std::chrono::steady_clock::time_point> dataReceived;
                    size_t remaining = 0;
                    size_t targetSegments = 0;
                    size_t inFlightCount = 0;
                    size_t nextSegmentToIssue = 0;
                    bool failed = false;
                };
                auto state = std::make_shared<ExactFetchState>();
                state->contents.resize(expectedSegments);
                state->received.resize(expectedSegments, false);
                state->inFlight.resize(expectedSegments, false);
                state->attempts.resize(expectedSegments, 0);
                state->interestIssued.resize(expectedSegments);
                state->dataReceived.resize(expectedSegments);
                state->remaining = expectedSegments;
                state->targetSegments = expectedSegments;
                const auto fetchDeadline = fetchStart + std::chrono::milliseconds(fetchTimeoutMs);
                if (fetchTimingEnabled) {
                    NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING event=start"
                                 << " mode=exact-segments"
                                 << " timestamp_us=" << nowMicroseconds()
                                 << " start_epoch_ms="
                                 << epochMs(std::chrono::system_clock::now())
                                 << " requestId=" << requestId.toUri()
                                 << " keyScope=" << keyScope
                                 << " dataName=" << dataName.toUri()
                                 << " expected_segments=" << expectedSegments
                                 << " timeout_ms=" << fetchTimeoutMs
                                 << " interest_lifetime_ms=" << exactSegmentInterestLifetimeMs
                                 << " exact_window=" << exactSegmentWindow
                                 << " init_cwnd=" << fetchInitCwnd);
                }
                auto finishIfComplete = [completed, mutex, cv, encoded, requestId, keyScope,
                                         dataName, fetchTimeoutMs,
                                         exactSegmentInterestLifetimeMs, fetchTimingEnabled,
                                         fetchInitCwnd, exactSegmentWindow, fetchStart,
                                         fetchStats, state] {
                    if (state->failed || state->remaining != 0) {
                        return;
                    }
                    ndn::Buffer assembled;
                    size_t totalSize = 0;
                    for (size_t i = 0; i < state->targetSegments; ++i) {
                        const auto& content = state->contents[i];
                        totalSize += content.size();
                    }
                    assembled.reserve(totalSize);
                    for (size_t i = 0; i < state->targetSegments; ++i) {
                        const auto& content = state->contents[i];
                        assembled.insert(assembled.end(), content.begin(), content.end());
                    }
                    const auto completeTime = std::chrono::steady_clock::now();
                    fetchStats->completeWall = std::chrono::system_clock::now();
                    const auto elapsedMs = elapsedMsSince(fetchStart, completeTime);
                    {
                        std::lock_guard<std::mutex> lock(*mutex);
                        *encoded = std::move(assembled);
                        completed->store(true);
                    }
                    if (fetchTimingEnabled) {
                        const double firstSegmentMs = fetchStats->receivedSegments == 0 ? 0.0 :
                            elapsedMsSince(fetchStart, fetchStats->firstSegmentReceived);
                        const double lastReceivedMs = fetchStats->receivedSegments == 0 ? 0.0 :
                            elapsedMsSince(fetchStart, fetchStats->lastSegmentReceived);
                        const double lastValidatedMs = fetchStats->validatedSegments == 0 ? 0.0 :
                            elapsedMsSince(fetchStart, fetchStats->lastSegmentValidated);
                        NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING event=complete"
                                     << " mode=exact-segments"
                                     << " requestId=" << requestId.toUri()
                                     << " keyScope=" << keyScope
                                     << " dataName=" << dataName.toUri()
                                     << " encoded_bytes=" << encoded->size()
                                     << " elapsed_ms=" << elapsedMs
                                     << " first_segment_ms=" << firstSegmentMs
                                     << " last_segment_received_ms=" << lastReceivedMs
                                     << " last_segment_validated_ms=" << lastValidatedMs
                                     << " first_segment_epoch_ms="
                                     << epochMs(fetchStats->firstSegmentWall)
                                     << " complete_epoch_ms="
                                     << epochMs(fetchStats->completeWall)
                                     << " received_segments=" << fetchStats->receivedSegments
                                     << " validated_segments=" << fetchStats->validatedSegments
                                     << " received_wire_bytes=" << fetchStats->receivedWireBytes
                                     << " nacks=" << fetchStats->nacks
                                     << " segment_timeouts=" << fetchStats->timeouts
                                     << " timeout_ms=" << fetchTimeoutMs
                                     << " interest_lifetime_ms="
                                     << exactSegmentInterestLifetimeMs
                                     << " exact_window=" << exactSegmentWindow
                                     << " init_cwnd=" << fetchInitCwnd);
                    }
                    cv->notify_one();
                };
                auto failOnce = [completed, mutex, cv, error, requestId, keyScope, dataName,
                                 fetchTimeoutMs, exactSegmentInterestLifetimeMs,
                                 fetchTimingEnabled, fetchInitCwnd, exactSegmentWindow,
                                 fetchStart, fetchStats, state]
                                (const std::string& message) {
                    if (state->failed || completed->load()) {
                        return;
                    }
                    state->failed = true;
                    fetchStats->completeWall = std::chrono::system_clock::now();
                    const auto elapsedMs = elapsedMsSince(
                        fetchStart, std::chrono::steady_clock::now());
                    {
                        std::lock_guard<std::mutex> lock(*mutex);
                        *error = message;
                        completed->store(true);
                    }
                    if (fetchTimingEnabled) {
                        NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING event=error"
                                     << " mode=exact-segments"
                                     << " requestId=" << requestId.toUri()
                                     << " keyScope=" << keyScope
                                     << " dataName=" << dataName.toUri()
                                     << " error_code=exact"
                                     << " error_message=\"" << message << "\""
                                     << " elapsed_ms=" << elapsedMs
                                     << " first_segment_epoch_ms="
                                     << epochMs(fetchStats->firstSegmentWall)
                                     << " complete_epoch_ms="
                                     << epochMs(fetchStats->completeWall)
                                     << " received_segments=" << fetchStats->receivedSegments
                                     << " validated_segments=" << fetchStats->validatedSegments
                                     << " received_wire_bytes=" << fetchStats->receivedWireBytes
                                     << " nacks=" << fetchStats->nacks
                                     << " segment_timeouts=" << fetchStats->timeouts
                                     << " timeout_ms=" << fetchTimeoutMs
                                     << " interest_lifetime_ms="
                                     << exactSegmentInterestLifetimeMs
                                     << " exact_window=" << exactSegmentWindow
                                     << " init_cwnd=" << fetchInitCwnd);
                    }
                    cv->notify_one();
                };

                auto expressSegment = std::make_shared<std::function<void(size_t)>>();
                auto issueMoreSegments = std::make_shared<std::function<void()>>();
                *expressSegment = [this, dataValidator, state, fetchStats, finishIfComplete, failOnce,
                                   expressSegment, issueMoreSegments, completed,
                                   fetchTimingEnabled, fetchStart, fetchDeadline,
                                   exactSegmentInterestLifetimeMs, requestId, keyScope,
                                   dataName](size_t i) {
                    if (state->failed || completed->load() || i >= state->targetSegments ||
                        i >= state->received.size() || state->received[i] ||
                        state->inFlight[i]) {
                        return;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= fetchDeadline) {
                        failOnce("timeout for " + ndn::Name(dataName).appendSegment(i).toUri());
                        return;
                    }
                    ndn::Name segmentName(dataName);
                    segmentName.appendSegment(i);
                    ndn::Interest interest(segmentName);
                    interest.setCanBePrefix(false);
                    interest.setMustBeFresh(true);
                    interest.setInterestLifetime(
                        ndn::time::milliseconds(exactSegmentInterestLifetimeMs));
                    state->inFlight[i] = true;
                    ++state->inFlightCount;
                    const auto attempt = ++state->attempts[i];
                    if (fetchTimingEnabled) {
                        state->interestIssued[i] = std::chrono::steady_clock::now();
                        NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING event=segment_interest"
                                     << " mode=exact-segments"
                                     << " timestamp_us=" << nowMicroseconds()
                                     << " requestId=" << requestId.toUri()
                                     << " keyScope=" << keyScope
                                     << " dataName=" << dataName.toUri()
                                     << " segment=" << i
                                     << " attempt=" << attempt
                                     << " segmentName=" << segmentName.toUri()
                                     << " fetch_start_to_interest_ms="
                                     << elapsedMsSince(fetchStart, state->interestIssued[i])
                                     << " interest_lifetime_ms="
                                     << exactSegmentInterestLifetimeMs);
                    }
                    auto processValidated =
                        std::make_shared<std::function<void(const ndn::Data&)>>();
                    *processValidated = [state, fetchStats, finishIfComplete, failOnce, i,
                                         issueMoreSegments, fetchTimingEnabled, fetchStart,
                                         requestId, keyScope, dataName]
                        (const ndn::Data& data) {
                            if (state->failed || i >= state->received.size() ||
                                i >= state->targetSegments || state->received[i]) {
                                (*issueMoreSegments)();
                                return;
                            }
                            const auto& finalBlock = data.getFinalBlock();
                            if (finalBlock && finalBlock->isSegment()) {
                                const auto finalSegment = finalBlock->toSegment();
                                const auto actualSegments =
                                    static_cast<size_t>(finalSegment + 1);
                                if (actualSegments > state->contents.size()) {
                                    failOnce("actual final segment exceeds planned exact "
                                             "fetch window for " + data.getName().toUri());
                                    return;
                                }
                                if (actualSegments < state->targetSegments) {
                                    size_t remaining = 0;
                                    for (size_t j = 0; j < actualSegments; ++j) {
                                        if (!state->received[j]) {
                                            ++remaining;
                                        }
                                    }
                                    state->targetSegments = actualSegments;
                                    state->remaining = remaining;
                                }
                            }
                            const auto acceptedAt = std::chrono::steady_clock::now();
                            fetchStats->lastSegmentValidated = acceptedAt;
                            ++fetchStats->validatedSegments;
                            if (fetchTimingEnabled) {
                                const auto issuedAt = i < state->interestIssued.size() ?
                                    state->interestIssued[i] :
                                    std::chrono::steady_clock::time_point{};
                                const auto receivedAt = i < state->dataReceived.size() ?
                                    state->dataReceived[i] :
                                    std::chrono::steady_clock::time_point{};
                                const double interestToValidatedMs =
                                    issuedAt == std::chrono::steady_clock::time_point{} ?
                                    0.0 : elapsedMsSince(issuedAt, acceptedAt);
                                const double dataToValidatedMs =
                                    receivedAt == std::chrono::steady_clock::time_point{} ?
                                    0.0 : elapsedMsSince(receivedAt, acceptedAt);
                                NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING"
                                             << " event=segment_validated"
                                             << " mode=exact-segments"
                                             << " timestamp_us=" << nowMicroseconds()
                                             << " requestId=" << requestId.toUri()
                                             << " keyScope=" << keyScope
                                             << " dataName=" << dataName.toUri()
                                             << " segment=" << i
                                             << " segmentName=" << data.getName().toUri()
                                             << " fetch_start_to_validated_ms="
                                             << elapsedMsSince(fetchStart, acceptedAt)
                                             << " interest_to_validated_ms="
                                             << interestToValidatedMs
                                             << " data_to_validated_ms="
                                             << dataToValidatedMs);
                            }
                            const auto content = data.getContent();
                            state->contents[i] = ndn::Buffer(content.value_begin(),
                                                             content.value_end());
                            state->received[i] = true;
                            if (i < state->targetSegments && state->remaining > 0) {
                                --state->remaining;
                            }
                            finishIfComplete();
                            (*issueMoreSegments)();
                        };
                    m_face.expressInterest(
                        interest,
                        [dataValidator, state, fetchStats, failOnce, processValidated, i,
                         issueMoreSegments, fetchTimingEnabled, fetchStart, requestId,
                         keyScope, dataName]
                        (const ndn::Interest&, const ndn::Data& data) {
                            const auto receivedAt = std::chrono::steady_clock::now();
                            if (i < state->inFlight.size() && state->inFlight[i]) {
                                state->inFlight[i] = false;
                                if (state->inFlightCount > 0) {
                                    --state->inFlightCount;
                                }
                            }
                            if (state->failed || i >= state->received.size() ||
                                i >= state->targetSegments || state->received[i]) {
                                (*issueMoreSegments)();
                                return;
                            }
                            ndn::Name expectedName(dataName);
                            expectedName.appendSegment(i);
                            if (data.getName() != expectedName) {
                                failOnce("exact Data segment name mismatch for " +
                                         expectedName.toUri());
                                return;
                            }
                            if (i < state->dataReceived.size()) {
                                state->dataReceived[i] = receivedAt;
                            }
                            if (fetchStats->receivedSegments == 0) {
                                fetchStats->firstSegmentReceived = receivedAt;
                                fetchStats->firstSegmentWall = std::chrono::system_clock::now();
                            }
                            fetchStats->lastSegmentReceived = receivedAt;
                            ++fetchStats->receivedSegments;
                            fetchStats->receivedWireBytes += data.wireEncode().size();
                            if (fetchTimingEnabled) {
                                const auto issuedAt = i < state->interestIssued.size() ?
                                    state->interestIssued[i] :
                                    std::chrono::steady_clock::time_point{};
                                const double interestToDataMs =
                                    issuedAt == std::chrono::steady_clock::time_point{} ? 0.0 :
                                    elapsedMsSince(issuedAt, receivedAt);
                                NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING"
                                             << " event=segment_received"
                                             << " mode=exact-segments"
                                             << " timestamp_us=" << nowMicroseconds()
                                             << " requestId=" << requestId.toUri()
                                             << " keyScope=" << keyScope
                                             << " dataName=" << dataName.toUri()
                                             << " segment=" << i
                                             << " segmentName=" << data.getName().toUri()
                                             << " fetch_start_to_data_ms="
                                             << elapsedMsSince(fetchStart, receivedAt)
                                             << " interest_to_data_ms=" << interestToDataMs
                                             << " wire_bytes=" << data.wireEncode().size());
                            }
                            dataValidator->validate(
                                data,
                                [processValidated](const ndn::Data& validated) {
                                    (*processValidated)(validated);
                                },
                                [failOnce](const ndn::Data&,
                                           const ndn::security::ValidationError& error) {
                                    failOnce("Data signature validation failed: " +
                                             error.getInfo());
                                });
                        },
                        [state, fetchStats, failOnce, expressSegment, issueMoreSegments,
                         fetchTimingEnabled, fetchStart, fetchDeadline, requestId, keyScope,
                         dataName, i]
                        (const ndn::Interest& interest, const ndn::lp::Nack&) {
                            ++fetchStats->nacks;
                            if (i < state->inFlight.size() && state->inFlight[i]) {
                                state->inFlight[i] = false;
                                if (state->inFlightCount > 0) {
                                    --state->inFlightCount;
                                }
                            }
                            if (state->failed || i >= state->received.size() ||
                                state->received[i]) {
                                (*issueMoreSegments)();
                                return;
                            }
                            if (std::chrono::steady_clock::now() >= fetchDeadline) {
                                failOnce("Nack for " + interest.getName().toUri());
                                return;
                            }
                            if (fetchTimingEnabled) {
                                NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING"
                                             << " event=segment_retry"
                                             << " mode=exact-segments"
                                             << " reason=nack"
                                             << " timestamp_us=" << nowMicroseconds()
                                             << " requestId=" << requestId.toUri()
                                             << " keyScope=" << keyScope
                                             << " dataName=" << dataName.toUri()
                                             << " segment=" << i
                                             << " attempts=" << state->attempts[i]
                                             << " fetch_start_to_retry_ms="
                                             << elapsedMsSince(fetchStart,
                                                              std::chrono::steady_clock::now()));
                            }
                            (*expressSegment)(i);
                            (*issueMoreSegments)();
                        },
                        [state, fetchStats, failOnce, expressSegment, issueMoreSegments,
                         fetchTimingEnabled, fetchStart, fetchDeadline, requestId, keyScope,
                         dataName, i]
                        (const ndn::Interest& interest) {
                            ++fetchStats->timeouts;
                            if (i < state->inFlight.size() && state->inFlight[i]) {
                                state->inFlight[i] = false;
                                if (state->inFlightCount > 0) {
                                    --state->inFlightCount;
                                }
                            }
                            if (state->failed || i >= state->received.size() ||
                                state->received[i]) {
                                (*issueMoreSegments)();
                                return;
                            }
                            if (std::chrono::steady_clock::now() >= fetchDeadline) {
                                failOnce("timeout for " + interest.getName().toUri());
                                return;
                            }
                            if (fetchTimingEnabled) {
                                NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING"
                                             << " event=segment_retry"
                                             << " mode=exact-segments"
                                             << " reason=timeout"
                                             << " timestamp_us=" << nowMicroseconds()
                                             << " requestId=" << requestId.toUri()
                                             << " keyScope=" << keyScope
                                             << " dataName=" << dataName.toUri()
                                             << " segment=" << i
                                             << " attempts=" << state->attempts[i]
                                             << " fetch_start_to_retry_ms="
                                             << elapsedMsSince(fetchStart,
                                                              std::chrono::steady_clock::now()));
                            }
                            (*expressSegment)(i);
                            (*issueMoreSegments)();
                        });
                };
                *issueMoreSegments = [state, expressSegment, exactSegmentWindow] {
                    while (!state->failed &&
                           state->inFlightCount < exactSegmentWindow &&
                           state->nextSegmentToIssue < state->targetSegments) {
                        const size_t segment = state->nextSegmentToIssue++;
                        if (segment < state->received.size() && !state->received[segment]) {
                            (*expressSegment)(segment);
                        }
                    }
                };
                (*issueMoreSegments)();
                return;
            }

            ndn::Interest interest(dataName);
            interest.setCanBePrefix(true);
            interest.setMustBeFresh(true);
            interest.setInterestLifetime(ndn::time::milliseconds(interestLifetimeMs));
            ndn::SegmentFetcher::Options options;
            options.probeLatestVersion = false;
            options.useConstantCwnd = true;
            options.initCwnd = fetchInitCwnd;
            options.maxTimeout = ndn::time::milliseconds(fetchTimeoutMs);
            options.interestLifetime = ndn::time::milliseconds(interestLifetimeMs);
            if (fetchTimingEnabled) {
                NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING event=start"
                             << " mode=segment-fetcher"
                             << " timestamp_us=" << nowMicroseconds()
                             << " start_epoch_ms="
                             << epochMs(std::chrono::system_clock::now())
                             << " requestId=" << requestId.toUri()
                             << " keyScope=" << keyScope
                             << " dataName=" << dataName.toUri()
                             << " timeout_ms=" << fetchTimeoutMs
                             << " interest_lifetime_ms=" << interestLifetimeMs
                             << " init_cwnd=" << fetchInitCwnd);
            }
            auto transportValidator = dataValidator;
            auto fetcher = ndn::SegmentFetcher::start(
                m_face, interest,
                transportValidator->getConfiguredValidatorForSegmentFetcher(), options);
            if (fetchTimingEnabled) {
                auto segmentReceivedAt = std::make_shared<
                    std::unordered_map<std::string, std::chrono::steady_clock::time_point>>();
                fetcher->afterSegmentReceived.connect(
                    [fetchStats, transportValidator, fetchStart, requestId, keyScope, dataName,
                     segmentReceivedAt](const ndn::Data& data) {
                        const auto now = std::chrono::steady_clock::now();
                        if (fetchStats->receivedSegments == 0) {
                            fetchStats->firstSegmentReceived = now;
                            fetchStats->firstSegmentWall = std::chrono::system_clock::now();
                        }
                        fetchStats->lastSegmentReceived = now;
                        ++fetchStats->receivedSegments;
                        fetchStats->receivedWireBytes += data.wireEncode().size();
                        const auto segmentName = data.getName().toUri();
                        (*segmentReceivedAt)[segmentName] = now;
                        uint64_t segmentNo = 0;
                        bool hasSegmentNo = false;
                        if (!data.getName().empty() && data.getName()[-1].isSegment()) {
                            segmentNo = data.getName()[-1].toSegment();
                            hasSegmentNo = true;
                        }
                        uint64_t finalSegment = 0;
                        bool hasFinalSegment = false;
                        const auto& finalBlock = data.getFinalBlock();
                        if (finalBlock && finalBlock->isSegment()) {
                            finalSegment = finalBlock->toSegment();
                            hasFinalSegment = true;
                        }
                        NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING"
                                     << " event=segment_received"
                                     << " mode=segment-fetcher"
                                     << " timestamp_us=" << nowMicroseconds()
                                     << " requestId=" << requestId.toUri()
                                     << " keyScope=" << keyScope
                                     << " dataName=" << dataName.toUri()
                                     << " segment=" << (hasSegmentNo ? segmentNo : 0)
                                     << " has_segment=" << (hasSegmentNo ? 1 : 0)
                                     << " final_segment="
                                     << (hasFinalSegment ? finalSegment : 0)
                                     << " has_final_segment="
                                     << (hasFinalSegment ? 1 : 0)
                                     << " segmentName=" << segmentName
                                     << " fetch_start_to_data_ms="
                                     << elapsedMsSince(fetchStart, now)
                                     << " interest_to_data_ms=0"
                                     << " wire_bytes=" << data.wireEncode().size());
                    });
                fetcher->afterSegmentValidated.connect(
                    [fetchStats, transportValidator, fetchStart, requestId, keyScope, dataName,
                     segmentReceivedAt](const ndn::Data& data) {
                        const auto now = std::chrono::steady_clock::now();
                        fetchStats->lastSegmentValidated = now;
                        ++fetchStats->validatedSegments;
                        const auto segmentName = data.getName().toUri();
                        const auto receivedIt = segmentReceivedAt->find(segmentName);
                        const double dataToValidatedMs =
                            receivedIt == segmentReceivedAt->end() ?
                            0.0 : elapsedMsSince(receivedIt->second, now);
                        uint64_t segmentNo = 0;
                        bool hasSegmentNo = false;
                        if (!data.getName().empty() && data.getName()[-1].isSegment()) {
                            segmentNo = data.getName()[-1].toSegment();
                            hasSegmentNo = true;
                        }
                        NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING"
                                     << " event=segment_validated"
                                     << " mode=segment-fetcher"
                                     << " timestamp_us=" << nowMicroseconds()
                                     << " requestId=" << requestId.toUri()
                                     << " keyScope=" << keyScope
                                     << " dataName=" << dataName.toUri()
                                     << " segment=" << (hasSegmentNo ? segmentNo : 0)
                                     << " has_segment=" << (hasSegmentNo ? 1 : 0)
                                     << " segmentName=" << segmentName
                                     << " fetch_start_to_validated_ms="
                                     << elapsedMsSince(fetchStart, now)
                                     << " interest_to_validated_ms=0"
                                     << " data_to_validated_ms=" << dataToValidatedMs);
                    });
                fetcher->afterSegmentNacked.connect(
                    [fetchStats, transportValidator] {
                        ++fetchStats->nacks;
                    });
                fetcher->afterSegmentTimedOut.connect(
                    [fetchStats, transportValidator] {
                        ++fetchStats->timeouts;
                    });
            }
            fetcher->onComplete.connect(
                [completed, mutex, cv, encoded, requestId, keyScope, dataName, fetchTimeoutMs,
                 interestLifetimeMs, fetchTimingEnabled, fetchInitCwnd, fetchStart, fetchStats,
                 transportValidator]
                (ndn::ConstBufferPtr buffer) {
                    const auto completeTime = std::chrono::steady_clock::now();
                    fetchStats->completeWall = std::chrono::system_clock::now();
                    const auto elapsedMs = elapsedMsSince(fetchStart, completeTime);
                    {
                        std::lock_guard<std::mutex> lock(*mutex);
                        encoded->assign(buffer->begin(), buffer->end());
                        completed->store(true);
                    }
                    if (fetchTimingEnabled) {
                        const double firstSegmentMs = fetchStats->receivedSegments == 0 ? 0.0 :
                            elapsedMsSince(fetchStart, fetchStats->firstSegmentReceived);
                        const double lastReceivedMs = fetchStats->receivedSegments == 0 ? 0.0 :
                            elapsedMsSince(fetchStart, fetchStats->lastSegmentReceived);
                        const double lastValidatedMs = fetchStats->validatedSegments == 0 ? 0.0 :
                            elapsedMsSince(fetchStart, fetchStats->lastSegmentValidated);
                        NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING event=complete"
                                     << " requestId=" << requestId.toUri()
                                     << " keyScope=" << keyScope
                                     << " dataName=" << dataName.toUri()
                                     << " encoded_bytes=" << buffer->size()
                                     << " elapsed_ms=" << elapsedMs
                                     << " first_segment_ms=" << firstSegmentMs
                                     << " last_segment_received_ms=" << lastReceivedMs
                                     << " last_segment_validated_ms=" << lastValidatedMs
                                     << " first_segment_epoch_ms="
                                     << epochMs(fetchStats->firstSegmentWall)
                                     << " complete_epoch_ms="
                                     << epochMs(fetchStats->completeWall)
                                     << " received_segments=" << fetchStats->receivedSegments
                                     << " validated_segments=" << fetchStats->validatedSegments
                                     << " received_wire_bytes=" << fetchStats->receivedWireBytes
                                     << " nacks=" << fetchStats->nacks
                                     << " segment_timeouts=" << fetchStats->timeouts
                                     << " timeout_ms=" << fetchTimeoutMs
                                     << " interest_lifetime_ms=" << interestLifetimeMs
                                     << " init_cwnd=" << fetchInitCwnd);
                    }
                    cv->notify_one();
                });
            fetcher->onError.connect(
                [completed, mutex, cv, error, requestId, keyScope, dataName, fetchTimeoutMs,
                 interestLifetimeMs, fetchTimingEnabled, fetchInitCwnd, fetchStart, fetchStats,
                 transportValidator]
                (uint32_t code, const std::string& msg) {
                    fetchStats->completeWall = std::chrono::system_clock::now();
                    const auto elapsedMs = elapsedMsSince(
                        fetchStart, std::chrono::steady_clock::now());
                    {
                        std::lock_guard<std::mutex> lock(*mutex);
                        *error = "SegmentFetcher error " + std::to_string(code) + ": " + msg;
                        completed->store(true);
                    }
                    if (fetchTimingEnabled) {
                        NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING event=error"
                                     << " requestId=" << requestId.toUri()
                                     << " keyScope=" << keyScope
                                     << " dataName=" << dataName.toUri()
                                     << " error_code=" << code
                                     << " elapsed_ms=" << elapsedMs
                                     << " first_segment_epoch_ms="
                                     << epochMs(fetchStats->firstSegmentWall)
                                     << " complete_epoch_ms="
                                     << epochMs(fetchStats->completeWall)
                                     << " received_segments=" << fetchStats->receivedSegments
                                     << " validated_segments=" << fetchStats->validatedSegments
                                     << " received_wire_bytes=" << fetchStats->receivedWireBytes
                                     << " nacks=" << fetchStats->nacks
                                     << " segment_timeouts=" << fetchStats->timeouts
                                     << " timeout_ms=" << fetchTimeoutMs
                                     << " interest_lifetime_ms=" << interestLifetimeMs
                                     << " init_cwnd=" << fetchInitCwnd);
                    }
                    cv->notify_one();
                });
        });

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(fetchTimeoutMs);
        std::unique_lock<std::mutex> lock(*mutex);
        cv->wait_until(lock, deadline, [&completed] { return completed->load(); });
        if (!completed->load()) {
            NDN_LOG_ERROR("Timed out fetching collaboration large Data " << dataName.toUri());
            return std::nullopt;
        }
        if (!error->empty()) {
            NDN_LOG_ERROR("Failed fetching collaboration large Data "
                          << dataName.toUri() << ": " << *error);
            return std::nullopt;
        }

        const auto fetchComplete = std::chrono::steady_clock::now();
        const double elapsedMs = elapsedMsSince(fetchStart, fetchComplete);
        const double firstSegmentMs = fetchStats->receivedSegments == 0 ? 0.0 :
            elapsedMsSince(fetchStart, fetchStats->firstSegmentReceived);
        ndn::Name producerProvider;
        if (auto parsed = parseCollaborationDataName(dataName)) {
            producerProvider = parsed->producerName;
        }
        m_networkTelemetry.updateLargeDataFetch(
            identity,
            producerProvider,
            keyScope,
            dataName,
            elapsedMs,
            firstSegmentMs,
            encoded->size(),
            fetchStats->receivedWireBytes,
            fetchStats->receivedSegments,
            fetchStats->timeouts,
            fetchStats->nacks);
        if (telemetryExportEnabled) {
            const auto snapshot =
                m_networkTelemetry.getDependencyEdge(identity, producerProvider, keyScope);
            const double goodputMbps =
                networkTelemetryGoodputMbps(fetchStats->receivedWireBytes, elapsedMs);
            NDN_LOG_WARN("NDNSF_NETWORK_TELEMETRY"
                         << " event=large_data_fetch"
                         << " sample_kind="
                         << toString(NetworkTelemetrySampleKind::LargeDataFetch)
                         << " consumerProvider=" << identity.toUri()
                         << " producerProvider=" << producerProvider.toUri()
                         << " keyScope=" << keyScope
                         << " dataName=" << dataName.toUri()
                         << " elapsed_ms=" << elapsedMs
                         << " first_byte_ms=" << firstSegmentMs
                         << " encoded_bytes=" << encoded->size()
                         << " wire_bytes=" << fetchStats->receivedWireBytes
                         << " received_segments=" << fetchStats->receivedSegments
                         << " segment_timeouts=" << fetchStats->timeouts
                         << " nacks=" << fetchStats->nacks
                         << " goodput_mbps=" << goodputMbps
                         << " confidence="
                         << (snapshot ? snapshot->confidence : 0.0)
                         << " sample_count="
                         << (snapshot ? snapshot->sampleCount : 0));
        }

        try {
            const auto decryptStart = std::chrono::steady_clock::now();
            ndn::Block block(*encoded);
            HybridMessageEnvelope envelope;
            envelope.WireDecode(block);
            const std::string adText = dataName.toUri() + "|COLLAB-LARGE|" +
                                       requestId.toUri() + "|" + keyScope;
            // Large Data is signed segment-by-segment. AES-GCM also binds the
            // ciphertext to the versioned large object name and request scope.
            const ndn::Buffer ad(reinterpret_cast<const uint8_t*>(adText.data()),
                                 adText.size());
            ndn::Buffer plaintext;
            if (!hybridAesGcmDecrypt(scopeKey,
                                     envelope,
                                     ndn::span<const uint8_t>(ad.data(), ad.size()),
                                     plaintext)) {
                return std::nullopt;
            }
            if (fetchTimingEnabled) {
                const auto decryptEnd = std::chrono::steady_clock::now();
                NDN_LOG_WARN("NDNSF_COLLAB_LARGE_FETCH_TIMING event=decrypt"
                             << " timestamp_us=" << nowMicroseconds()
                             << " requestId=" << requestId.toUri()
                             << " keyScope=" << keyScope
                             << " dataName=" << dataName.toUri()
                             << " encoded_bytes=" << encoded->size()
                             << " plaintext_bytes=" << plaintext.size()
                             << " decrypt_ms="
                             << elapsedMsSince(decryptStart, decryptEnd)
                             << " fetch_start_to_decrypt_done_ms="
                             << elapsedMsSince(fetchStart, decryptEnd));
            }
            return plaintext;
        }
        catch (const std::exception& e) {
            NDN_LOG_ERROR("Failed decrypting collaboration large Data "
                          << dataName.toUri() << ": " << e.what());
            return std::nullopt;
        }
    }

    void ServiceProvider::publishCollaborationFinalResponse(
        const ndn::Name& requesterName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage,
        const ndn::Buffer& payload,
        const std::string& selectionDigest)
    {
        if (isTruthyEnv("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE")) {
            NDN_LOG_WARN("NDNSF_COLLAB_FINAL_RESPONSE"
                         << " event=publish_requested"
                         << " requestId=" << requestId.toUri()
                         << " service=" << serviceName.toUri()
                         << " payload_bytes=" << payload.size());
        }
        ResponseMessage response;
        response.setStatus(true);
        ndn::Buffer responsePayload(payload);
        response.setPayload(responsePayload, responsePayload.size());
        if (m_useTokens) {
            response.setUserToken(requestMessage.getUserToken());
        }
        response.setPolicyEpoch(getCurrentPolicyEpoch(serviceName));
        if (const auto version = getControllerVersion(serviceName)) {
            response.setControllerVersion(*version);
        }
        boost::asio::post(m_face.getIoContext(),
            [this,
             requesterName,
             serviceName,
             requestId,
             requestMessage,
             selectionDigest,
             response = std::move(response)]() mutable {
                finishRequestExecutionOnEventLoop(requesterName,
                                                  identity,
                                                  serviceName,
                                                  requestId,
                                                  requestMessage,
                                                  std::move(response),
                                                  selectionDigest);
            });
    }

    void ServiceProvider::deliverCollaborationData(const CollaborationData& data)
    {
        std::vector<std::function<void(const CollaborationData&)>> callbacks;
        std::vector<CollaborationSubscription> contextCallbacks;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            const ndn::Name requestId(data.sessionId);
            m_collaborationDataByRequest[requestId].push_back(data);
            for (const auto& subscription : m_collaborationSubscriptions) {
                if (!subscription.requestId.equals(requestId)) {
                    continue;
                }
                if (subscription.keyScope != data.keyScope) {
                    continue;
                }
                if (!subscription.topicPrefix.isPrefixOf(data.topic)) {
                    continue;
                }
                if (subscription.onData) {
                    callbacks.push_back(subscription.onData);
                }
                if (subscription.onContextData) {
                    contextCallbacks.push_back(subscription);
                }
            }
        }
        m_collaborationCv.notify_all();
        for (auto& callback : callbacks) {
            auto invoke = [callback = std::move(callback), data]() {
                callback(data);
            };
            if (m_handlerPool.getThreadCount() == 0 ||
                !m_handlerPool.post(invoke)) {
                invoke();
            }
        }
        for (auto& subscription : contextCallbacks) {
            auto invoke = [this, subscription = std::move(subscription), data]() mutable {
                CollaborationContext ctx(*this,
                                         subscription.requesterName,
                                         subscription.requestId,
                                         subscription.requestMessage,
                                         subscription.assignment);
                subscription.onContextData(ctx, data);
            };
            if (m_handlerPool.getThreadCount() == 0 ||
                !m_handlerPool.post(invoke)) {
                invoke();
            }
        }
    }

    void ServiceProvider::addCollaborationSubscription(
        const ndn::Name& requestId,
        KeyScope keyScope,
        Topic topicPrefix,
        std::function<void(const CollaborationData&)> onData)
    {
        if (!onData) {
            return;
        }

        std::vector<CollaborationData> existing;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            CollaborationSubscription subscription;
            subscription.requestId = requestId;
            subscription.keyScope = std::move(keyScope);
            subscription.topicPrefix = std::move(topicPrefix);
            subscription.onData = onData;

            auto it = m_collaborationDataByRequest.find(requestId);
            if (it != m_collaborationDataByRequest.end()) {
                for (const auto& data : it->second) {
                    if (data.keyScope == subscription.keyScope &&
                        subscription.topicPrefix.isPrefixOf(data.topic)) {
                        existing.push_back(data);
                    }
                }
            }
            m_collaborationSubscriptions.push_back(std::move(subscription));
        }

        for (const auto& data : existing) {
            auto invoke = [onData, data]() {
                onData(data);
            };
            if (m_handlerPool.getThreadCount() == 0 ||
                !m_handlerPool.post(invoke)) {
                invoke();
            }
        }
    }

    void ServiceProvider::addCollaborationSubscription(
        const ndn::Name& requesterName,
        const ndn::Name& requestId,
        RequestMessage requestMessage,
        CollaborationAssignment assignment,
        KeyScope keyScope,
        Topic topicPrefix,
        std::function<void(CollaborationContext&, const CollaborationData&)> onData)
    {
        if (!onData) {
            return;
        }

        std::vector<CollaborationData> existing;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            CollaborationSubscription subscription;
            subscription.requesterName = requesterName;
            subscription.requestId = requestId;
            subscription.keyScope = std::move(keyScope);
            subscription.topicPrefix = std::move(topicPrefix);
            subscription.requestMessage = requestMessage;
            subscription.assignment = assignment;
            subscription.onContextData = onData;

            auto it = m_collaborationDataByRequest.find(requestId);
            if (it != m_collaborationDataByRequest.end()) {
                for (const auto& data : it->second) {
                    if (data.keyScope == subscription.keyScope &&
                        subscription.topicPrefix.isPrefixOf(data.topic)) {
                        existing.push_back(data);
                    }
                }
            }
            m_collaborationSubscriptions.push_back(std::move(subscription));
        }

        for (const auto& data : existing) {
            auto invoke = [this,
                           requesterName,
                           requestId,
                           requestMessage,
                           assignment,
                           onData,
                           data]() mutable {
                CollaborationContext ctx(*this,
                                         requesterName,
                                         requestId,
                                         requestMessage,
                                         assignment);
                onData(ctx, data);
            };
            if (m_handlerPool.getThreadCount() == 0 ||
                !m_handlerPool.post(invoke)) {
                invoke();
            }
        }
    }

    void ServiceProvider::addCollaborationReceiveFilter(
        const ndn::Name& requestId,
        KeyScope keyScope,
        Topic topicPrefix)
    {
        std::lock_guard<std::mutex> lock(m_collaborationMutex);
        CollaborationSubscription subscription;
        subscription.requestId = requestId;
        subscription.keyScope = std::move(keyScope);
        subscription.topicPrefix = std::move(topicPrefix);
        subscription.receiveFilterOnly = true;
        m_collaborationSubscriptions.push_back(std::move(subscription));
    }

    ServiceProvider::CollaborationWorkFence
    ServiceProvider::makeCollaborationWorkFence(
        const ndn::Name& requesterName, const ndn::Name& requestId,
        const ndn::Name& serviceName, std::optional<ControllerVersion> version,
        std::shared_ptr<RegistrationState> registrationState)
    {
        const auto pendingKey = ndn::Name(requesterName).append(serviceName).append(requestId);
        const auto queuedAt = std::chrono::steady_clock::now();
        auto deadline = queuedAt + std::chrono::milliseconds(std::max(
            100, intEnvOrDefault("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", 30000)));
        {
            std::lock_guard<std::mutex> lock(m_pendingCleanupDeadlineMutex);
            const auto found = m_pendingCleanupDeadlines.find(pendingKey);
            if (found != m_pendingCleanupDeadlines.end()) {
                // This is the existing cleanup horizon, not a new protocol
                // deadline. Queue admission must never extend that horizon.
                deadline = std::min(deadline, found->second);
            }
        }
        const auto stopping = m_fetchStopping;
        return {deadline,
                [this, stopping, requestId, serviceName, version, deadline,
                 registrationState] {
            if (stopping->load() || std::chrono::steady_clock::now() >= deadline ||
                // spec182: a retired scoped registration fences all work
                // queued against it.
                (registrationState && registrationState->closed) ||
                !isAcceptableControllerVersion(serviceName, version) ||
                !authorizeControllerTransition(serviceName,
                                               ProtectedTransition::PROVIDER_EXECUTION)) {
                return false;
            }
            // completeCollaborationRoleOnEventLoop clears pending ACK/token
            // bookkeeping after one role finishes, while sibling roles may
            // still be queued. That table is not collaboration lifetime
            // authority. Keep its captured deadline cap above immutable;
            // revocation and shutdown remain fenced by version, authorization,
            // the stop token, and the request's collaboration binding below.
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            const auto found = m_collaborationServiceNamesByRequest.find(requestId);
            return found != m_collaborationServiceNamesByRequest.end() &&
                   found->second == serviceName;
        }};
    }

    void ServiceProvider::prepareCollaborationAssignmentAsync(
        const ndn::Name& requesterName,
        const ndn::Name& requestId,
        CollaborationAssignment assignment,
        std::function<void(bool, std::string,
                           CollaborationAssignment)> onReady,
        std::shared_ptr<RegistrationState> registrationState)
    {
        struct FetchState
        {
            ndn::Name requestId;
            CollaborationAssignment assignment;
            std::function<void(bool, std::string,
                               CollaborationAssignment)> onReady;
            size_t pending = 0;
            bool failed = false;
            bool finished = false;
            std::string error;
            std::map<KeyScope, ndn::Buffer> fetchedKeys;
            ndn::Buffer fetchedArtifact;
            std::optional<LargeDataReference> assignmentReference;
            CollaborationWorkFence fence;
        };

        auto state = std::make_shared<FetchState>();
        state->requestId = requestId;
        state->assignment = std::move(assignment);
        state->onReady = std::move(onReady);

        if (auto reference = parseLargeDataReferencePayload(
                state->assignment.assignmentPayload)) {
            static const std::string OBJECT_TYPE =
                "application/vnd.ndnsf.collaboration-assignment-v1";
            static constexpr size_t MAX_EXTERNAL_ASSIGNMENT_BYTES =
                4 * 1024 * 1024;
            ndn::Name expectedPrefix(requesterName);
            expectedPrefix.append("NDNSF")
                          .append("LARGE-DATA")
                          .append(state->assignment.service)
                          .append(requestId);
            const bool validReference =
                reference->encrypted &&
                reference->objectType == OBJECT_TYPE &&
                reference->plaintextSize > 0 &&
                reference->plaintextSize <= MAX_EXTERNAL_ASSIGNMENT_BYTES &&
                reference->digest.rfind("sha256:", 0) == 0 &&
                reference->digest.size() == 71 &&
                expectedPrefix.isPrefixOf(reference->dataName) &&
                reference->dataName.size() >= expectedPrefix.size() + 2 &&
                reference->dataName.get(-1).isVersion();
            if (!validReference) {
                state->failed = true;
                state->error =
                    "invalid external collaboration assignment reference";
            }
            else {
                state->assignmentReference = std::move(*reference);
            }
        }

        if (state->failed ||
            !authorizeControllerTransition(state->assignment.service,
                                           ProtectedTransition::PROVIDER_EXECUTION)) {
            state->onReady(false, state->failed ? state->error :
                "collaboration preparation authority expired", std::move(state->assignment));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            m_collaborationServiceNamesByRequest[requestId] =
                state->assignment.service;
            // spec182: bind the collaboration request to the registration
            // generation the dispatch was accepted against (scoped only);
            // sibling roles share the requestId and therefore the binding.
            if (registrationState)
                m_collaborationRegistrationStates[requestId] = registrationState;
            auto& scopeKeys = m_collaborationScopeKeysByRequest[requestId];
            for (const auto& entry : state->assignment.scopeKeys) {
                scopeKeys[entry.first] = entry.second;
            }
            auto& scopeKeyDataNames =
                m_collaborationScopeKeyDataNamesByRequest[requestId];
            for (const auto& entry : state->assignment.scopeKeyDataNames) {
                if (!entry.second.empty()) {
                    scopeKeyDataNames[entry.first] = entry.second;
                }
            }
            if (!state->assignment.assignedArtifact.empty() &&
                !state->assignment.artifactPayload.empty()) {
                m_collaborationArtifacts[state->assignment.assignedArtifact.toUri()] =
                    state->assignment.artifactPayload;
            }
        }

        state->fence = makeCollaborationWorkFence(
            requesterName, requestId, state->assignment.service,
            getControllerVersion(state->assignment.service),
            registrationState);

        std::map<KeyScope, ndn::Name> keysToFetch;
        bool needsArtifactFetch = false;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            const auto keyIt = m_collaborationScopeKeysByRequest.find(requestId);
            for (const auto& entry : state->assignment.scopeKeyDataNames) {
                if (entry.second.empty()) {
                    continue;
                }
                if (keyIt != m_collaborationScopeKeysByRequest.end() &&
                    keyIt->second.count(entry.first) != 0) {
                    continue;
                }
                keysToFetch[entry.first] = entry.second;
            }
            needsArtifactFetch =
                !state->assignment.assignedArtifact.empty() &&
                !state->assignment.artifactDataName.empty() &&
                m_collaborationArtifacts.count(
                    state->assignment.assignedArtifact.toUri()) == 0;
        }

        state->pending = keysToFetch.size() + (needsArtifactFetch ? 1 : 0) +
                         (state->assignmentReference ? 1 : 0);

        auto finishIfReady = [this, state]() mutable {
            if (state->pending != 0 || state->finished) {
                return;
            }
            state->finished = true;
            if (state->failed || !state->fence.current()) {
                state->onReady(false, state->failed ? state->error :
                               "collaboration preparation authority or deadline expired",
                               std::move(state->assignment));
                return;
            }
            const bool traceAssignmentFetch =
                isTruthyEnv("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE");
            if (traceAssignmentFetch) {
                NDN_LOG_WARN("NDNSF_COLLAB_ASSIGNMENT_FETCH"
                             << " event=ready"
                             << " requestId=" << state->requestId.toUri()
                             << " role=" << state->assignment.role
                             << " failed=" << (state->failed ? "true" : "false")
                             << " error=\"" << state->error << "\"");
            }

            std::vector<PendingEncryptedCollaborationData> pending;
            {
                std::lock_guard<std::mutex> lock(m_collaborationMutex);
                auto& scopeKeys = m_collaborationScopeKeysByRequest[state->requestId];
                for (auto& entry : state->fetchedKeys) {
                    scopeKeys[entry.first] = std::move(entry.second);
                }
                if (!state->fetchedArtifact.empty() &&
                    !state->assignment.assignedArtifact.empty()) {
                    m_collaborationArtifacts[state->assignment.assignedArtifact.toUri()] =
                        std::move(state->fetchedArtifact);
                }
                auto pendingIt =
                    m_pendingEncryptedCollaborationData.find(state->requestId);
                if (pendingIt != m_pendingEncryptedCollaborationData.end()) {
                    pending = std::move(pendingIt->second);
                    m_pendingEncryptedCollaborationData.erase(pendingIt);
                }
            }

            for (const auto& item : pending) {
                decryptCollaborationDataOrQueue(item.dataName,
                                                item.requestId,
                                                item.producer,
                                                item.message);
            }

            state->onReady(!state->failed, state->error,
                           std::move(state->assignment));
        };

        auto startFetch = [this, state, finishIfReady](
                              const ndn::Name& dataName,
                              std::function<void(const ndn::Buffer&)> onPlaintext) mutable {
            const auto serviceName = state->assignment.service.toUri();
            const bool traceAssignmentFetch =
                isTruthyEnv("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE");
            const auto stopping = m_fetchStopping;
            const bool queued = m_fetchPool.post(
                [this,
                 stopping,
                 state,
                 finishIfReady,
                 serviceName,
                 dataName,
                 traceAssignmentFetch,
                 onPlaintext = std::move(onPlaintext)]() mutable {
                    if (stopping->load()) return;
                    if (traceAssignmentFetch) {
                        NDN_LOG_WARN("NDNSF_COLLAB_ASSIGNMENT_FETCH"
                                     << " event=start"
                                     << " requestId=" << state->requestId.toUri()
                                     << " role=" << state->assignment.role
                                     << " service=" << serviceName
                                     << " dataName=" << dataName.toUri());
                    }
                    LargeDataFetchResult result;
                    try {
                        if (state->fence.current()) {
                            result = fetchAndDecryptLargeDataUntil(
                                dataName, serviceName, state->fence.deadline, state->fence.current);
                        }
                        else {
                            result.errorMessage = "collaboration fetch authority or deadline expired";
                        }
                    }
                    catch (const std::exception& error) {
                        result.errorMessage = error.what();
                    }
                    catch (...) {
                        result.errorMessage = "unknown collaboration assignment fetch error";
                    }
                    boost::asio::post(m_face.getIoContext(),
                        [state, stopping,
                         finishIfReady,
                         dataName,
                         traceAssignmentFetch,
                         onPlaintext = std::move(onPlaintext),
                         result = std::move(result)]() mutable {
                            if (stopping->load()) return;
                            if (result.success && !state->fence.current()) {
                                result.success = false;
                                result.plaintext.clear();
                                result.errorMessage = "collaboration completion authority or deadline expired";
                            }
                            if (result.success) {
                                ndn::Buffer buffer(result.plaintext.begin(),
                                                   result.plaintext.end());
                                if (traceAssignmentFetch) {
                                    NDN_LOG_WARN("NDNSF_COLLAB_ASSIGNMENT_FETCH"
                                                 << " event=done"
                                                 << " requestId=" << state->requestId.toUri()
                                                 << " role=" << state->assignment.role
                                                 << " dataName=" << dataName.toUri()
                                                 << " bytes=" << buffer.size());
                                }
                                onPlaintext(buffer);
                                if (state->pending > 0) {
                                    --state->pending;
                                }
                                finishIfReady();
                                return;
                            }
                            state->failed = true;
                            if (!state->error.empty()) {
                                state->error += "; ";
                            }
                            state->error += dataName.toUri() + ": " +
                                            result.errorMessage;
                            if (traceAssignmentFetch) {
                                NDN_LOG_WARN("NDNSF_COLLAB_ASSIGNMENT_FETCH"
                                             << " event=error"
                                             << " requestId=" << state->requestId.toUri()
                                             << " role=" << state->assignment.role
                                             << " dataName=" << dataName.toUri()
                                             << " error=\"" << result.errorMessage << "\"");
                            }
                            if (state->pending > 0) {
                                --state->pending;
                            }
                            finishIfReady();
                        });
                });
            if (!queued) {
                state->failed = true;
                state->error += "collaboration fetch queue is full; ";
                if (state->pending > 0) --state->pending;
                finishIfReady();
            }
        };

        for (const auto& entry : keysToFetch) {
            startFetch(entry.second,
                       [state, keyScope = entry.first](const ndn::Buffer& buffer) {
                           if (buffer.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                               state->failed = true;
                               if (!state->error.empty()) {
                                   state->error += "; ";
                               }
                               state->error += "invalid collaboration scope key " +
                                               keyScope;
                               return;
                           }
                           state->fetchedKeys[keyScope] = buffer;
                       });
        }

        if (needsArtifactFetch) {
            startFetch(state->assignment.artifactDataName,
                       [state](const ndn::Buffer& buffer) {
                           state->fetchedArtifact = buffer;
                       });
        }

        if (state->assignmentReference) {
            const auto reference = *state->assignmentReference;
            startFetch(reference.dataName,
                       [state, reference](const ndn::Buffer& buffer) {
                           if (buffer.size() != reference.plaintextSize ||
                               sha256DigestString(buffer) != reference.digest) {
                               state->failed = true;
                               if (!state->error.empty()) {
                                   state->error += "; ";
                               }
                               state->error +=
                                   "external collaboration assignment size or "
                                   "digest mismatch";
                               return;
                           }
                           state->assignment.assignmentPayload = buffer;
                       });
        }

        finishIfReady();
    }

    void ServiceProvider::decryptCollaborationDataOrQueue(
        const ndn::Name& dataName,
        const ndn::Name& requestId,
        const ndn::Name& producer,
        const CollaborationDataMessage& message)
    {
        ndn::Buffer scopeKey;
        bool needScopeKeyFetch = false;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            auto requestIt = m_collaborationScopeKeysByRequest.find(requestId);
            if (requestIt != m_collaborationScopeKeysByRequest.end()) {
                auto keyIt = requestIt->second.find(message.getKeyScope());
                if (keyIt != requestIt->second.end()) {
                    scopeKey = keyIt->second;
                }
            }
            if (scopeKey.empty()) {
                m_pendingEncryptedCollaborationData[requestId].push_back(
                    PendingEncryptedCollaborationData{dataName, requestId,
                                                      producer, message});
                needScopeKeyFetch = true;
            }
        }
        if (needScopeKeyFetch) {
            maybeFetchCollaborationScopeKey(requestId, message.getKeyScope());
            return;
        }
        if (scopeKey.empty()) {
            return;
        }

        auto decryptAndDeliver = [this, dataName, requestId, producer,
                                  scopeKey = std::move(scopeKey),
                                  message]() mutable {
            CollaborationData data;
            data.sessionId = requestId.toUri();
            data.keyScope = message.getKeyScope();
            data.topic = message.getTopic();
            data.producer = producer;
            data.producerRole = message.getProducerRole();
            data.sequence = message.getSequence();

            bool ok = false;
            try {
                ndn::Block envelopeBlock(message.getPayload());
                HybridMessageEnvelope envelope;
                if (envelope.WireDecode(envelopeBlock)) {
                    auto ad = collaborationAssociatedData(dataName,
                                                          requestId,
                                                          message,
                                                          envelope.getKeyId(),
                                                          envelope.getEpochId());
                    if (isTruthyEnv("NDNSF_COLLAB_AUTH_TRACE")) {
                        NDN_LOG_WARN("NDNSF_COLLAB_AUTH_TRACE event=decrypt"
                                     << " provider=" << identity.toUri()
                                     << " producer=" << producer.toUri()
                                     << " requestId=" << requestId.toUri()
                                     << " dataName=" << dataName.toUri()
                                     << " keyScope=" << message.getKeyScope()
                                     << " producerRole=" << message.getProducerRole()
                                     << " sequence=" << message.getSequence()
                                     << " keyDigest=" << sha256DigestString(scopeKey)
                                     << " adDigest=" << sha256DigestString(ad)
                                     << " envelopeKeyId=" << envelope.getKeyId()
                                     << " envelopeEpochId=" << envelope.getEpochId());
                    }
                    ok = hybridAesGcmDecrypt(
                        scopeKey,
                        envelope,
                        ndn::span<const uint8_t>(ad.data(), ad.size()),
                        data.payload);
                }
            }
            catch (const std::exception&) {
                ok = false;
            }

            if (!ok) {
                NDN_LOG_ERROR("Collaboration data authentication failed for "
                              << dataName.toUri());
                return;
            }
            // The caller may be a one-worker collaboration handler waiting in
            // waitFor().  Publish the decrypted record to the protected queue
            // immediately so its condition variable can wake that handler;
            // only user callbacks are dispatched asynchronously by
            // deliverCollaborationData().
            deliverCollaborationData(data);
        };
        // Collaboration handlers may synchronously wait for this record with
        // waitFor().  Queueing decryption on the same handler pool would
        // deadlock a one-worker provider: the waiting handler occupies the
        // only worker while the control record waits behind it.  These
        // bounded collaboration records are decrypted inline; callbacks
        // remain asynchronous through deliverCollaborationData().
        decryptAndDeliver();
    }

    bool ServiceProvider::maybeFetchCollaborationScopeKey(
        const ndn::Name& requestId,
        const KeyScope& keyScope)
    {
        ndn::Name keyDataName;
        ndn::Name serviceName;
        const std::string fetchKey = requestId.toUri() + "|" + keyScope;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            auto cachedIt = m_collaborationScopeKeysByRequest.find(requestId);
            if (cachedIt != m_collaborationScopeKeysByRequest.end() &&
                cachedIt->second.count(keyScope) != 0) {
                return false;
            }
            auto namesIt = m_collaborationScopeKeyDataNamesByRequest.find(requestId);
            if (namesIt == m_collaborationScopeKeyDataNamesByRequest.end()) {
                return false;
            }
            auto nameIt = namesIt->second.find(keyScope);
            if (nameIt == namesIt->second.end() || nameIt->second.empty()) {
                return false;
            }
            auto serviceIt = m_collaborationServiceNamesByRequest.find(requestId);
            if (serviceIt == m_collaborationServiceNamesByRequest.end() ||
                serviceIt->second.empty()) {
                NDN_LOG_ERROR("Missing collaboration service name for scope key fetch "
                              << requestId.toUri() << " scope=" << keyScope);
                return false;
            }
            if (!m_collaborationScopeKeyFetchesInFlight.insert(fetchKey).second) {
                return false;
            }
            keyDataName = nameIt->second;
            serviceName = serviceIt->second;
        }

        // scopeKeyData names identify hybrid segmented large-data objects.
        // Fetching them through NAC-ABE Consumer::consume races the assignment
        // prefetch and attempts to decode HybridMessageEnvelope (TLV 172) as
        // NAC encrypted content (TLV 602), which can terminate the Face loop.
        // Use the same hybrid large-data path as assignment prefetch, off the
        // event-loop thread because that path waits for asynchronous fetches.
        const auto stopping = m_fetchStopping;
        const auto fence = makeCollaborationWorkFence(
            ndn::Name(), requestId, serviceName, getControllerVersion(serviceName));
        const bool queued = m_fetchPool.post([this, stopping, fence, keyDataName, serviceName, requestId, keyScope,
                     fetchKey]() mutable {
            if (stopping->load()) return;
            LargeDataFetchResult result;
            try {
                if (fence.current()) {
                    result = fetchAndDecryptLargeDataUntil(
                        keyDataName, serviceName.toUri(), fence.deadline, fence.current);
                }
                else {
                    result.errorMessage = "collaboration scope key authority or deadline expired";
                }
            }
            catch (const std::exception& e) {
                result.success = false;
                result.errorMessage = e.what();
            }
            catch (...) {
                result.success = false;
                result.errorMessage = "unknown collaboration scope key fetch error";
            }

            boost::asio::post(m_face.getIoContext(),
                [this, stopping, fence, requestId, keyScope, fetchKey,
                 result = std::move(result)]() mutable {
                    if (stopping->load()) return;
                    const bool current = fence.current();
                    std::vector<PendingEncryptedCollaborationData> pending;
                    {
                        std::lock_guard<std::mutex> lock(m_collaborationMutex);
                        m_collaborationScopeKeyFetchesInFlight.erase(fetchKey);
                        if (!result.success || !current) {
                            NDN_LOG_ERROR("Failed to fetch collaboration scope key for "
                                          << requestId.toUri()
                                          << " scope=" << keyScope
                                          << ": " << result.errorMessage);
                            return;
                        }
                        if (result.plaintext.size() !=
                            HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                            NDN_LOG_ERROR("Fetched invalid collaboration scope key for "
                                          << requestId.toUri()
                                          << " scope=" << keyScope);
                            return;
                        }
                        ndn::Buffer buffer(result.plaintext.begin(),
                                           result.plaintext.end());
                        m_collaborationScopeKeysByRequest[requestId][keyScope] =
                            std::move(buffer);
                        auto pendingIt =
                            m_pendingEncryptedCollaborationData.find(requestId);
                        if (pendingIt != m_pendingEncryptedCollaborationData.end()) {
                            pending = std::move(pendingIt->second);
                            m_pendingEncryptedCollaborationData.erase(pendingIt);
                        }
                    }
                    for (const auto& item : pending) {
                        decryptCollaborationDataOrQueue(item.dataName,
                                                        item.requestId,
                                                        item.producer,
                                                        item.message);
                    }
                });
        });
        if (!queued) {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            m_collaborationScopeKeyFetchesInFlight.erase(fetchKey);
        }
        return queued;
    }

    std::vector<ServiceProvider::CollaborationData>
    ServiceProvider::waitForCollaborationData(
        const ndn::Name& requestId,
        const std::string& keyScope,
        const ndn::Name& topicPrefix,
        size_t minCount,
        int timeoutMs)
    {
        auto matches = [&] {
            std::vector<CollaborationData> result;
            auto it = m_collaborationDataByRequest.find(requestId);
            if (it == m_collaborationDataByRequest.end()) {
                return result;
            }
            for (const auto& data : it->second) {
                if (data.keyScope != keyScope) {
                    continue;
                }
                if (!topicPrefix.isPrefixOf(data.topic)) {
                    continue;
                }
                result.push_back(data);
            }
            return result;
        };

        std::unique_lock<std::mutex> lock(m_collaborationMutex);
        auto current = matches();
        if (current.size() >= minCount) {
            return current;
        }
        m_collaborationCv.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [&] {
                current = matches();
                return current.size() >= minCount;
            });
        return current;
    }

    void ServiceProvider::onCollaborationDataMessage(
        const ndn::svs::SVSPubSub::SubscriptionData& subscription)
    {
        if (!isFresh(subscription)) {
            return;
        }
        auto parsed = parseCollaborationDataName(subscription.name);
        if (!parsed) {
            return;
        }
        if (parsed->producerName.equals(identity)) {
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
            return;
        }

        // User-owned COMMIT/ROLLBACK controls are addressed to one exact
        // Provider in the requester-name component of the collaboration
        // Data name.  The shared SVS subscription also carries ordinary
        // Provider-to-Provider data addressed to the original User, so this
        // recipient check is intentionally restricted to the reserved
        // user-control producer role.
        if (message.getProducerRole() == "user-control-v1" &&
            !parsed->requesterName.equals(identity)) {
            return;
        }

        // The SVS subscription intentionally covers every collaboration
        // packet.  Once a request installs one or more role bindings, drop
        // packets outside those bindings before looking up a key or trying
        // authentication.  Otherwise another role's ciphertext can be
        // attempted with a local key of the same name and produce a false
        // authentication failure.
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            bool hasReceiveFilter = false;
            bool matchesReceiveFilter = false;
            for (const auto& filter : m_collaborationSubscriptions) {
                if (!filter.requestId.equals(parsed->requestId)) {
                    continue;
                }
                hasReceiveFilter = true;
                if (filter.keyScope == message.getKeyScope() &&
                    filter.topicPrefix.isPrefixOf(message.getTopic())) {
                    matchesReceiveFilter = true;
                    break;
                }
            }
            if (hasReceiveFilter && !matchesReceiveFilter) {
                return;
            }
        }

        decryptCollaborationDataOrQueue(subscription.name,
                                        parsed->requestId,
                                        parsed->producerName,
                                        message);
    }

    ServiceProvider::CollaborationAssignment
    ServiceProvider::parseCollaborationAssignment(const ndn::Name& serviceName,
                                                  const ndn::Buffer& payload)
    {
        CollaborationAssignment assignment;
        assignment.service = serviceName;
        assignment.assignmentPayload = payload;
        if (payload.empty()) {
            assignment.role = serviceName.toUri();
            return assignment;
        }

        ndn::Buffer fieldPayload = payload;
        CollaborationAssignmentEnvelope envelope;
        if (decodeCollaborationAssignmentEnvelope(payload, envelope)) {
            assignment.role = std::move(envelope.role);
            assignment.assignedArtifact =
                std::move(envelope.assignedArtifact);
            assignment.artifactDataName =
                std::move(envelope.artifactDataName);
            assignment.requiresProvisioning =
                envelope.requiresProvisioning;
            assignment.provisioningTimeoutMs = static_cast<int>(
                std::min<uint64_t>(
                    envelope.provisioningTimeoutMs,
                    static_cast<uint64_t>(std::numeric_limits<int>::max())));
            assignment.scopeKeys = std::move(envelope.scopeKeys);
            assignment.scopeKeyDataNames = std::move(envelope.scopeKeyDataNames);
            assignment.assignmentPayload =
                std::move(envelope.opaquePayload);
            fieldPayload = assignment.assignmentPayload;
        }
        else {
            // A single Provider may be selected for several collaboration
            // roles.  ServiceUser groups those per-role envelopes into one
            // canonical OpaqueAssignmentSet before publishing Selection.
            // Parse the first envelope as the execution context and retain
            // the scope-key material from every local role.  The native DI
            // handler executes the complete local plan once; treating the
            // container TLV as a semicolon assignment would otherwise leave
            // role set unresolved and fall back to the service name.
            std::vector<ndn::Buffer> assignmentItems;
            try {
                assignmentItems = decodeOpaqueAssignmentSet(payload);
            }
            catch (const std::exception&) {
                // Preserve the existing fail-closed fallback for malformed
                // or non-canonical containers.
                assignmentItems.clear();
            }
            if (assignmentItems.size() > 1) {
                CollaborationAssignmentEnvelope first;
                bool haveEnvelope = false;
                for (const auto& item : assignmentItems) {
                    CollaborationAssignmentEnvelope itemEnvelope;
                    if (!decodeCollaborationAssignmentEnvelope(item, itemEnvelope)) {
                        continue;
                    }
                    if (!haveEnvelope) {
                        first = std::move(itemEnvelope);
                        haveEnvelope = true;
                        continue;
                    }
                    for (const auto& entry : itemEnvelope.scopeKeys) {
                        first.scopeKeys.emplace(entry.first, entry.second);
                    }
                    for (const auto& entry : itemEnvelope.scopeKeyDataNames) {
                        first.scopeKeyDataNames.emplace(entry.first, entry.second);
                    }
                    if (!itemEnvelope.artifactDataName.empty()) {
                        if (!first.artifactDataName.empty() &&
                            !first.artifactDataName.equals(
                                itemEnvelope.artifactDataName)) {
                            throw std::runtime_error(
                                "conflicting collaboration artifact Data names");
                        }
                        first.artifactDataName = itemEnvelope.artifactDataName;
                    }
                }
                if (haveEnvelope) {
                    assignment.role = first.role;
                    assignment.assignedArtifact = first.assignedArtifact;
                    assignment.artifactDataName = first.artifactDataName;
                    assignment.requiresProvisioning = first.requiresProvisioning;
                    assignment.provisioningTimeoutMs = static_cast<int>(
                        std::min<uint64_t>(
                            first.provisioningTimeoutMs,
                            static_cast<uint64_t>(std::numeric_limits<int>::max())));
                    assignment.scopeKeys = std::move(first.scopeKeys);
                    assignment.scopeKeyDataNames = std::move(first.scopeKeyDataNames);
                    assignment.assignmentPayload = std::move(first.opaquePayload);
                    fieldPayload = assignment.assignmentPayload;
                }
            }
        }

        const auto fields = parseSemicolonFields(fieldPayload);
        auto readField = [&fields](const std::string& key) {
            auto it = fields.find(key);
            return it == fields.end() ? std::string() : it->second;
        };

        if (assignment.role.empty()) {
            assignment.role = readField("role");
        }
        if (assignment.role.empty()) {
            assignment.role = serviceName.toUri();
        }
        const auto artifact = readField("artifact");
        if (!artifact.empty()) {
            assignment.assignedArtifact = ndn::Name(artifact);
        }
        const auto artifactDataName = readField("artifactDataName");
        if (!artifactDataName.empty()) {
            assignment.artifactDataName = ndn::Name(artifactDataName);
        }
        if (fields.find("requiresProvisioning") != fields.end()) {
            assignment.requiresProvisioning =
                readField("requiresProvisioning") == "1";
        }
        const auto timeout = readField("provisioningTimeoutMs");
        if (!timeout.empty()) {
            try {
                assignment.provisioningTimeoutMs = std::stoi(timeout);
            }
            catch (const std::exception&) {
                assignment.provisioningTimeoutMs = 0;
            }
        }
        for (const auto& field : fields) {
            static const std::string prefix = "scopeKey.";
            if (field.first.rfind(prefix, 0) == 0) {
                auto key = hexDecode(field.second);
                if (key.size() == HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                    assignment.scopeKeys[field.first.substr(prefix.size())] =
                        std::move(key);
                }
            }
            static const std::string keyDataPrefix = "scopeKeyData.";
            if (field.first.rfind(keyDataPrefix, 0) == 0 && !field.second.empty()) {
                assignment.scopeKeyDataNames[field.first.substr(keyDataPrefix.size())] =
                    ndn::Name(field.second);
            }
            static const std::string roleProviderPrefix = "roleProvider.";
            if (field.first.rfind(roleProviderPrefix, 0) == 0 && !field.second.empty()) {
                assignment.roleProviders[field.first.substr(roleProviderPrefix.size())] =
                    ndn::Name(field.second);
            }
        }
        const auto artifactData = readField("artifactData");
        if (!artifactData.empty()) {
            assignment.artifactPayload = hexDecode(artifactData);
        }
        return assignment;
    }

    ResponseMessage ServiceProvider::dispatchRequest(
        const ndn::Name& requesterIdentity,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage) const
    {
        auto service = m_services.find(serviceName);
        if (service == m_services.end()) {
            return makeErrorResponse("No handler registered for " + serviceName.toUri());
        }

        const bool targetedMode =
            requestMessage.getRequestMode() == tlv::TargetedRequest ||
            requestMessage.getRequestMode() == tlv::TargetedBootstrapRequest;
        const auto& requestHandler =
            targetedMode ? service->second.targetedRequestHandler
                         : service->second.requestHandler;
        if (!requestHandler) {
            return makeErrorResponse("Registered service has no request handler for " +
                                     serviceName.toUri());
        }

        return requestHandler(requesterIdentity,
                              providerName,
                              serviceName,
                              requestId,
                              requestMessage);
    }

    ResponseMessage ServiceProvider::handleDecryptedRequestByName(
        const ndn::Name& requestName,
        const RequestMessage& requestMessage) const
    {
        auto parsedV2 = ndn_service_framework::parseRequestNameV2(requestName);
        if (parsedV2) {
            const auto requestVersion = requestMessage.hasControllerVersion() ?
                std::optional<ControllerVersion>(requestMessage.getControllerVersion()) :
                std::nullopt;
            maybeRefreshControllerVersionHint(parsedV2->serviceName, requestVersion);
            if (!authorizeControllerTransition(parsedV2->serviceName,
                                               ProtectedTransition::PROVIDER_EXECUTION)) {
                return makeErrorResponse("Controller revoked provider authorization for " +
                                         parsedV2->serviceName.toUri());
            }
            if (!isAcceptablePolicyEpoch(parsedV2->serviceName,
                                         requestMessage.getPolicyEpoch())) {
                return makeErrorResponse("Stale policy epoch for " +
                                         parsedV2->serviceName.toUri());
            }
            if (!isAcceptableControllerVersion(parsedV2->serviceName,
                                               requestVersion)) {
                return makeErrorResponse("Stale controller version for " +
                                         parsedV2->serviceName.toUri());
            }
            if (!hasProviderPermission(identity, parsedV2->serviceName, m_authorizations)) {
                return makeErrorResponse("Permission denied for " +
                                         parsedV2->serviceName.toUri());
            }
            if (m_useTokens && requestMessage.getUserToken().empty()) {
                return makeErrorResponse("Missing UserToken for " +
                                         parsedV2->serviceName.toUri());
            }
            auto service = m_services.find(parsedV2->serviceName);
            if (requestMessage.getRequestMode() == tlv::TargetedRequest) {
                if (requestMessage.getTargetProvider().empty()) {
                    return makeErrorResponse("Targeted request missing target provider for " +
                                             parsedV2->serviceName.toUri());
                }
                if (!requestMessage.getTargetProvider().equals(identity)) {
                    return makeErrorResponse("Targeted request is for " +
                                             requestMessage.getTargetProvider().toUri());
                }
                if (service == m_services.end() ||
                    !service->second.targetedRequestHandler) {
                    return makeErrorResponse("Service is not registered for targeted mode for " +
                                             parsedV2->serviceName.toUri());
                }
                std::string tokenError;
                if (!consumeTargetedProviderToken(parsedV2->requesterName,
                                                  parsedV2->serviceName,
                                                  requestMessage,
                                                  tokenError)) {
                    return makeErrorResponse(tokenError + " for " +
                                             parsedV2->serviceName.toUri());
                }
            }
            else if (requestMessage.getRequestMode() == tlv::TargetedBootstrapRequest) {
                if (requestMessage.getTargetProvider().empty()) {
                    return makeErrorResponse("Targeted bootstrap missing target provider for " +
                                             parsedV2->serviceName.toUri());
                }
                if (!requestMessage.getTargetProvider().equals(identity)) {
                    return makeErrorResponse("Targeted bootstrap is for " +
                                             requestMessage.getTargetProvider().toUri());
                }
                if (service == m_services.end() ||
                    !service->second.targetedRequestHandler) {
                    return makeErrorResponse("Service is not registered for targeted mode for " +
                                             parsedV2->serviceName.toUri());
                }
            }
            else if (service != m_services.end() &&
                     !service->second.requestHandler &&
                     service->second.targetedRequestHandler) {
                return makeErrorResponse("Service is targeted-only for " +
                                         parsedV2->serviceName.toUri());
            }
            if (requestMessage.getStrategy() == tlv::AllSelected) {
                return makeErrorResponse("AllSelected requires selection before execution for " +
                                         parsedV2->serviceName.toUri());
            }

            auto response = dispatchRequest(parsedV2->requesterName,
                                            identity,
                                            parsedV2->serviceName,
                                            parsedV2->requestId,
                                            requestMessage);
            if (m_useTokens) {
                response.setUserToken(requestMessage.getUserToken());
            }
            if (requestMessage.getRequestMode() == tlv::TargetedBootstrapRequest &&
                service != m_services.end() &&
                service->second.targetedRequestHandler) {
                attachTargetedTokenBatch(parsedV2->requesterName,
                                         parsedV2->serviceName,
                                         requestMessage,
                                         response);
            }
            response.setPolicyEpoch(getCurrentPolicyEpoch(parsedV2->serviceName));
            if (const auto version = getControllerVersion(parsedV2->serviceName)) {
                response.setControllerVersion(*version);
            }
            return response;
        }

        return makeErrorResponse("Non-V2 request name rejected: " +
                                 requestName.toUri());
    }

    ResponseMessage ServiceProvider::handleDecryptedRequestByName(
        const ndn::Name& requestName,
        const ndn::Block& requestBlock) const
    {
        RequestMessage requestMessage;
        if (!requestMessage.WireDecode(requestBlock)) {
            return makeErrorResponse("Failed to decode RequestMessage for " +
                                     requestName.toUri());
        }

        return handleDecryptedRequestByName(requestName, requestMessage);
    }

    ResponseMessage ServiceProvider::makeErrorResponse(const std::string& errorInfo)
    {
        ResponseMessage response;
        response.setStatus(false);
        response.setErrorInfo(errorInfo);
        return response;
    }

    ServiceProvider::AckDecision ServiceProvider::makeDefaultAckDecision()
    {
        AckDecision decision;
        decision.status = true;
        decision.message = "Permission Granted";
        return decision;
    }

    std::string ServiceProvider::fencePendingRegistrationExecution(
        const ndn::Name& pendingKey,
        const std::shared_ptr<RegistrationState>& entryState)
    {
        if (!entryState) {
            return std::string();
        }
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        const auto boundIt = m_pendingRegistrationStates.find(pendingKey);
        if (boundIt == m_pendingRegistrationStates.end()) {
            // No acceptance-time binding (targeted acceptances, or a
            // Selection whose pending entry was already consumed): the
            // request carries no earlier generation claim, so the current
            // entry generation governs.
            return std::string();
        }
        if (boundIt->second != entryState) {
            // The acceptance was bound to an earlier generation than the
            // entry the dispatcher resolved now; refuse and drop the stale
            // binding (execution will not happen for this acceptance).
            m_pendingRegistrationStates.erase(boundIt);
            return "registration generation changed before execution";
        }
        // Claim the binding: this is the first execution dispatch for the
        // accepted request.  Execution from here on fences via the captured
        // registration state, so the pendingKey binding is released.
        m_pendingRegistrationStates.erase(boundIt);
        return std::string();
    }

    bool ServiceProvider::gateInlineRequestExecution(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage,
        std::string selectionDigest,
        std::shared_ptr<RegistrationState>& registrationState)
    {
        registrationState.reset();
        const auto service = m_services.find(serviceName);
        if (service == m_services.end() || !service->second.registrationState) {
            return true;
        }
        registrationState = service->second.registrationState;
        if (registrationState->closed) {
            publishExecutionFailureOnEventLoop(requesterName,
                                               providerName,
                                               serviceName,
                                               requestId,
                                               requestMessage,
                                               "Service registration closed before execution",
                                               std::move(selectionDigest));
            return false;
        }
        const ndn::Name pendingKey = ndn::Name(requesterName)
                                         .append(serviceName).append(requestId);
        const std::string fenceError =
            fencePendingRegistrationExecution(pendingKey, registrationState);
        if (!fenceError.empty()) {
            publishExecutionFailureOnEventLoop(requesterName,
                                               providerName,
                                               serviceName,
                                               requestId,
                                               requestMessage,
                                               fenceError,
                                               std::move(selectionDigest));
            return false;
        }
        return true;
    }

    void ServiceProvider::cleanupPendingRequestState(const ndn::Name& pendingKey,
                                                     bool preserveReplayTombstone)
    {
        ++m_cleanupInvocationCount;
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PENDING_CLEANUP timestamp_us="
                  << nowMicroseconds()
                  << " providerName=" << identity.toUri()
                  << " pendingKey=" << pendingKey.toUri()
                  << " hadRequest=" << (pendingRequests.find(pendingKey) != pendingRequests.end())
                  << " hadProviderToken="
                  << (pendingProviderTokens.find(pendingKey) != pendingProviderTokens.end()));
        auto tokenHashIt = m_pendingRequestTokenHashes.find(pendingKey);
        if (tokenHashIt != m_pendingRequestTokenHashes.end()) {
            m_recentProviderRequestTokenHashes.erase(tokenHashIt->second);
            m_pendingRequestTokenHashes.erase(tokenHashIt);
        }
        pendingRequests.erase(pendingKey);
        // spec182: the registration-generation binding shares the pending
        // request's lifetime exactly; release it together.
        m_pendingRegistrationStates.erase(pendingKey);
        pendingProviderTokens.erase(pendingKey);
        auto streamIt = m_streamLifecycles.find(pendingKey);
        if (streamIt != m_streamLifecycles.end()) {
            auto& lifecycle = *streamIt->second;
            // Pending-state destruction must not silently abandon an active
            // streamed invocation.  Fence the provider view before releasing
            // the map owner; an already terminal shared authority needs no
            // second terminal claim.
            if (!lifecycle.terminalAuthority()->isTerminal() &&
                !lifecycle.terminalAuthority()->isFenced()) {
                lifecycle.provider().fence();
            }
            m_streamLifecycles.erase(streamIt);
        }
        pendingReservationLeases.erase(pendingKey);
        m_recentProviderRequests.erase(pendingKey);
        m_selectedProviderRequests.erase(pendingKey);
        m_selectionDecryptsInFlight.erase(pendingKey);
        auto requestScopedIt = m_requestScopedInvocations.find(pendingKey);
        if (requestScopedIt != m_requestScopedInvocations.end()) {
            const auto keyId = requestScopedIt->second.keys.keyId;
            requestScopedIt->second.keys.zeroize();
            m_requestScopedInvocations.erase(requestScopedIt);
            m_requestScopedNonceRegistry.invalidate(keyId);
        }
        auto selectedTokenHashIt = m_selectedProviderTokenHashes.find(pendingKey);
        if (selectedTokenHashIt != m_selectedProviderTokenHashes.end()) {
            if (!preserveReplayTombstone) {
                m_consumedProviderTokenHashes.erase(selectedTokenHashIt->second);
            }
            m_selectedProviderTokenHashes.erase(selectedTokenHashIt);
        }
        {
            std::lock_guard<std::mutex> deadlineLock(
                m_pendingCleanupDeadlineMutex);
            m_pendingCleanupDeadlines.erase(pendingKey);
            m_pendingCleanupExpiryUnixMs.erase(pendingKey);
            m_authoritativePendingCleanupDeadlines.erase(pendingKey);
        }
    }

    bool ServiceProvider::expirePendingRequestState(const ndn::Name& pendingKey)
    {
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        const bool hadRequest = pendingRequests.find(pendingKey) != pendingRequests.end();
        const bool hadToken = pendingProviderTokens.find(pendingKey) != pendingProviderTokens.end();
        const bool hadRecent = m_recentProviderRequests.find(pendingKey) != m_recentProviderRequests.end();
        const bool hadRequestToken = m_pendingRequestTokenHashes.find(pendingKey) !=
                                     m_pendingRequestTokenHashes.end();
        if (!hadRequest && !hadToken && !hadRecent && !hadRequestToken) {
            std::lock_guard<std::mutex> deadlineLock(
                m_pendingCleanupDeadlineMutex);
            m_pendingCleanupDeadlines.erase(pendingKey);
            m_pendingCleanupExpiryUnixMs.erase(pendingKey);
            m_authoritativePendingCleanupDeadlines.erase(pendingKey);
            return false;
        }
        ++m_cleanupInvocationCount;

        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PENDING_EXPIRED timestamp_us="
                  << nowMicroseconds()
                  << " providerName=" << identity.toUri()
                  << " pendingKey=" << pendingKey.toUri()
                  << " hadRequest=" << hadRequest
                  << " hadProviderToken=" << hadToken
                  << " hadRecent=" << hadRecent
                  << " hadRequestToken=" << hadRequestToken);
        if (!pendingKey.empty()) {
            updateProviderRequestLifecycleState(
                ndn::Name(pendingKey[-1].toUri()),
                ndn::Name(),
                ProviderRequestLifecycleState::PROVIDER_REQUEST_EXPIRED);
        }
        auto tokenHashIt = m_pendingRequestTokenHashes.find(pendingKey);
        if (tokenHashIt != m_pendingRequestTokenHashes.end()) {
            m_recentProviderRequestTokenHashes.erase(tokenHashIt->second);
            m_pendingRequestTokenHashes.erase(tokenHashIt);
        }
        pendingRequests.erase(pendingKey);
        // spec182: expire the registration-generation binding with the
        // pending request it mirrors.
        m_pendingRegistrationStates.erase(pendingKey);
        pendingProviderTokens.erase(pendingKey);
        auto requestScopedIt = m_requestScopedInvocations.find(pendingKey);
        if (requestScopedIt != m_requestScopedInvocations.end()) {
            const auto keyId = requestScopedIt->second.keys.keyId;
            requestScopedIt->second.keys.zeroize();
            m_requestScopedInvocations.erase(requestScopedIt);
            m_requestScopedNonceRegistry.invalidate(keyId);
        }
        auto streamIt = m_streamLifecycles.find(pendingKey);
        if (streamIt != m_streamLifecycles.end()) {
            auto& lifecycle = *streamIt->second;
            if (!lifecycle.terminalAuthority()->isTerminal() &&
                !lifecycle.terminalAuthority()->isFenced()) {
                lifecycle.provider().fence();
            }
            m_streamLifecycles.erase(streamIt);
        }
        pendingReservationLeases.erase(pendingKey);
        m_recentProviderRequests.erase(pendingKey);
        m_selectedProviderRequests.erase(pendingKey);
        m_selectionDecryptsInFlight.erase(pendingKey);
        auto selectedTokenHashIt = m_selectedProviderTokenHashes.find(pendingKey);
        if (selectedTokenHashIt != m_selectedProviderTokenHashes.end()) {
            m_consumedProviderTokenHashes.erase(selectedTokenHashIt->second);
            m_selectedProviderTokenHashes.erase(selectedTokenHashIt);
        }
        {
            std::lock_guard<std::mutex> deadlineLock(
                m_pendingCleanupDeadlineMutex);
            m_pendingCleanupDeadlines.erase(pendingKey);
            m_pendingCleanupExpiryUnixMs.erase(pendingKey);
            m_authoritativePendingCleanupDeadlines.erase(pendingKey);
        }
        NDN_LOG_INFO("Expired pending provider request/token state for "
                     << pendingKey.toUri());
        return true;
    }

    void ServiceProvider::publishHybridMessage(const ndn::Name& messageName,
                                               const ndn::Name&,
                                               AbstractMessage& message)
    {
        const auto plaintextBlock = message.WireEncode();
        auto plaintext = ndn::Buffer(plaintextBlock.begin(), plaintextBlock.end());
        boost::asio::post(m_face.getIoContext(),
            [this, messageName, plaintext = std::move(plaintext)]() mutable {
                publishHybridEncodedMessage(messageName, std::move(plaintext));
            });
    }

    void ServiceProvider::publishHybridEncodedMessage(const ndn::Name& messageName,
                                                      ndn::Buffer plaintext)
    {
        ndn::Name serviceName;
        ndn::Name requestId;
        ndn::Name senderPrefix = identity;
        if (auto ack = parseRequestAckNameV2(messageName)) {
            serviceName = ack->serviceName;
            requestId = ack->requestId;
        }
        else if (auto response = parseResponseNameV2(messageName)) {
            serviceName = response->serviceName;
            requestId = response->requestId;
        }
        else if (auto event = parseInvocationEventName(messageName)) {
            serviceName = event->serviceName;
            requestId = event->requestId;
        }
        else {
            NDN_LOG_ERROR("Hybrid publish unsupported message name: " << messageName);
            return;
        }

        const auto messageType = hybridMessageTypeForName(messageName);
        const auto accessAttribute = hybridAccessAttributeForName(messageName, serviceName);
        auto key = m_hybridMessageCrypto.getOrCreateSendKey(
            serviceName, senderPrefix, accessAttribute, messageType, m_hybridCryptoCounters);

        const auto ad = hybridAssociatedData(messageName, messageType, requestId,
                                            serviceName, senderPrefix,
                                            key.keyId, key.epochId);
        HybridMessageEnvelope envelope;
        envelope.setKeyId(key.keyId);
        envelope.setEpochId(key.epochId);
        envelope.setMessageType(messageType);
        if (m_timelineTrace) {
            logTimelineTrace("provider", "aes_gcm_encrypt_start", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"messageType", messageType}});
            logTimelineTrace("provider", cryptoStageForName(messageName) + "_crypto_start",
                             requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"messageName", messageName.toUri()},
                              {"mode", "hybrid"}});
        }

        // Publish the NAC-ABE wrapped MessageKey once under its deterministic
        // epoch name.  The first packet in a new epoch also carries that
        // wrapped key inline so a cold receiver can decrypt without racing the
        // named-key Interest.  Subsequent ACK/Response packets carry only
        // compact key/epoch identifiers and recover the key by name.
        ndn::Buffer cachedWrappedKey;
        const bool wrappedKeyKnown =
            m_hybridMessageCrypto.getWrappedSendKey(key.keyId, cachedWrappedKey);
        if (!wrappedKeyKnown && m_hybridMessageCrypto.shouldAttachWrappedKey(key.keyId)) {
            if (m_timelineTrace) {
                logTimelineTrace("provider", "wrapped_key_published", requestId,
                                 {{"value", "true"},
                                  {"serviceName", serviceName.toUri()},
                                  {"messageType", messageType}});
                logTimelineTrace("provider", "hybrid_key_wrap_start", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"messageType", messageType}});
            }
            const auto wrapStartUs = timelineSteadyMicroseconds();
            ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
            std::tie(contentData, ckData) =
                activeNacProducer().produce(key.keyName,
                                    std::vector<std::string>{accessAttribute},
                                    ndn::span<const uint8_t>(key.key.data(), key.key.size()),
                                    m_signingInfo);
            auto wrapped = mergeDataContents(contentData);
            if (wrapped.empty()) {
                NDN_LOG_ERROR("Hybrid MessageKey wrap produced empty content for "
                              << messageName.toUri());
                return;
            }
            envelope.setWrappedMessageKey(
                ndn::Buffer(wrapped.data(), wrapped.size()));
            serveDataWithIMS(contentData, ckData);
            m_hybridMessageCrypto.cacheWrappedSendKey(
                serviceName, key.keyId,
                ndn::Buffer(wrapped.data(), wrapped.size()));
            ++m_hybridCryptoCounters.nac_abe_key_wrap_count;
            const auto wrapEndUs = timelineSteadyMicroseconds();
            if (m_timelineTrace) {
                logTimelineTrace("provider", "hybrid_key_wrap_done", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"messageType", messageType},
                                  {"duration_us", std::to_string(wrapEndUs >= wrapStartUs ?
                                                                 wrapEndUs - wrapStartUs : 0)}});
            }
        }
        else if (m_timelineTrace) {
            logTimelineTrace("provider", "wrapped_key_published", requestId,
                             {{"value", "false"},
                              {"source", "epoch-cache"},
                              {"serviceName", serviceName.toUri()},
                              {"messageType", messageType}});
        }

        auto encryptAndPost = [this, messageName, requestId, serviceName, messageType,
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
            boost::asio::post(m_face.getIoContext(),
                [this, messageName, requestId, serviceName, messageType,
                 keyId, epochId, aesStartUs, aesEndUs, wrappedKeyAttached,
                 ciphertextBytes, error = std::move(error),
                 buffer = std::move(buffer)]() mutable {
                if (!error.empty()) {
                    NDN_LOG_ERROR("[NDNSF_HYBRID] role=provider event=HYBRID_PUBLISH_FAILED"
                                  << " messageName=" << messageName.toUri()
                                  << " reason=" << error);
                    return;
                }
                if (m_timelineTrace) {
                    logTimelineTrace("provider", "aes_gcm_encrypt_done", requestId,
                                     {{"serviceName", serviceName.toUri()},
                                      {"messageType", messageType},
                                      {"duration_us", std::to_string(aesEndUs >= aesStartUs ?
                                                                     aesEndUs - aesStartUs : 0)}});
                    logTimelineTrace("provider", cryptoStageForName(messageName) + "_crypto_done",
                                     requestId,
                                     {{"serviceName", serviceName.toUri()},
                                      {"messageName", messageName.toUri()},
                                      {"mode", "hybrid"}});
                }
                ++m_hybridCryptoCounters.symmetric_encrypt_count;
                if (m_useTokens) {
                    if (messageType == "ACK") {
                        ++m_hybridCryptoCounters.provider_token_symmetric_encrypt_count;
                        ++m_hybridCryptoCounters.user_token_symmetric_encrypt_count;
                    }
                    if (messageType == "RESPONSE") {
                        ++m_hybridCryptoCounters.user_token_symmetric_encrypt_count;
                    }
                }
                const auto queuedAtUs = nowMicroseconds();
                NDN_LOG_DEBUG("[NDNSF_HYBRID] role=provider event=HYBRID_PUBLISH"
                              << " messageName=" << messageName.toUri()
                              << " messageType=" << messageType
                              << " keyId=" << keyId
                              << " epochId=" << epochId
                              << " wrappedKeyAttached=" << wrappedKeyAttached
                              << " ciphertextBytes=" << ciphertextBytes);
                ndn::Block contentBlock(buffer);
                const auto beginUs = nowMicroseconds();
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SVS_PUBLISH_BEGIN timestamp_us="
                          << beginUs
                          << " providerName=" << identity.toUri()
                          << " messageName=" << messageName.toUri()
                          << " contentBytes=" << contentBlock.value_size()
                          << " eventLoopLagUs=" << (beginUs >= queuedAtUs ? beginUs - queuedAtUs : 0)
                          << " mode=hybrid-message-crypto");
                logControlTiming("provider", "SVS_PUBLISH_BEGIN", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"providerName", identity.toUri()},
                                  {"messageType", messageType},
                                  {"messageName", messageName.toUri()},
                                  {"contentBytes", std::to_string(contentBlock.value_size())},
                                  {"eventLoopLagUs", std::to_string(beginUs >= queuedAtUs ?
                                                                    beginUs - queuedAtUs : 0)},
                                  {"mode", "hybrid-message-crypto"}});
                if (m_timelineTrace) {
                    ndn::Name rid;
                    ndn::Name svc;
                    if (auto ack = parseRequestAckNameV2(messageName)) {
                        rid = ack->requestId;
                        svc = ack->serviceName;
                    }
                    else if (auto response = parseResponseNameV2(messageName)) {
                        rid = response->requestId;
                        svc = response->serviceName;
                    }
                    else if (auto event = parseInvocationEventName(messageName)) {
                        rid = event->requestId;
                        svc = event->serviceName;
                    }
                    if (!rid.empty()) {
                        logTimelineTrace("provider", cryptoStageForName(messageName) + "_publish_start",
                                         rid,
                                         {{"serviceName", svc.toUri()},
                                          {"messageName", messageName.toUri()},
                                          {"mode", "hybrid"}});
                    }
                }
                const bool asyncPublish = useAsyncSvsPublish();
                const auto publishedSeqNo = publishSvs(m_svsps, messageName, contentBlock);
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event="
                              << (asyncPublish ? "SVS_PUBLISH_ACCEPTED" : "SVS_PUBLISH_DONE")
                              << " timestamp_us=" << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " providerName=" << identity.toUri()
                              << " messageName=" << messageName.toUri()
                              << " seqNo=" << publishedSeqNo
                              << " mode=hybrid-message-crypto");
                logControlTiming("provider", "SVS_PUBLISH_DONE", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"providerName", identity.toUri()},
                                  {"messageType", messageType},
                                  {"messageName", messageName.toUri()},
                                  {"contentBytes", std::to_string(contentBlock.value_size())},
                                  {"mode", "hybrid-message-crypto"}});
                if (m_timelineTrace) {
                    ndn::Name rid;
                    ndn::Name svc;
                    if (auto ack = parseRequestAckNameV2(messageName)) {
                        rid = ack->requestId;
                        svc = ack->serviceName;
                    }
                    else if (auto response = parseResponseNameV2(messageName)) {
                        rid = response->requestId;
                        svc = response->serviceName;
                    }
                    else if (auto event = parseInvocationEventName(messageName)) {
                        rid = event->requestId;
                        svc = event->serviceName;
                    }
                    if (!rid.empty()) {
                        logTimelineTrace("provider", cryptoStageForName(messageName) + "_publish_done",
                                         rid,
                                         {{"serviceName", svc.toUri()},
                                          {"messageName", messageName.toUri()},
                                          {"mode", "hybrid"}});
                    }
                }
            });
        };
        // Response/ACK publication can be called by a handler that is waiting
        // for the peer's control messages.  Queueing encryption on the same
        // handler pool would deadlock a single-worker provider until that
        // wait expires. Keep the normal bounded envelope inline so the
        // request can make progress; only an oversized response uses the
        // independent fetch pool to avoid blocking the Face event loop.
        const auto inlineLimit = responseLargeDataThresholdBytes();
        if (inlineLimit > 0 && plaintext.size() > inlineLimit &&
            m_fetchPool.getThreadCount() != 0 &&
            m_fetchPool.post(encryptAndPost)) {
            return;
        }
        encryptAndPost();
    }

    bool ServiceProvider::decryptHybridMessage(const ndn::Name& messageName,
                                               const ndn::Block& envelopeBlock,
                                               std::function<void(const ndn::Buffer&)> onSuccess,
                                               std::function<void(const std::string&)> onError)
    {
        const auto decryptEntryUs = timelineSteadyMicroseconds();
        HybridMessageEnvelope envelope;
        if (!envelope.WireDecode(envelopeBlock)) {
            return false;
        }

        ndn::Name serviceName;
        ndn::Name requestId;
        ndn::Name senderPrefix;
        if (auto request = parseRequestNameV2(messageName)) {
            serviceName = request->serviceName;
            requestId = request->requestId;
            senderPrefix = request->requesterName;
        }
        else if (auto selection = parseCompactServiceSelectionNameV2(messageName)) {
            serviceName = selection->serviceName;
            requestId = selection->requestId;
            senderPrefix = selection->requesterName;
        }
        else if (auto selection = parseServiceSelectionNameV2(messageName)) {
            serviceName = selection->serviceName;
            requestId = selection->requestId;
            senderPrefix = selection->requesterName;
        }
        else {
            return false;
        }

        const auto accessAttribute = hybridAccessAttributeForName(messageName, serviceName);
        const auto keyDataName = makeHybridMessageKeyDataName(
            serviceName, senderPrefix, accessAttribute, envelope.getEpochId());

        auto finish = [this, envelope, messageName, serviceName, requestId,
                       senderPrefix, decryptEntryUs, onSuccess = std::move(onSuccess),
                       onError](const ndn::Buffer& key) mutable {
            const auto keyReadyUs = timelineSteadyMicroseconds();
            const auto ad = hybridAssociatedData(messageName, envelope.getMessageType(),
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
                logHybridCryptoTiming("provider", "hybrid_decrypt_aes_done", requestId,
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
                    logHybridCryptoTiming("provider", "hybrid_decrypt_callback_dispatch", requestId,
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
                        if (envelope.getMessageType() == "REQUEST") {
                            ++m_hybridCryptoCounters.user_token_symmetric_decrypt_count;
                        }
                        if (envelope.getMessageType() == "SELECTION") {
                            ++m_hybridCryptoCounters.provider_token_symmetric_decrypt_count;
                        }
                    }
                    if (onSuccess) {
                        onSuccess(plaintext);
                    }
                });
            };
            if (m_handlerPool.getThreadCount() == 0 ||
                !m_handlerPool.post(decryptAndPost)) {
                decryptAndPost();
            }
        };

        ndn::Buffer key;
        if (m_hybridMessageCrypto.findReceiveKey(envelope.getKeyId(), key,
                                                 m_hybridCryptoCounters)) {
            logHybridCryptoTiming("provider", "hybrid_decrypt_key_cache", requestId,
                                  {{"messageType", envelope.getMessageType()},
                                   {"hit", "true"},
                                   {"entryToCacheLookupUs",
                                    std::to_string(timelineSteadyMicroseconds() - decryptEntryUs)}});
            finish(key);
            return true;
        }
        logHybridCryptoTiming("provider", "hybrid_decrypt_key_cache", requestId,
                              {{"messageType", envelope.getMessageType()},
                               {"hit", "false"},
                               {"wrappedKeyAttached",
                                envelope.hasWrappedMessageKey() ? "true" : "false"},
                               {"entryToCacheLookupUs",
                                std::to_string(timelineSteadyMicroseconds() - decryptEntryUs)}});
        ++m_hybridCryptoCounters.nac_abe_key_unwrap_count;
        const auto unwrapStartUs = timelineSteadyMicroseconds();
        logHybridCryptoTiming("provider", "hybrid_decrypt_key_unwrap_start", requestId,
                              {{"messageType", envelope.getMessageType()},
                               {"source", envelope.hasWrappedMessageKey() ?
                                          "inline" : "named-fetch"},
                               {"keyName", keyDataName.toUri()}});
        try {
            auto onKey = [this, serviceName, envelope, finish = std::move(finish), requestId,
                          unwrapStartUs, keyDataName](const ndn::Buffer& unwrappedKey) mutable {
                                    logHybridCryptoTiming("provider", "hybrid_decrypt_key_unwrap_done", requestId,
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
            // Keep unwrap and synchronous-exception reporting independent of
            // the success closure's ownership of its callback copy.
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
                activeNacConsumer().consume(
                    keyDataName, std::move(onKey), std::move(onKeyError));
            }
        }
        catch (const std::exception& e) {
            if (onError) {
                onError("hybrid MessageKey unwrap failed: " + std::string(e.what()));
            }
        }
        return true;
    }

    void ServiceProvider::schedulePendingRequestCleanup(
        const ndn::Name& pendingKey,
        ndn::time::milliseconds ttl,
        bool authoritative)
    {
        const auto total = ttl + m_pendingRequestTimeoutGrace;
        bool scheduleTimer = false;
        {
            std::lock_guard<std::mutex> lock(m_pendingCleanupDeadlineMutex);
            const auto existing = m_pendingCleanupDeadlines.find(pendingKey);
            if (existing == m_pendingCleanupDeadlines.end()) {
                scheduleTimer = true;
            }
            else if (!authoritative ||
                     m_authoritativePendingCleanupDeadlines.find(pendingKey) !=
                         m_authoritativePendingCleanupDeadlines.end()) {
                // Duplicate Request traffic cannot extend the provisional
                // horizon, and duplicate ACKs cannot extend the first
                // Provider-authorized horizon.
                return;
            }
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(total.count());
            const auto expiry = nowMilliseconds() +
                static_cast<uint64_t>(std::max<int64_t>(1, total.count()));
            m_pendingCleanupDeadlines[pendingKey] = deadline;
            m_pendingCleanupExpiryUnixMs[pendingKey] = expiry;
            if (authoritative) {
                m_authoritativePendingCleanupDeadlines.insert(pendingKey);
            }
        }
        if (!scheduleTimer) {
            return;
        }
        m_scheduler.schedule(
            std::max(total, ndn::time::milliseconds(1)),
            [this, pendingKey] {
                ndn::time::milliseconds remaining{0};
                {
                    std::lock_guard<std::mutex> lock(
                        m_pendingCleanupDeadlineMutex);
                    const auto found =
                        m_pendingCleanupDeadlines.find(pendingKey);
                    if (found == m_pendingCleanupDeadlines.end()) {
                        return;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (found->second > now) {
                        const auto remainingStd =
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                    found->second - now);
                        remaining =
                            ndn::time::milliseconds(remainingStd.count());
                    }
                }
                if (remaining.count() > 0) {
                    m_scheduler.schedule(
                        std::max(remaining, ndn::time::milliseconds(1)),
                        [this, pendingKey] {
                            expirePendingRequestState(pendingKey);
                        });
                    return;
                }
                expirePendingRequestState(pendingKey);
            });
    }

    void ServiceProvider::PublishMessage(const ndn::Name &messageName, const ndn::Name &messageNameWithoutPrefix,AbstractMessage &message)
    {
        // log message
        NDN_LOG_DEBUG("PublishMessage: " << messageName.toUri());

        if (m_svsps == nullptr && m_localPublicationHandler) {
            const auto wireBlock = message.WireEncode();
            const ndn::Buffer wire(wireBlock.data(), wireBlock.size());
            m_localPublicationHandler(messageName, wire);
            return;
        }

        auto results = ndn_service_framework::GetAttributesByName(messageName);
        if (!results)
        {
            NDN_LOG_ERROR("GetAttributesByName failed: " << messageName);
            return;
        }
        NDN_LOG_DEBUG("GetAttributesByName: messageName=" << messageName.toUri()
                     << " attributes=" << formatAttributesForLog(*results));
        publishHybridMessage(messageName, messageNameWithoutPrefix, message);
        return;
        const auto stage = cryptoStageForName(messageName);
        ndn::Name timelineRequestId;
        ndn::Name timelineServiceName;
        if (auto ack = parseRequestAckNameV2(messageName)) {
            timelineRequestId = ack->requestId;
            timelineServiceName = ack->serviceName;
        }
        else if (auto response = parseResponseNameV2(messageName)) {
            timelineRequestId = response->requestId;
            timelineServiceName = response->serviceName;
        }
        const auto plaintextBlock = message.WireEncode();
        const bool usePlaintext =
            (stage == "ack" && plaintextAckDiagEnabled()) ||
            (stage == "response" && plaintextResponseDiagEnabled());
        const auto encryptStartUs = nowMicroseconds();
        if (m_timelineTrace && !timelineRequestId.empty()) {
            logTimelineTrace("provider", stage + "_crypto_start", timelineRequestId,
                             {{"serviceName", timelineServiceName.toUri()},
                              {"messageName", messageName.toUri()}});
        }
        if (usePlaintext) {
            const auto encryptEndUs = nowMicroseconds();
            if (m_timelineTrace && !timelineRequestId.empty()) {
                logTimelineTrace("provider", stage + "_crypto_done", timelineRequestId,
                                 {{"serviceName", timelineServiceName.toUri()},
                                  {"messageName", messageName.toUri()},
                                  {"duration_us",
                                   std::to_string(encryptEndUs >= encryptStartUs ?
                                                  encryptEndUs - encryptStartUs : 0)}});
            }
            logCryptoDiag("provider", stage, "encrypt", "plaintext", "success",
                          encryptStartUs, encryptEndUs, messageName,
                          plaintextBlock.size());

            auto buffer = ndn::Buffer(plaintextBlock.begin(), plaintextBlock.end());
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SVS_PUBLISH_QUEUED timestamp_us="
                      << nowMicroseconds()
                      << " providerName=" << identity.toUri()
                      << " messageName=" << messageName.toUri()
                      << " contentBytes=" << buffer.size()
                      << " contentSegments=0"
                      << " ckSegments=0");
            const auto queuedAtUs = nowMicroseconds();
            boost::asio::post(m_face.getIoContext(),
                [this, messageName, queuedAtUs, buffer = std::move(buffer)]() mutable {
                    ndn::Block contentBlock(buffer);
                    const auto beginUs = nowMicroseconds();
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SVS_PUBLISH_BEGIN timestamp_us="
                              << beginUs
                              << " providerName=" << identity.toUri()
                              << " messageName=" << messageName.toUri()
                              << " contentBytes=" << contentBlock.value_size()
                              << " eventLoopLagUs=" << (beginUs >= queuedAtUs ?
                                                         beginUs - queuedAtUs : 0));
                    if (m_timelineTrace) {
                        ndn::Name rid;
                        ndn::Name svc;
                        if (auto ack = parseRequestAckNameV2(messageName)) {
                            rid = ack->requestId;
                            svc = ack->serviceName;
                        }
                        else if (auto response = parseResponseNameV2(messageName)) {
                            rid = response->requestId;
                            svc = response->serviceName;
                        }
                        if (!rid.empty()) {
                            logTimelineTrace("provider", cryptoStageForName(messageName) + "_publish_start",
                                             rid,
                                             {{"serviceName", svc.toUri()},
                                              {"messageName", messageName.toUri()}});
                        }
                    }
                    publishSvs(m_svsps, messageName, contentBlock);
                    if (m_timelineTrace) {
                        ndn::Name rid;
                        ndn::Name svc;
                        if (auto ack = parseRequestAckNameV2(messageName)) {
                            rid = ack->requestId;
                            svc = ack->serviceName;
                        }
                        else if (auto response = parseResponseNameV2(messageName)) {
                            rid = response->requestId;
                            svc = response->serviceName;
                        }
                        if (!rid.empty()) {
                            logTimelineTrace("provider", cryptoStageForName(messageName) + "_publish_done",
                                             rid,
                                             {{"serviceName", svc.toUri()},
                                              {"messageName", messageName.toUri()}});
                        }
                    }
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SVS_PUBLISH_DONE timestamp_us="
                              << nowMicroseconds()
                              << " providerName=" << identity.toUri()
                              << " messageName=" << messageName.toUri());
                    NDN_LOG_TRACE("Message Published: " << messageName.toUri()
                                 << " " << contentBlock.value_size());
                });
            return;
        }

        std::vector<uint8_t> plaintext(plaintextBlock.begin(), plaintextBlock.end());
        const bool isAck = stage == "ack";
        if (isAck) {
            ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PRODUCE_STARTED timestamp_us="
                      << nowMicroseconds()
                      << " providerName=" << identity.toUri()
                      << " messageName=" << messageName.toUri()
                      << " stage=" << stage
                      << " mode=synchronous-ack"
                      << " plaintextBytes=" << plaintext.size());
            try {
                std::tie(contentData, ckData) =
                    activeNacProducer().produce(
                        messageNameWithoutPrefix,
                        *results,
                        ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                        m_signingInfo);
                const auto encryptEndUs = nowMicroseconds();
                if (m_timelineTrace && !timelineRequestId.empty()) {
                    logTimelineTrace("provider", stage + "_crypto_done", timelineRequestId,
                                     {{"serviceName", timelineServiceName.toUri()},
                                      {"messageName", messageName.toUri()},
                                      {"duration_us",
                                       std::to_string(encryptEndUs >= encryptStartUs ?
                                                      encryptEndUs - encryptStartUs : 0)}});
                }
                logCryptoDiag("provider", stage, "encrypt",
                              "synchronous-ack", "success",
                              encryptStartUs, encryptEndUs,
                              messageName, plaintext.size());
            }
            catch (const std::exception& e) {
                const auto encryptEndUs = nowMicroseconds();
                logCryptoDiag("provider", stage, "encrypt",
                              "synchronous-ack", "failure",
                              encryptStartUs, encryptEndUs,
                              messageName, plaintext.size(), e.what());
                NDN_LOG_ERROR("NAC-ABE produce failed for "
                              << messageName.toUri() << ": " << e.what());
                return;
            }

            auto buffer = mergeDataContents(contentData);
            if (buffer.empty()) {
                NDN_LOG_ERROR("NAC-ABE produce returned empty content for "
                              << messageName.toUri());
                return;
            }
            const auto queuedAtUs = nowMicroseconds();
            serveDataWithIMS(contentData, ckData);
            ndn::Block contentBlock(buffer);
            const auto beginUs = nowMicroseconds();
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SVS_PUBLISH_BEGIN timestamp_us="
                      << beginUs
                      << " providerName=" << identity.toUri()
                      << " messageName=" << messageName.toUri()
                      << " contentBytes=" << contentBlock.value_size()
                      << " eventLoopLagUs=" << (beginUs >= queuedAtUs ?
                                                 beginUs - queuedAtUs : 0)
                      << " mode=synchronous-ack");
            if (m_timelineTrace && !timelineRequestId.empty()) {
                logTimelineTrace("provider", stage + "_publish_start", timelineRequestId,
                                 {{"serviceName", timelineServiceName.toUri()},
                                  {"messageName", messageName.toUri()}});
            }
            publishSvs(m_svsps, messageName, contentBlock);
            if (m_timelineTrace && !timelineRequestId.empty()) {
                logTimelineTrace("provider", stage + "_publish_done", timelineRequestId,
                                 {{"serviceName", timelineServiceName.toUri()},
                                  {"messageName", messageName.toUri()}});
            }
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SVS_PUBLISH_DONE timestamp_us="
                      << nowMicroseconds()
                      << " providerName=" << identity.toUri()
                      << " messageName=" << messageName.toUri()
                      << " mode=synchronous-ack");
            NDN_LOG_TRACE("Message Published: " << messageName.toUri()
                         << " " << contentBlock.value_size());
            return;
        }

        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PRODUCE_QUEUED timestamp_us="
                  << nowMicroseconds()
                  << " providerName=" << identity.toUri()
                  << " messageName=" << messageName.toUri()
                  << " stage=" << stage
                  << " plaintextBytes=" << plaintext.size());
        if (!m_cryptoProduceQueue.post(
                [this,
                 messageName,
                 messageNameWithoutPrefix,
                 attributes = *results,
                 stage,
                 encryptStartUs,
                 plaintext = std::move(plaintext)]() mutable {
                    ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PRODUCE_STARTED timestamp_us="
                              << nowMicroseconds()
                              << " providerName=" << identity.toUri()
                              << " messageName=" << messageName.toUri()
                              << " stage=" << stage
                              << " mode=serialized-worker"
                              << " plaintextBytes=" << plaintext.size());
                    try {
                        std::tie(contentData, ckData) =
                            activeNacProducer().produce(
                                messageNameWithoutPrefix,
                                attributes,
                                ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                                m_signingInfo);
                        const auto encryptEndUs = nowMicroseconds();
                        if (m_timelineTrace) {
                            ndn::Name rid;
                            ndn::Name svc;
                            if (auto ack = parseRequestAckNameV2(messageName)) {
                                rid = ack->requestId;
                                svc = ack->serviceName;
                            }
                            else if (auto response = parseResponseNameV2(messageName)) {
                                rid = response->requestId;
                                svc = response->serviceName;
                            }
                            if (!rid.empty()) {
                                logTimelineTrace("provider", stage + "_crypto_done", rid,
                                                 {{"serviceName", svc.toUri()},
                                                  {"messageName", messageName.toUri()},
                                                  {"duration_us",
                                                   std::to_string(encryptEndUs >= encryptStartUs ?
                                                                  encryptEndUs - encryptStartUs : 0)}});
                            }
                        }
                        logCryptoDiag("provider", stage, "encrypt",
                                      "serialized-worker", "success",
                                      encryptStartUs, encryptEndUs,
                                      messageName, plaintext.size());
                        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PRODUCE_COMPLETED timestamp_us="
                                  << encryptEndUs
                                  << " providerName=" << identity.toUri()
                                  << " messageName=" << messageName.toUri()
                                  << " stage=" << stage
                                  << " mode=serialized-worker"
                                  << " contentSegments=" << contentData.size()
                                  << " ckSegments=" << ckData.size());
                    }
                    catch (const std::exception& e) {
                        const auto encryptEndUs = nowMicroseconds();
                        logCryptoDiag("provider", stage, "encrypt",
                                      "serialized-worker", "failure",
                                      encryptStartUs, encryptEndUs,
                                      messageName, plaintext.size(), e.what());
                        NDN_LOG_ERROR("NAC-ABE produce failed for "
                                      << messageName.toUri() << ": " << e.what());
                        return;
                    }

                    auto buffer = mergeDataContents(contentData);
                    if (buffer.empty()) {
                        NDN_LOG_ERROR("NAC-ABE produce returned empty content for "
                                      << messageName.toUri());
                        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PRODUCE_EMPTY_CONTENT timestamp_us="
                                  << nowMicroseconds()
                                  << " providerName=" << identity.toUri()
                                  << " messageName=" << messageName.toUri()
                                  << " stage=" << stage
                                  << " mode=serialized-worker"
                                  << " contentSegments=" << contentData.size()
                                  << " ckSegments=" << ckData.size());
                        return;
                    }
                    const auto queuedAtUs = nowMicroseconds();
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SVS_PUBLISH_QUEUED timestamp_us="
                              << queuedAtUs
                              << " providerName=" << identity.toUri()
                              << " messageName=" << messageName.toUri()
                              << " contentBytes=" << buffer.size()
                              << " contentSegments=" << contentData.size()
                              << " ckSegments=" << ckData.size());
                    boost::asio::post(m_face.getIoContext(),
                        [this,
                         messageName,
                         queuedAtUs,
                         buffer = std::move(buffer),
                         contentData = std::move(contentData),
                         ckData = std::move(ckData)]() mutable {
                            serveDataWithIMS(contentData, ckData);
                            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=IMS_INSERT_DONE timestamp_us="
                                      << nowMicroseconds()
                                      << " providerName=" << identity.toUri()
                                      << " messageName=" << messageName.toUri()
                                      << " contentSegments=" << contentData.size()
                                      << " ckSegments=" << ckData.size());
                            ndn::Block contentBlock(buffer);
                            const auto beginUs = nowMicroseconds();
                            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SVS_PUBLISH_BEGIN timestamp_us="
                                      << beginUs
                                      << " providerName=" << identity.toUri()
                                      << " messageName=" << messageName.toUri()
                                      << " contentBytes=" << contentBlock.value_size()
                                      << " eventLoopLagUs=" << (beginUs >= queuedAtUs ?
                                                                 beginUs - queuedAtUs : 0));
                            if (m_timelineTrace) {
                                ndn::Name requestId;
                                ndn::Name serviceName;
                                if (auto ack = parseRequestAckNameV2(messageName)) {
                                    requestId = ack->requestId;
                                    serviceName = ack->serviceName;
                                }
                                else if (auto response = parseResponseNameV2(messageName)) {
                                    requestId = response->requestId;
                                    serviceName = response->serviceName;
                                }
                                if (!requestId.empty()) {
                                    logTimelineTrace("provider", cryptoStageForName(messageName) + "_publish_start",
                                                     requestId,
                                                     {{"serviceName", serviceName.toUri()},
                                                      {"messageName", messageName.toUri()}});
                                }
                            }
                            publishSvs(m_svsps, messageName, contentBlock);
                            if (m_timelineTrace) {
                                ndn::Name requestId;
                                ndn::Name serviceName;
                                if (auto ack = parseRequestAckNameV2(messageName)) {
                                    requestId = ack->requestId;
                                    serviceName = ack->serviceName;
                                }
                                else if (auto response = parseResponseNameV2(messageName)) {
                                    requestId = response->requestId;
                                    serviceName = response->serviceName;
                                }
                                if (!requestId.empty()) {
                                    logTimelineTrace("provider", cryptoStageForName(messageName) + "_publish_done",
                                                     requestId,
                                                     {{"serviceName", serviceName.toUri()},
                                                      {"messageName", messageName.toUri()}});
                                }
                            }
                            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SVS_PUBLISH_DONE timestamp_us="
                                      << nowMicroseconds()
                                      << " providerName=" << identity.toUri()
                                      << " messageName=" << messageName.toUri());
                            NDN_LOG_TRACE("Message Published: " << messageName.toUri()
                                         << " " << contentBlock.value_size());
                        });
                })) {
            NDN_LOG_ERROR("NAC-ABE produce queue is full; dropping publish for "
                          << messageName.toUri());
        }

    }

    void ServiceProvider::onMissingData(const std::vector<ndn::svs::MissingDataInfo>& infoVector)
    {
        // Keep this opt-in and bounded: a missing Sync range can contain many
        // publications, and routine qualification must not turn it into a
        // log-amplification path.  The diagnostic is useful for distinguishing
        // a publication-fetch gap from a subscription/freshness rejection.
        if (std::getenv("NDNSF_SVS_DIAGNOSTIC") == nullptr) {
            return;
        }
        NDN_LOG_WARN("NDNSF_SVS_MISSING_DATA provider=" << identity.toUri()
                     << " count=" << infoVector.size());
        const size_t limit = std::min<size_t>(infoVector.size(), 16);
        for (size_t i = 0; i < limit; ++i) {
            const auto& info = infoVector[i];
            NDN_LOG_WARN("NDNSF_SVS_MISSING_RANGE provider=" << identity.toUri()
                         << " node=" << info.nodeId.toUri()
                         << " low=" << info.low << " high=" << info.high);
        }
    }

    void ServiceProvider::updateNdnsdMeta(const std::string& key, const std::string& value)
    {
        std::lock_guard<std::mutex> lock(m_ndnsdMetaMutex);
        m_ndnsdMeta[key] = value;
    }

    void ServiceProvider::setNdnsdMeta(const std::map<std::string, std::string>& meta)
    {
        std::lock_guard<std::mutex> lock(m_ndnsdMetaMutex);
        m_ndnsdMeta = meta;
    }

    void ServiceProvider::startNdnsdPeriodicPublish(int intervalSeconds)
    {
        if (!m_ServiceDiscovery.isEnabled()) {
            NDN_LOG_INFO("[ServiceProvider] NDNSD disabled; skip periodic publish");
            return;
        }
        if (intervalSeconds <= 0 || m_serviceNames.empty()) {
            return;
        }
        NDN_LOG_INFO("[ServiceProvider] NDNSD periodic publish started"
                     << " interval=" << intervalSeconds << "s"
                     << " services=" << m_serviceNames.size());
        m_ndnsdHeartbeatIntervalSeconds = intervalSeconds;
        m_ndnsdScheduler = std::make_unique<ndn::Scheduler>(m_face.getIoContext());

        std::function<void()> heartbeat = [this] {
            std::map<std::string, std::string> meta;
            {
                std::lock_guard<std::mutex> lock(m_ndnsdMetaMutex);
                meta = m_ndnsdMeta;
            }
            meta["publishSource"] = "ndnsf-core-heartbeat";
            int lifetime = m_ndnsdHeartbeatIntervalSeconds * 2;
            for (const auto& serviceUri : m_serviceNames) {
                publishServiceInfo(ndn::Name(serviceUri), lifetime, meta);
            }
        };
        // Store callback for re-scheduling; schedule initial tick
        auto recurring = std::make_shared<std::function<void()>>(std::move(heartbeat));
        *recurring = [this, intervalSeconds, recurring] {
            (*recurring)();  // publish
            m_ndnsdHeartbeatEvent = m_ndnsdScheduler->schedule(
                ndn::time::seconds(intervalSeconds), *recurring);
        };
        m_ndnsdHeartbeatEvent = m_ndnsdScheduler->schedule(
            ndn::time::seconds(intervalSeconds), *recurring);
    }

    void ServiceProvider::stopNdnsdPeriodicPublish()
    {
        if (m_ndnsdScheduler == nullptr) {
            return;
        }
        m_ndnsdHeartbeatEvent = {};
        m_ndnsdScheduler->cancelAllEvents();
        m_ndnsdScheduler.reset();
        m_ndnsdHeartbeatIntervalSeconds = 0;
        NDN_LOG_INFO("[ServiceProvider] NDNSD periodic publish stopped");
    }

    void ServiceProvider::OnRequest(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        const bool svsDiagnostic =
            std::getenv("NDNSF_SVS_DIAGNOSTIC") != nullptr;
        if (svsDiagnostic) {
            NDN_LOG_WARN("NDNSF_SVS_REQUEST_SEEN provider=" << identity.toUri()
                         << " name=" << subscription.name.toUri()
                         << " producer=" << subscription.producerPrefix.toUri()
                         << " seq=" << subscription.seqNo
                         << " bytes=" << subscription.data.size());
        }
        if(!isFresh(subscription)) {
            if (svsDiagnostic) {
                NDN_LOG_WARN("NDNSF_SVS_REQUEST_REJECTED provider="
                             << identity.toUri() << " reason=stale_or_duplicate"
                             << " name=" << subscription.name.toUri()
                             << " producer=" << subscription.producerPrefix.toUri()
                             << " seq=" << subscription.seqNo);
            }
            return;
        }
        NDN_LOG_DEBUG("[ServiceProvider] OnRequest name="
                  << subscription.name.toUri()
                  << " producer=" << subscription.producerPrefix.toUri()
                  << " bytes=" << subscription.data.size());
        // log the request
        NDN_LOG_DEBUG("OnRequest: " << subscription.name << " " << subscription.data.size());

        auto requestV2 = ndn_service_framework::parseRequestNameV2(subscription.name);
        if (requestV2) {
            logValidatedPublicationAudit(
                "provider", "REQUEST", subscription,
                requestV2->requestId, requestV2->serviceName,
                requestV2->requesterName, identity);
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=REQUEST_RECEIVED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestV2->requestId.toUri()
                      << " serviceName=" << requestV2->serviceName.toUri()
                      << " requestName=" << subscription.name.toUri());
            logControlTiming("provider", "REQUEST_RECEIVED", requestV2->requestId,
                             {{"serviceName", requestV2->serviceName.toUri()},
                              {"providerName", identity.toUri()},
                              {"requesterName", requestV2->requesterName.toUri()},
                              {"requestName", subscription.name.toUri()},
                              {"contentBytes", std::to_string(subscription.data.size())}});
            if (m_timelineTrace) {
                logTimelineTrace("provider", "request_observed", requestV2->requestId,
                                 {{"serviceName", requestV2->serviceName.toUri()},
                                  {"requesterName", requestV2->requesterName.toUri()},
                                  {"requestName", subscription.name.toUri()}});
            }
            const ndn::Name fullServiceName =
                makePermissionFullServiceName(identity, requestV2->serviceName);
            if (!m_authorizations.contains(fullServiceName.toUri(),
                                           requestV2->serviceName.toUri(),
                                           tlv::ProviderPermission))
            {
                NDN_LOG_INFO("[ServiceProvider] OnRequest missing permission provider="
                          << identity.toUri()
                          << " service=" << requestV2->serviceName.toUri());
                NDN_LOG_ERROR("Not serving: " << requestV2->serviceName);
                return;
            }

            if(subscription.data.size() > 0){
                if (m_timelineTrace) {
                    logTimelineTrace("provider", "request_decrypt_start",
                                     requestV2->requestId,
                                     {{"serviceName", requestV2->serviceName.toUri()}});
                }
                if (decryptHybridMessage(subscription.name,
                                         ndn::Block(subscription.data),
                                         std::bind(&ServiceProvider::OnRequestDecryptionSuccessCallbackV2,
                                                   this,
                                                   requestV2->requesterName,
                                                   requestV2->serviceName,
                                                   requestV2->requestId,
                                                   _1),
                                         std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback,
                                                   this,
                                                   requestV2->requesterName,
                                                   requestV2->serviceName,
                                                   requestV2->requestId,
                                                   _1))) {
                    return;
                }
                OnRequestDecryptionErrorCallback(requestV2->requesterName,
                                                 requestV2->serviceName,
                                                 requestV2->requestId,
                                                 "invalid hybrid request envelope");
                return;
            }
            else{
                activeNacConsumer().consume(subscription.name,
                                    std::bind(&ServiceProvider::OnRequestDecryptionSuccessCallbackV2,
                                              this,
                                              requestV2->requesterName,
                                              requestV2->serviceName,
                                              requestV2->requestId,
                                              _1),
                                    std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback,
                                              this,
                                              requestV2->requesterName,
                                              requestV2->serviceName,
                                              requestV2->requestId,
                                              _1));

            }
            return;
        }

        NDN_LOG_WARN("Reject non-V2 request name: " << subscription.name);

    }

void ServiceProvider::OnRequestDecryptionSuccessCallbackV2(
    const ndn::Name& requesterIdentity,
    const ndn::Name& serviceName,
    const ndn::Name& requestId,
    const ndn::Buffer& buffer)
{
    auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());
    auto decodeAndFinish = [this, requesterIdentity, serviceName,
                            requestId, raw]() mutable {
        ndn_service_framework::RequestMessage requestMessage;
        try {
            ndn::Block block(ndn::span<const uint8_t>(raw->data(), raw->size()));
            if (!requestMessage.WireDecode(block)) {
                NDN_LOG_ERROR("OnRequestDecryptionSuccessCallbackV2: RequestMessage decode failed");
                return;
            }
        }
        catch (const std::exception& e) {
            NDN_LOG_ERROR("OnRequestDecryptionSuccessCallbackV2: RequestMessage decode failed: "
                          << e.what());
            return;
        }

        boost::asio::post(m_face.getIoContext(),
            [this, requesterIdentity, serviceName, requestId,
             raw,
             requestMessage = std::move(requestMessage)]() mutable {
                finishDecodedRequestOnEventLoop(requesterIdentity,
                                                serviceName,
                                                requestId,
                                                std::move(requestMessage));
            });
    };

    if (m_handlerPool.getThreadCount() != 0 &&
        m_handlerPool.post(std::move(decodeAndFinish))) {
        return;
    }

    try {
        ndn::Block block(buffer);
        ndn_service_framework::RequestMessage requestMessage;
        if (!requestMessage.WireDecode(block)) {
            NDN_LOG_ERROR("OnRequestDecryptionSuccessCallbackV2: RequestMessage decode failed");
            return;
        }
        finishDecodedRequestOnEventLoop(requesterIdentity,
                                        serviceName,
                                        requestId,
                                        std::move(requestMessage));
    }
    catch (const std::exception& e) {
        NDN_LOG_ERROR("OnRequestDecryptionSuccessCallbackV2: RequestMessage decode failed: "
                      << e.what());
    }
}

void ServiceProvider::finishDecodedRequestOnEventLoop(
    const ndn::Name& requesterIdentity,
    const ndn::Name& serviceName,
    const ndn::Name& requestId,
    ndn_service_framework::RequestMessage requestMessage)
{
    NDN_LOG_DEBUG("OnRequestDecryptionSuccessCallbackV2: "
        << requesterIdentity.toUri()
        << serviceName.toUri()
        << requestId.toUri());
    NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=REQUEST_DECRYPT_DONE timestamp_us="
              << nowMicroseconds()
              << " requestId=" << requestId.toUri()
              << " serviceName=" << serviceName.toUri()
              << " requesterName=" << requesterIdentity.toUri());
    if (m_timelineTrace) {
        logTimelineTrace("provider", "request_decrypt_done", requestId,
                         {{"serviceName", serviceName.toUri()},
                          {"requesterName", requesterIdentity.toUri()}});
        logTimelineTrace("provider", "user_token_validate_start", requestId,
                         {{"serviceName", serviceName.toUri()}});
    }
    updateProviderRequestLifecycleState(
        requestId, serviceName,
        ProviderRequestLifecycleState::REQUEST_OBSERVED);

    const auto requestVersion = requestMessage.hasControllerVersion() ?
        std::optional<ControllerVersion>(requestMessage.getControllerVersion()) :
        std::nullopt;
    maybeRefreshControllerVersionHint(serviceName, requestVersion);
    if (!authorizeControllerTransition(serviceName,
                                       ProtectedTransition::PROVIDER_EXECUTION)) {
        NDN_LOG_ERROR("Reject decoded request under revoked Controller status requestId="
                      << requestId.toUri());
        return;
    }

    if (!isAcceptablePolicyEpoch(serviceName, requestMessage.getPolicyEpoch())) {
        NDN_LOG_ERROR("Reject request with stale policy epoch requestId="
                      << requestId.toUri()
                      << " receivedEpoch=" << requestMessage.getPolicyEpoch()
                      << " currentEpoch=" << m_currentPolicyEpoch);
        return;
    }

    if (!isAcceptableControllerVersion(serviceName, requestVersion)) {
        NDN_LOG_ERROR("Reject request with stale ControllerVersion requestId="
                      << requestId.toUri());
        return;
    }

    if (!hasProviderPermission(identity, serviceName, m_authorizations)) {
        NDN_LOG_ERROR("Not Serving: " << serviceName);
        return;
    }

    if (m_useTokens && requestMessage.getUserToken().empty()) {
        NDN_LOG_ERROR("OnRequestDecryptionSuccessCallbackV2: Missing UserToken");
        return;
    }
    if (m_timelineTrace) {
        logTimelineTrace("provider", "user_token_validate_done", requestId,
                         {{"serviceName", serviceName.toUri()},
                          {"valid", "true"}});
    }
    NDN_LOG_DEBUG("OnRequestDecryptionSuccessCallbackV2: Permission Granted to "
                 << requesterIdentity.toUri()
                 << " for " << serviceName.toUri());
    const ndn::Name pendingKey = ndn::Name(requesterIdentity.toUri())
                                    .append(serviceName)
                                    .append(requestId);
    const std::string requestTokenHash =
        m_useTokens ? replayTokenHash("REQUEST", requesterIdentity,
                                      serviceName, requestMessage.getUserToken()) : "";
    {
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        if (m_recentProviderRequests.find(pendingKey) != m_recentProviderRequests.end() ||
            (!requestTokenHash.empty() &&
             m_recentProviderRequestTokenHashes.find(requestTokenHash) !=
                 m_recentProviderRequestTokenHashes.end())) {
            const bool duplicateRequest =
                m_recentProviderRequests.find(pendingKey) != m_recentProviderRequests.end();
            const bool duplicateToken =
                !requestTokenHash.empty() &&
                m_recentProviderRequestTokenHashes.find(requestTokenHash) !=
                    m_recentProviderRequestTokenHashes.end();
            NDN_LOG_WARN("NDNSF_PROVIDER_REPLAY_REJECTED provider=" << identity.toUri()
                         << " requester=" << requesterIdentity.toUri()
                         << " service=" << serviceName.toUri()
                         << " requestId=" << requestId.toUri()
                         << " reason="
                         << (duplicateRequest && duplicateToken ? "duplicate-request-and-token" :
                             duplicateRequest ? "duplicate-request" : "duplicate-token"));
            std::cout << "NDNSF_PROVIDER_REPLAY_REJECTED"
                      << " provider=" << identity.toUri()
                      << " requester=" << requesterIdentity.toUri()
                      << " service=" << serviceName.toUri()
                      << " requestId=" << requestId.toUri()
                      << " reason="
                      << (duplicateRequest && duplicateToken ? "duplicate-request-and-token" :
                          duplicateRequest ? "duplicate-request" : "duplicate-token")
                      << std::endl;
            return;
        }
        m_recentProviderRequests.insert(pendingKey);
        if (!requestTokenHash.empty()) {
            m_recentProviderRequestTokenHashes.insert(requestTokenHash);
            m_pendingRequestTokenHashes[pendingKey] = requestTokenHash;
        }
    }
    schedulePendingRequestCleanup(pendingKey);

    if (requestMessage.getRequestMode() == tlv::TargetedRequest) {
        if (finishTargetedRequestOnEventLoop(requesterIdentity,
                                           serviceName,
                                           requestId,
                                           std::move(requestMessage))) {
            return;
        }
        return;
    }

    if (hasService(serviceName) ||
        m_collaborationServices.find(serviceName) != m_collaborationServices.end()) {
        NDN_LOG_DEBUG("Dispatch request using V2 dynamic handler for "
                     << serviceName.toUri());

        if (shouldSuppressAdaptiveAck(requesterIdentity, serviceName, requestId)) {
            AckDecision decision;
            decision.status = false;
            decision.message = "Provider overloaded";
            finishAckDecisionOnEventLoop(requesterIdentity,
                                         serviceName,
                                         requestId,
                                         std::move(requestMessage),
                                         std::move(decision));
            return;
        }

        auto service = m_services.find(serviceName);
        auto collabService = m_collaborationServices.find(serviceName);
        // spec182: remember the scoped registration (if any) this acceptance
        // is being taken against, so a positive decision binds the pending
        // request to exactly that generation.  Legacy entries carry no
        // registration state.
        std::shared_ptr<RegistrationState> ackRegistrationState;
        if (service != m_services.end()) {
            ackRegistrationState = service->second.registrationState;
        }
        else if (collabService != m_collaborationServices.end()) {
            ackRegistrationState = collabService->second.registrationState;
        }
        if (service != m_services.end() &&
            requestMessage.getRequestMode() == tlv::TargetedBootstrapRequest) {
            if (!service->second.targetedRequestHandler) {
                AckDecision decision;
                decision.status = false;
                decision.message = "Service is not registered for targeted mode";
                finishAckDecisionOnEventLoop(requesterIdentity,
                                             serviceName,
                                             requestId,
                                             std::move(requestMessage),
                                             std::move(decision));
                return;
            }
            if (requestMessage.getTargetProvider().empty() ||
                !requestMessage.getTargetProvider().equals(identity)) {
                NDN_LOG_DEBUG("Ignore targeted bootstrap for different provider target="
                              << requestMessage.getTargetProvider().toUri()
                              << " local=" << identity.toUri()
                              << " requestId=" << requestId.toUri());
                return;
            }
        }
        else if (service != m_services.end() &&
                 !service->second.requestHandler &&
                 service->second.targetedRequestHandler) {
            AckDecision decision;
            decision.status = false;
            decision.message = "Service is targeted-only";
            finishAckDecisionOnEventLoop(requesterIdentity,
                                         serviceName,
                                         requestId,
                                         std::move(requestMessage),
                                         std::move(decision));
            return;
        }
        AckDecision decision = makeDefaultAckDecision();
        AckStrategyHandler ackHandler;
        if (service != m_services.end() && service->second.ackHandler) {
            ackHandler = service->second.ackHandler;
        }
        else if (collabService != m_collaborationServices.end() &&
                 collabService->second.ackHandler) {
            ackHandler = collabService->second.ackHandler;
        }
        if (ackHandler) {
            if (m_timelineTrace) {
                logTimelineTrace("provider", "ack_decision_start", requestId,
                                 {{"serviceName", serviceName.toUri()}});
            }
            auto asyncAckHandler = ackHandler;
            if (dispatchAckDecisionAsync(requesterIdentity,
                                         serviceName,
                                         requestId,
                                         requestMessage,
                                         std::move(asyncAckHandler),
                                         ackRegistrationState)) {
                return;
            }
            decision = ackHandler(requestMessage);
            if (m_timelineTrace) {
                logTimelineTrace("provider", "ack_decision_done", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"status", decision.status ? "true" : "false"}});
            }
            if (decision.message.empty()) {
                decision.message =
                    decision.status ? "Permission Granted" : "Permission Denied";
            }
        }
        finishAckDecisionOnEventLoop(requesterIdentity,
                                     serviceName,
                                     requestId,
                                     std::move(requestMessage),
                                     std::move(decision),
                                     ackRegistrationState);
        return;
    }

    NDN_LOG_INFO("No V2 dynamic handler for " << serviceName.toUri());

    if (shouldSuppressAdaptiveAck(requesterIdentity, serviceName, requestId)) {
        PublishRequestAckMessageV2(requesterIdentity,
                                   serviceName,
                                   requestId,
                                   false,
                                   "Provider overloaded",
                                   ndn::Buffer(),
                                   m_useTokens ? requestMessage.getUserToken() : "",
                                   "");
        return;
    }
    std::string msg = "Permission Granted";
    const bool requiresDiReservation =
        requestMessage.hasRequestCapabilities() &&
        requestMessage.getRequestCapabilities().hasField(
            "DIReservationSelectionV1") &&
        requestMessage.getRequestCapabilities().getField(
            "DIReservationSelectionV1") == "required";
    if (requiresDiReservation) {
        PublishRequestAckMessageV2(requesterIdentity,
                                   serviceName,
                                   requestId,
                                   false,
                                   "DI_RESERVATION_HANDLER_REQUIRED",
                                   ndn::Buffer(),
                                   m_useTokens ? requestMessage.getUserToken() : "",
                                   "");
        return;
    }
    std::string providerToken;
    {
        std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
        pendingRequests[pendingKey] =
            std::make_shared<RequestMessage>(requestMessage);
        schedulePendingRequestCleanup(pendingKey);
        if (m_useTokens) {
            auto tokenIt = pendingProviderTokens.find(pendingKey);
            if (tokenIt != pendingProviderTokens.end()) {
                providerToken = tokenIt->second;
            }
            else {
                providerToken = makeOneTimeToken();
                pendingProviderTokens[pendingKey] = providerToken;
            }
        }
    }
    PublishRequestAckMessageV2(requesterIdentity,
                               serviceName,
                               requestId,
                               true,
                               msg,
                               ndn::Buffer(),
                               m_useTokens ? requestMessage.getUserToken() : "",
                               providerToken);
}

    void ServiceProvider::OnRequestDecryptionErrorCallback(
        const ndn::Name& requesterIdentity,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const std::string& error)
    {
        // log error
        NDN_LOG_ERROR("OnRequestDecryptionErrorCallback: "
                      << requesterIdentity.toUri() << serviceName.toUri()
                      << requestId.toUri() << " error=" << error);
    }

void ServiceProvider::processNDNSDServiceInfoCallback(const ndnsd::discovery::Details & callback)
{
        NDN_LOG_INFO("Service publish callback received");
}

    void ServiceProvider::onPermissionResponseData(const ndn::Interest& interest,
                                                   const ndn::Data& data)
    {
        const auto expectedController = extractPermissionControllerIdentity(interest);
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
                        auto response =
                            decryptPermissionResponseWithKeyChain(encryptedResponse, m_keyChain);
                        if (response.getTargetIdentity() != identity.toUri()) {
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

    void ServiceProvider::onPermissionResponseTimeout(const ndn::Interest& interest,
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
                    std::bind(&ServiceProvider::onPermissionResponseData, this, _1, _2),
                    [this, nextAttempt](const ndn::Interest& interest, const ndn::lp::Nack&) {
                        onPermissionResponseTimeout(interest, nextAttempt);
                    },
                    [this, nextAttempt](const ndn::Interest& interest) {
                        onPermissionResponseTimeout(interest, nextAttempt);
                    });
            });
    }

    void ServiceProvider::fetchPolicyManifestFromController(const ndn::Name& controllerPrefix,
                                                            int attempt)
    {
        ndn::Name interestName(controllerPrefix);
        interestName.append(ndn::Name("/NDNSF/POLICY-MANIFEST"));

        ndn::Interest interest(interestName);
        interest.setCanBePrefix(false);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::milliseconds(permissionFetchLifetimeMs()));

        NDN_LOG_INFO("Fetch policy manifest: " << interestName
                     << " attempt=" << attempt
                     << "/" << permissionFetchMaxAttempts());
        m_face.expressInterest(
            interest,
            std::bind(&ServiceProvider::onPolicyManifestData, this, _1, _2),
            [this, attempt](const ndn::Interest& interest, const ndn::lp::Nack&) {
                onPolicyManifestTimeout(interest, attempt);
            },
            [this, attempt](const ndn::Interest& interest) {
                onPolicyManifestTimeout(interest, attempt);
            });
    }

    void ServiceProvider::onPolicyManifestData(const ndn::Interest& interest,
                                               const ndn::Data& data)
    {
        const auto expectedController = extractPermissionControllerIdentity(interest);
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
                if (manifest.hasControllerVersion()) {
                    adoptControllerVersion(manifest.getControllerVersion());
                }
                m_requiredKeyEpoch = manifest.getRequiredKeyEpoch();
                m_policyGracePeriodMs = manifest.getGracePeriodMs();
                NDN_LOG_INFO("Installed PolicyManifest " << manifest.toString());
            },
            [](const ndn::Data& badData, const ndn::security::ValidationError& error) {
                NDN_LOG_ERROR("PolicyManifest Data validation failed: "
                              << badData.getName() << " reason=" << error);
            });
    }

    void ServiceProvider::onPolicyManifestTimeout(const ndn::Interest& interest,
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
                    std::bind(&ServiceProvider::onPolicyManifestData, this, _1, _2),
                    [this, nextAttempt](const ndn::Interest& interest, const ndn::lp::Nack&) {
                        onPolicyManifestTimeout(interest, nextAttempt);
                    },
                    [this, nextAttempt](const ndn::Interest& interest) {
                        onPolicyManifestTimeout(interest, nextAttempt);
                    });
            });
    }

    void ServiceProvider::fetchPolicyStatusFromController(const ndn::Name& controllerPrefix,
                                                           const ndn::Name& serviceName,
                                                           int attempt,
                                                           std::optional<ControllerVersion> expectedVersion)
    {
        if (controllerPrefix.empty() || serviceName.empty()) {
            return;
        }
        const auto serviceKey = serviceName.toUri();
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            if (attempt == 1 && !m_policyStatusFetchInFlight.insert(serviceKey).second) {
                return;
            }
        }

        ndn::Name interestName;
        if (expectedVersion) {
            interestName = makePolicyStatusName(controllerPrefix, serviceName,
                                                *expectedVersion);
        }
        else {
            interestName = controllerPrefix;
            interestName.append("NDNSF").append("POLICY-STATUS").append(serviceName);
        }
        ndn::Interest interest(interestName);
        interest.setCanBePrefix(!expectedVersion);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::milliseconds(permissionFetchLifetimeMs()));

        NDN_LOG_INFO("Fetch policy status: " << interestName
                     << " attempt=" << attempt
                     << "/" << permissionFetchMaxAttempts());
        m_face.expressInterest(
            interest,
            [this, attempt](const ndn::Interest& statusInterest,
                            const ndn::Data& statusData) {
                onPolicyStatusData(statusInterest, statusData, attempt);
            },
            [this, attempt](const ndn::Interest& retryInterest, const ndn::lp::Nack&) {
                onPolicyStatusTimeout(retryInterest, attempt);
            },
            [this, attempt](const ndn::Interest& retryInterest) {
                onPolicyStatusTimeout(retryInterest, attempt);
            });
    }

    void ServiceProvider::onPolicyStatusData(const ndn::Interest& interest,
                                             const ndn::Data& data,
                                             int attempt)
    {
        const auto expectedController = extractPermissionControllerIdentity(interest);
        const auto parsedInterest = expectedController ?
            parsePolicyStatusName(*expectedController, interest.getName()) :
            std::optional<PolicyStatusName>();
        const ndn::Name serviceName = parsedInterest ?
            parsedInterest->serviceName : ndn::Name();
        const auto requestedVersion = parsedInterest ? parsedInterest->version : std::nullopt;
        const auto serviceKey = serviceName.toUri();
        auto clearInFlight = [this, serviceKey] {
            if (serviceKey.empty()) {
                return;
            }
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            m_policyStatusFetchInFlight.erase(serviceKey);
        };

        auto retryValidationFailure = [this, interest, serviceKey, attempt,
                                       clearInFlight] {
            {
                std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
                const auto coordinator = m_policyRefreshCoordinators.find(serviceKey);
                if (coordinator != m_policyRefreshCoordinators.end()) {
                    coordinator->second.failFetch(nowMilliseconds());
                }
            }
            clearInFlight();
            const int maxAttempts = permissionFetchMaxAttempts();
            if (attempt >= maxAttempts) {
                NDN_LOG_ERROR("PolicyStatus validation exhausted: "
                              << interest.getName()
                              << " attempt=" << attempt << "/" << maxAttempts);
                return;
            }
            const int nextAttempt = attempt + 1;
            const int backoffMs = permissionFetchRetryBackoffMs(attempt);
            NDN_LOG_WARN("PolicyStatus validation retry: "
                         << interest.getName()
                         << " attempt=" << attempt
                         << " retryAttempt=" << nextAttempt
                         << " backoffMs=" << backoffMs);
            m_scheduler.schedule(ndn::time::milliseconds(backoffMs),
                [this, interest, nextAttempt] {
                    const auto controllerPrefix =
                        extractPermissionControllerIdentity(interest)
                            .value_or(ndn::Name());
                    const auto parsed = parsePolicyStatusName(
                        controllerPrefix, interest.getName());
                    if (!parsed) {
                        return;
                    }
                    fetchPolicyStatusFromController(
                        controllerPrefix, parsed->serviceName,
                        nextAttempt, parsed->version);
                });
        };

        validator->validateWithConfiguredTrustSchema(
            data,
            [this, expectedController, serviceName, requestedVersion,
             clearInFlight, retryValidationFailure](const ndn::Data& validatedData) {
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
                const auto parsedData = expectedController ?
                    parsePolicyStatusName(*expectedController, validatedData.getName()) :
                    std::optional<PolicyStatusName>();
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
                const bool exactNameMatches =
                    parsedData && parsedData->version &&
                    parsedData->serviceName == serviceName &&
                    (!requestedVersion || parsedData->version == requestedVersion) &&
                    status.getControllerVersion() == *parsedData->version &&
                    validatedData.getName() == makePolicyStatusName(
                        *expectedController, serviceName, *parsedData->version);
                if (!ok || serviceName.empty() || status.getServiceName() != serviceName ||
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

    void ServiceProvider::onPolicyStatusTimeout(const ndn::Interest& interest,
                                                int attempt)
    {
        const auto controllerPrefix = extractPermissionControllerIdentity(interest)
            .value_or(ndn::Name());
        const auto parsed = parsePolicyStatusName(controllerPrefix, interest.getName());
        if (parsed) {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            const auto coordinator = m_policyRefreshCoordinators.find(
                parsed->serviceName.toUri());
            if (coordinator != m_policyRefreshCoordinators.end()) {
                coordinator->second.failFetch(nowMilliseconds());
            }
        }
        const int maxAttempts = permissionFetchMaxAttempts();
        if (attempt >= maxAttempts) {
            const auto& name = interest.getName();
            for (size_t i = 0; i + 1 < name.size(); ++i) {
                if (name[i].toUri() == "POLICY-STATUS") {
                    std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
                    m_policyStatusFetchInFlight.erase(name.getSubName(i + 1).toUri());
                    break;
                }
            }
            NDN_LOG_ERROR("PolicyStatus timeout: " << interest.getName()
                          << " attempt=" << attempt << "/" << maxAttempts
                          << " final=1");
            return;
        }

        const int nextAttempt = attempt + 1;
        const int backoffMs = permissionFetchRetryBackoffMs(attempt);
        NDN_LOG_WARN("PolicyStatus timeout: " << interest.getName()
                     << " attempt=" << attempt << "/" << maxAttempts
                     << " retryAttempt=" << nextAttempt
                     << " backoffMs=" << backoffMs);
        m_scheduler.schedule(ndn::time::milliseconds(backoffMs),
            [this, interest, nextAttempt] {
                const auto controllerPrefix = extractPermissionControllerIdentity(interest)
                    .value_or(ndn::Name());
                const auto parsed = parsePolicyStatusName(controllerPrefix, interest.getName());
                if (!parsed) {
                    return;
                }
                fetchPolicyStatusFromController(controllerPrefix,
                                                parsed->serviceName,
                                                nextAttempt,
                                                parsed->version);
            });
    }

    void ServiceProvider::maybeRefreshControllerVersionHint(
        const ndn::Name& serviceName,
        const std::optional<ControllerVersion>& messageVersion) const
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
        // Some synchronous test/adapter entry points are const for historical
        // API compatibility, but an authenticated higher hint intentionally
        // schedules a network refresh.  The refresh mutates only the provider's
        // bounded coordinator/Face state and is therefore routed through the
        // existing non-const implementation.
        auto* self = const_cast<ServiceProvider*>(this);
        bool shouldFetch = false;
        {
            std::lock_guard<std::mutex> lock(self->m_controllerVersionMutex);
            auto [coordinator, inserted] = self->m_policyRefreshCoordinators.try_emplace(
                serviceName.toUri(), serviceName);
            (void) inserted;
            const auto result = coordinator->second.observeHint(
                *messageVersion, true, nowMilliseconds());
            shouldFetch = result.outcome ==
                PolicyRefreshCoordinator::Outcome::FETCH_STARTED;
        }
        if (shouldFetch) {
            self->fetchPolicyStatusFromController(
                controllerPrefix, serviceName, 1, *messageVersion);
        }
    }

    bool ServiceProvider::replyFromIMS(const ndn::Interest &interest)
    {
        const auto streamEvent = parseInvocationEventName(interest.getName());
        std::optional<ndn::Data> dataToSend;
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            if (auto data = m_IMS.find(interest)) {
                dataToSend.emplace(*data);
            }
        }
        if (dataToSend)
        {
            // A discovery Interest (CanBePrefix) for a segmented object must
            // learn the final segment from the served first Data; otherwise a
            // SegmentFetcher keeps requesting past the last segment until its
            // deadline.  The IMS does not track final block ids, so walk the
            // contiguous retained segments and publish the last one.
            if (interest.getCanBePrefix() &&
                dataToSend->getName().at(-1).isSegment() &&
                !dataToSend->getFinalBlock()) {
                uint64_t finalSegment = dataToSend->getName().at(-1).toSegment();
                while (true) {
                    auto nextName = dataToSend->getName();
                    nextName.set(-1, ndn::name::Component::fromSegment(
                        finalSegment + 1));
                    bool hasNext = false;
                    {
                        std::lock_guard<std::mutex> lock(_cache_mutex);
                        hasNext = static_cast<bool>(m_IMS.find(nextName));
                    }
                    if (!hasNext)
                        break;
                    ++finalSegment;
                }
                auto finalized = *dataToSend;
                finalized.setFinalBlock(
                    ndn::name::Component::fromSegment(finalSegment));
                dataToSend = std::move(finalized);
            }
            if (streamEvent && std::getenv("SPEC175_TRACE") != nullptr) {
                NDN_LOG_INFO("SPEC175_TRACE stream-exact-ims-hit name="
                             << interest.getName()
                             << " cursor=" << streamEvent->cursor);
            }
            NDN_LOG_TRACE("Reply from IMS: " << interest.getName().toUri());
            m_face.put(*dataToSend);
            return true;
        }else{
            if (streamEvent && std::getenv("SPEC175_TRACE") != nullptr) {
                NDN_LOG_INFO("SPEC175_TRACE stream-exact-ims-miss name="
                             << interest.getName()
                             << " cursor=" << streamEvent->cursor);
            }
            NDN_LOG_TRACE("Not Found In IMS: " << interest.getName().toUri());
            // for(auto d:m_IMS)
            // {
            //     NDN_LOG_TRACE("In IMS: " << d.getName().toUri());
            // }
        }
        return false;
    }

    void ServiceProvider::pruneExpiredPendingImsInterestsLocked()
    {
        const auto now = ndn::time::steady_clock::now();
        m_pendingImsInterestCount = 0;
        for (auto it = m_pendingImsInterestsByName.begin();
             it != m_pendingImsInterestsByName.end();) {
            auto& bucket = it->second;
            bucket.erase(
                std::remove_if(bucket.begin(), bucket.end(),
                               [now](const PendingImsInterest& item) {
                                   return item.expiresAt <= now;
                               }),
                bucket.end());
            if (bucket.empty()) {
                it = m_pendingImsInterestsByName.erase(it);
            }
            else {
                m_pendingImsInterestCount += bucket.size();
                ++it;
            }
        }
        m_pendingPrefixImsInterests.erase(
            std::remove_if(m_pendingPrefixImsInterests.begin(),
                           m_pendingPrefixImsInterests.end(),
                           [now](const PendingImsInterest& item) {
                               return item.expiresAt <= now;
                           }),
            m_pendingPrefixImsInterests.end());
        m_pendingImsInterestCount += m_pendingPrefixImsInterests.size();
    }

    void ServiceProvider::rememberPendingImsInterest(const ndn::Interest& interest)
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        pruneExpiredPendingImsInterestsLocked();
        const size_t maxPending =
            static_cast<size_t>(std::max(0, intEnvOrDefault("NDNSF_PENDING_IMS_INTEREST_MAX", 4096)));
        if (maxPending == 0) {
            return;
        }
        while (m_pendingImsInterestCount >= maxPending &&
               !m_pendingImsInsertionOrder.empty()) {
            const auto oldestName = m_pendingImsInsertionOrder.front();
            m_pendingImsInsertionOrder.pop_front();
            auto bucketIt = m_pendingImsInterestsByName.find(oldestName);
            if (bucketIt == m_pendingImsInterestsByName.end() ||
                bucketIt->second.empty()) {
                continue;
            }
            bucketIt->second.pop_front();
            --m_pendingImsInterestCount;
            if (bucketIt->second.empty()) {
                m_pendingImsInterestsByName.erase(bucketIt);
            }
        }
        while (m_pendingImsInterestCount >= maxPending &&
               !m_pendingPrefixImsInterests.empty()) {
            m_pendingPrefixImsInterests.erase(m_pendingPrefixImsInterests.begin());
            --m_pendingImsInterestCount;
        }
        const auto now = ndn::time::steady_clock::now();
        PendingImsInterest item{
            interest,
            now,
            now + interest.getInterestLifetime()
        };
        if (interest.getCanBePrefix()) {
            m_pendingPrefixImsInterests.push_back(std::move(item));
        }
        else {
            m_pendingImsInterestsByName[interest.getName()].push_back(std::move(item));
            m_pendingImsInsertionOrder.push_back(interest.getName());
        }
        ++m_pendingImsInterestCount;
        if (isTruthyEnv("NDNSF_PENDING_IMS_TIMING")) {
            NDN_LOG_WARN("NDNSF_PENDING_IMS_TIMING event=remember"
                         << " interest=" << interest.getName().toUri()
                         << " lifetime_ms=" << interest.getInterestLifetime().count()
                         << " pending=" << m_pendingImsInterestCount);
        }
        NDN_LOG_TRACE("Pending IMS Interest: " << interest.getName().toUri()
                      << " pending=" << m_pendingImsInterestCount);
    }

    void ServiceProvider::satisfyPendingImsInterestsLocked(const ndn::Data& insertedData)
    {
        std::vector<ndn::Data> toSend;
        const auto now = ndn::time::steady_clock::now();
        const bool timingEnabled = isTruthyEnv("NDNSF_PENDING_IMS_TIMING");

        auto satisfyItem = [&](const PendingImsInterest& item) {
            // Streamed events are retained in the IMS with an explicit
            // retention interval, while the application-signed Data packet
            // may intentionally omit a wire FreshnessPeriod.  In that case
            // ndn-cxx's MustBeFresh selector rejects an otherwise exact
            // same-name event even though the IMS has just marked it fresh.
            // For an exact streamed-event Interest, same-name equality is
            // therefore the authoritative match; prefix/general Interests
            // continue to use the normal selector semantics.
            const bool exactStreamEvent =
                item.interest.getName() == insertedData.getName() &&
                parseInvocationEventName(insertedData.getName()).has_value();
            if (item.expiresAt <= now ||
                (!item.interest.matchesData(insertedData) && !exactStreamEvent)) {
                return false;
            }
            if (timingEnabled) {
                const auto ageUs = ndn::time::duration_cast<ndn::time::microseconds>(
                    now - item.requestedAt).count();
                NDN_LOG_WARN("NDNSF_PENDING_IMS_TIMING event=satisfy"
                             << " interest=" << item.interest.getName().toUri()
                             << " dataName=" << insertedData.getName().toUri()
                             << " pending_age_ms=" << (ageUs / 1000.0)
                             << " remaining_before=" << m_pendingImsInterestCount);
            }
            toSend.emplace_back(insertedData);
            return true;
        };

        auto bucketIt = m_pendingImsInterestsByName.find(insertedData.getName());
        if (bucketIt != m_pendingImsInterestsByName.end()) {
            auto& bucket = bucketIt->second;
            std::deque<PendingImsInterest> pending;
            for (const auto& item : bucket) {
                if (satisfyItem(item)) {
                    --m_pendingImsInterestCount;
                }
                else if (item.expiresAt > now) {
                    pending.push_back(item);
                }
                else {
                    --m_pendingImsInterestCount;
                }
            }
            if (pending.empty()) {
                m_pendingImsInterestsByName.erase(bucketIt);
            }
            else {
                bucket = std::move(pending);
            }
        }

        std::vector<PendingImsInterest> pendingPrefix;
        pendingPrefix.reserve(m_pendingPrefixImsInterests.size());
        for (const auto& item : m_pendingPrefixImsInterests) {
            if (satisfyItem(item)) {
                --m_pendingImsInterestCount;
            }
            else if (item.expiresAt > now) {
                pendingPrefix.push_back(item);
            }
            else {
                --m_pendingImsInterestCount;
            }
        }
        m_pendingPrefixImsInterests = std::move(pendingPrefix);

        for (const auto& data : toSend) {
            m_face.put(data);
        }
    }

    void ServiceProvider::satisfyPendingImsInterestsLocked()
    {
        pruneExpiredPendingImsInterestsLocked();
        std::vector<ndn::Data> toSend;
        const auto now = ndn::time::steady_clock::now();
        const bool timingEnabled = isTruthyEnv("NDNSF_PENDING_IMS_TIMING");
        for (auto it = m_pendingImsInterestsByName.begin();
             it != m_pendingImsInterestsByName.end();) {
            auto& bucket = it->second;
            std::deque<PendingImsInterest> pending;
            for (const auto& item : bucket) {
                if (auto data = m_IMS.find(item.interest)) {
                    if (timingEnabled) {
                        const auto ageUs = ndn::time::duration_cast<ndn::time::microseconds>(
                            now - item.requestedAt).count();
                        NDN_LOG_WARN("NDNSF_PENDING_IMS_TIMING event=satisfy"
                                     << " interest=" << item.interest.getName().toUri()
                                     << " dataName=" << data->getName().toUri()
                                     << " pending_age_ms=" << (ageUs / 1000.0)
                                     << " remaining_before=" << m_pendingImsInterestCount);
                    }
                    toSend.emplace_back(*data);
                    --m_pendingImsInterestCount;
                }
                else {
                    pending.push_back(item);
                }
            }
            if (pending.empty()) {
                it = m_pendingImsInterestsByName.erase(it);
            }
            else {
                bucket = std::move(pending);
                ++it;
            }
        }
        std::vector<PendingImsInterest> pendingPrefix;
        pendingPrefix.reserve(m_pendingPrefixImsInterests.size());
        for (const auto& item : m_pendingPrefixImsInterests) {
            if (auto data = m_IMS.find(item.interest)) {
                if (timingEnabled) {
                    const auto ageUs = ndn::time::duration_cast<ndn::time::microseconds>(
                        now - item.requestedAt).count();
                    NDN_LOG_WARN("NDNSF_PENDING_IMS_TIMING event=satisfy"
                                 << " interest=" << item.interest.getName().toUri()
                                 << " dataName=" << data->getName().toUri()
                                 << " pending_age_ms=" << (ageUs / 1000.0)
                                 << " remaining_before=" << m_pendingImsInterestCount);
                }
                toSend.emplace_back(*data);
                --m_pendingImsInterestCount;
            }
            else {
                pendingPrefix.push_back(item);
            }
        }
        m_pendingPrefixImsInterests = std::move(pendingPrefix);
        for (const auto& data : toSend) {
            m_face.put(data);
        }
    }

    void ServiceProvider::insertDataIntoIMS(const ndn::Data& data)
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        m_IMS.insert(data);
        satisfyPendingImsInterestsLocked(data);
    }

    void ServiceProvider::insertDataIntoIMS(const ndn::Data& data,
                                            const ndn::time::milliseconds& freshness)
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        m_IMS.insert(data, freshness);
        satisfyPendingImsInterestsLocked(data);
    }

    void ServiceProvider::onPrefixRegisterFailure(const ndn::Name &prefix, const std::string &reason)
    {
        // log error
        NDN_LOG_ERROR("Prefix registration failed for prefix " << prefix.toUri() << " reason: " << reason);
    }
    void ServiceProvider::onInterest(const ndn::InterestFilter &, const ndn::Interest &interest)
    {
        const auto streamEvent = parseInvocationEventName(interest.getName());
        if (streamEvent && std::getenv("SPEC175_TRACE") != nullptr) {
            NDN_LOG_INFO("SPEC175_TRACE stream-exact-interest name="
                         << interest.getName()
                         << " cursor=" << streamEvent->cursor
                         << " lifetimeMs="
                         << interest.getInterestLifetime().count());
        }
        // log interest
        NDN_LOG_DEBUG("Received Interest: " << interest.getName().toUri());
        if (handleExecutionActivateInterest(interest)) {
            return;
        }
        if (replySelectionExecutionStatus(interest)) {
            return;
        }
        if (!replyFromIMS(interest)) {
            rememberPendingImsInterest(interest);
            if (streamEvent && std::getenv("SPEC175_TRACE") != nullptr) {
                NDN_LOG_INFO("SPEC175_TRACE stream-exact-pending name="
                             << interest.getName()
                             << " cursor=" << streamEvent->cursor);
            }
        }

    }

    void ServiceProvider::serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data> &contentData, ndn::nacabe::SPtrVector<ndn::Data> &ckData)
    {
        //log data
        NDN_LOG_DEBUG("serveDataWithIMS: " << contentData.size() << " " << ckData.size());
        for (auto data : contentData)
        {
            insertDataIntoIMS(*data);
        }
        for (auto data : ckData)
        {
            insertDataIntoIMS(*data);
        }
    }

    LargeDataFetchResult ServiceProvider::fetchAndDecryptLargeData(
        const ndn::Name& encryptedDataName,
        const std::string& serviceName)
    {
        return fetchAndDecryptLargeDataUntil(encryptedDataName, serviceName,
            std::chrono::steady_clock::time_point::max());
    }

    LargeDataFetchResult ServiceProvider::fetchAndDecryptLargeDataUntil(
        const ndn::Name& encryptedDataName, const std::string& serviceName,
        std::chrono::steady_clock::time_point admittedDeadline,
        std::function<bool()> current)
    {
        const auto stopping = m_fetchStopping;
        LargeDataFetchResult result;
        if (stopping->load()) {
            result.errorMessage = "provider is stopping";
            return result;
        }
        if (encryptedDataName.empty()) {
            result.errorMessage = "encryptedDataName is empty";
            return result;
        }
        if (serviceName.empty()) {
            result.errorMessage = "serviceName is empty";
            return result;
        }
        const auto dataValidator = validator;
        if (!dataValidator) {
            result.errorMessage = "large-data fetch requires a configured Data validator";
            return result;
        }

        // The transport and legacy NAC-ABE paths share one request budget.
        // Without a common deadline, a missing object waits once for the
        // SegmentFetcher and then waits again for the legacy fallback, making
        // a single failed lookup consume roughly two full timeout periods.
        const int fetchTimeoutMs = std::max(
            100, intEnvOrDefault("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", 30000));
        const auto overallDeadline = std::min(admittedDeadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(fetchTimeoutMs));
        if (std::chrono::steady_clock::now() >= overallDeadline) {
            result.errorMessage = "large-data admission deadline expired";
            return result;
        }
        const int interestLifetimeMs = std::max(
            50, std::min(4000, fetchTimeoutMs));

        auto fetchLegacyNacAbe = [this, stopping, current, &encryptedDataName, &serviceName,
                                  overallDeadline, interestLifetimeMs]() {
            LargeDataFetchResult legacyResult;
            if (std::chrono::steady_clock::now() >= overallDeadline ||
                (current && !current())) {
                legacyResult.errorMessage = "large-data authority or deadline expired";
                return legacyResult;
            }
            auto completed = std::make_shared<std::atomic<bool>>(false);
            auto mutex = std::make_shared<std::mutex>();
            auto cv = std::make_shared<std::condition_variable>();
            auto error = std::make_shared<std::string>();
            auto plaintext = std::make_shared<ndn::Buffer>();

            boost::asio::post(m_face.getIoContext(),
                [this, stopping, current, overallDeadline, encryptedDataName, completed, mutex, cv, error, plaintext,
                 interestLifetimeMs] {
                if (stopping->load() || std::chrono::steady_clock::now() >= overallDeadline ||
                    (current && !current())) return;
                ndn::Interest interest(encryptedDataName);
                interest.setCanBePrefix(true);
                interest.setMustBeFresh(true);
                interest.setInterestLifetime(
                    ndn::time::milliseconds(interestLifetimeMs));

                try {
                    activeNacConsumer().consume(
                        interest,
                        [completed, mutex, cv, plaintext](const ndn::Buffer& buffer) {
                            {
                                std::lock_guard<std::mutex> lock(*mutex);
                                *plaintext = buffer;
                                completed->store(true);
                            }
                            cv->notify_one();
                        },
                        [completed, mutex, cv, error](const std::string& reason) {
                            {
                                std::lock_guard<std::mutex> lock(*mutex);
                                *error = reason;
                                completed->store(true);
                            }
                            cv->notify_one();
                        });
                }
                catch (const std::exception& e) {
                    {
                        std::lock_guard<std::mutex> lock(*mutex);
                        *error = std::string("large-data fetch/decrypt failed: ") + e.what();
                        completed->store(true);
                    }
                    cv->notify_one();
                }
            });

            std::unique_lock<std::mutex> lock(*mutex);
            while (!completed->load() && !stopping->load() &&
                   (!current || current()) &&
                   std::chrono::steady_clock::now() < overallDeadline) {
                cv->wait_until(lock, std::min(overallDeadline,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
            }

            if (!completed->load()) {
                legacyResult.errorMessage = "large-data fetch timed out or data not found";
                return legacyResult;
            }
            if (!error->empty()) {
                legacyResult.errorMessage = "large-data authorization/decryption failure for " +
                                            serviceName + ": " + *error;
                return legacyResult;
            }

            legacyResult.plaintext.assign(plaintext->begin(), plaintext->end());
            legacyResult.success = true;
            return legacyResult;
        };

        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto mutex = std::make_shared<std::mutex>();
        auto cv = std::make_shared<std::condition_variable>();
        auto error = std::make_shared<std::string>();
        auto encodedEnvelope = std::make_shared<ndn::Buffer>();

        boost::asio::post(m_face.getIoContext(), [this, stopping, current, overallDeadline, dataValidator, encryptedDataName,
                                                  completed, mutex, cv, error, encodedEnvelope,
                                                  interestLifetimeMs, fetchTimeoutMs] {
            if (stopping->load() || std::chrono::steady_clock::now() >= overallDeadline ||
                (current && !current())) return;
            ndn::Interest interest(encryptedDataName);
            interest.setCanBePrefix(true);
            interest.setMustBeFresh(true);
            interest.setInterestLifetime(
                ndn::time::milliseconds(interestLifetimeMs));

            try {
                ndn::SegmentFetcher::Options options;
                options.probeLatestVersion = false;
                options.useConstantCwnd = true;
                options.initCwnd = static_cast<double>(
                    std::max(1, intEnvOrDefault("NDNSF_REQUEST_LARGE_FETCH_INIT_CWND", 8)));
                options.maxTimeout = ndn::time::milliseconds(std::max<int64_t>(1,
                    std::min<int64_t>(std::min(10000, fetchTimeoutMs),
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            overallDeadline - std::chrono::steady_clock::now()).count())));
                options.interestLifetime = ndn::time::milliseconds(interestLifetimeMs);
                auto transportValidator = dataValidator;
                auto fetcher = ndn::SegmentFetcher::start(m_face,
                                                           interest,
                                                           transportValidator->getConfiguredValidatorForSegmentFetcher(),
                                                           options);
                fetcher->onComplete.connect(
                    [completed, mutex, cv, encodedEnvelope, transportValidator](ndn::ConstBufferPtr buffer) {
                        {
                            std::lock_guard<std::mutex> lock(*mutex);
                            encodedEnvelope->assign(buffer->begin(), buffer->end());
                            completed->store(true);
                        }
                        cv->notify_one();
                    });
                fetcher->onError.connect(
                    [completed, mutex, cv, error, transportValidator](uint32_t code, const std::string& reason) {
                        {
                            std::lock_guard<std::mutex> lock(*mutex);
                            *error = "SegmentFetcher error " + std::to_string(code) +
                                     ": " + reason;
                            completed->store(true);
                        }
                        cv->notify_one();
                    });
            }
            catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lock(*mutex);
                    *error = std::string("large-data fetch/decrypt failed: ") + e.what();
                    completed->store(true);
                }
                cv->notify_one();
            }
        });

        std::unique_lock<std::mutex> lock(*mutex);
        while (!completed->load() && !stopping->load() &&
               (!current || current()) &&
               std::chrono::steady_clock::now() < overallDeadline) {
            cv->wait_until(lock, std::min(overallDeadline,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
        }
        if (stopping->load()) {
            result.errorMessage = "provider is stopping";
            return result;
        }

        if (!completed->load()) {
            return fetchLegacyNacAbe();
        }
        if (!error->empty()) {
            return fetchLegacyNacAbe();
        }

        HybridMessageEnvelope envelope;
        try {
            ndn::Block block(*encodedEnvelope);
            if (!envelope.WireDecode(block)) {
                return fetchLegacyNacAbe();
            }
        }
        catch (const std::exception&) {
            return fetchLegacyNacAbe();
        }

        const auto messageType = envelope.getMessageType();
        if (messageType != "REQUEST-LARGE") {
            result.errorMessage = "large-data hybrid envelope has unexpected message type " +
                                  messageType;
            return result;
        }

        auto decryptCompleted = std::make_shared<std::atomic<bool>>(false);
        auto decryptMutex = std::make_shared<std::mutex>();
        auto decryptCv = std::make_shared<std::condition_variable>();
        auto decryptError = std::make_shared<std::string>();
        auto plaintext = std::make_shared<ndn::Buffer>();

        auto finishDecrypt = [encryptedDataName,
                              serviceName,
                              envelope,
                              plaintext,
                              decryptCompleted,
                              decryptMutex,
                              decryptCv,
                              decryptError](const ndn::Buffer& key) mutable {
            const std::string adText = encryptedDataName.toUri() + "|" +
                                       envelope.getMessageType() + "|" + serviceName;
            const ndn::Buffer ad(reinterpret_cast<const uint8_t*>(adText.data()),
                                 adText.size());
            ndn::Buffer decrypted;
            const bool ok = hybridAesGcmDecrypt(
                key, envelope, ndn::span<const uint8_t>(ad.data(), ad.size()), decrypted);
            {
                std::lock_guard<std::mutex> lock(*decryptMutex);
                if (!ok) {
                    *decryptError = "hybrid AES-GCM authentication failed";
                }
                else {
                    *plaintext = decrypted;
                }
                decryptCompleted->store(true);
            }
            decryptCv->notify_one();
        };

        ndn::Buffer key;
        if (m_hybridMessageCrypto.findReceiveKey(envelope.getKeyId(),
                                                 key,
                                                 m_hybridCryptoCounters)) {
            finishDecrypt(key);
        }
        else if (envelope.hasWrappedMessageKey()) {
            boost::asio::post(m_face.getIoContext(),
                [this, stopping, current, overallDeadline, envelope, serviceName, encryptedDataName, finishDecrypt, decryptCompleted,
                 decryptMutex, decryptCv, decryptError]() mutable {
                    if (stopping->load() || std::chrono::steady_clock::now() >= overallDeadline ||
                        (current && !current())) return;
                    const auto keyDataName = makeHybridMessageKeyDataName(
                        ndn::Name(serviceName), extractLargeDataProducerPrefix(encryptedDataName),
                        std::string("/SERVICE") + serviceName,
                        envelope.getEpochId());
                    activeNacConsumer().consume(
                        keyDataName,
                        makeNacInlineContentBlock(envelope.getWrappedMessageKey()),
                        [this, stopping, current, overallDeadline, serviceName, envelope, finishDecrypt](const ndn::Buffer& unwrappedKey) mutable {
                            if (stopping->load() || std::chrono::steady_clock::now() >= overallDeadline ||
                                (current && !current())) return;
                            m_hybridMessageCrypto.cacheReceiveKey(serviceName,
                                                                  envelope.getKeyId(),
                                                                  envelope.getEpochId(),
                                                                  unwrappedKey);
                            finishDecrypt(unwrappedKey);
                        },
                        [decryptCompleted, decryptMutex, decryptCv, decryptError](
                            const std::string& reason) {
                            {
                                std::lock_guard<std::mutex> lock(*decryptMutex);
                                *decryptError = "hybrid MessageKey unwrap failed: " + reason;
                                decryptCompleted->store(true);
                            }
                            decryptCv->notify_one();
                        });
                });
        }
        else {
            boost::asio::post(m_face.getIoContext(),
                [this, stopping, current, overallDeadline, serviceName, encryptedDataName, envelope, finishDecrypt, decryptCompleted,
                 decryptMutex, decryptCv, decryptError]() mutable {
                    if (stopping->load() || std::chrono::steady_clock::now() >= overallDeadline ||
                        (current && !current())) return;
                    const auto keyDataName = makeHybridMessageKeyDataName(
                        ndn::Name(serviceName), extractLargeDataProducerPrefix(encryptedDataName),
                        std::string("/SERVICE") + serviceName,
                        envelope.getEpochId());
                    activeNacConsumer().consume(
                        keyDataName,
                        [this, stopping, current, overallDeadline, serviceName, envelope, finishDecrypt](const ndn::Buffer& unwrappedKey) mutable {
                            if (stopping->load() || std::chrono::steady_clock::now() >= overallDeadline ||
                                (current && !current())) return;
                            m_hybridMessageCrypto.cacheReceiveKey(serviceName,
                                                                  envelope.getKeyId(),
                                                                  envelope.getEpochId(),
                                                                  unwrappedKey);
                            finishDecrypt(unwrappedKey);
                        },
                        [decryptCompleted, decryptMutex, decryptCv, decryptError, keyDataName](
                            const std::string& reason) {
                            {
                                std::lock_guard<std::mutex> lock(*decryptMutex);
                                *decryptError = "hybrid MessageKey fetch failed " +
                                                keyDataName.toUri() + ": " + reason;
                                decryptCompleted->store(true);
                            }
                            decryptCv->notify_one();
                        });
                });
        }

        std::unique_lock<std::mutex> decryptLock(*decryptMutex);
        while (!decryptCompleted->load() && !stopping->load() &&
               (!current || current()) &&
               std::chrono::steady_clock::now() < overallDeadline) {
            decryptCv->wait_until(decryptLock, std::min(overallDeadline,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
        }
        if (!decryptCompleted->load()) {
            result.errorMessage = "large-data hybrid decrypt timed out for " +
                                  encryptedDataName.toUri();
            return result;
        }
        if (!decryptError->empty()) {
            result.errorMessage = "large-data hybrid decrypt failure for " +
                                  serviceName + ": " + *decryptError;
            return result;
        }

        result.plaintext.assign(plaintext->begin(), plaintext->end());
        result.success = true;
        return result;
    }

    LargeDataFetchResult ServiceProvider::resolveLargeDataReferencePayload(
        const ndn::Buffer& payload,
        const std::string& serviceName)
    {
        LargeDataFetchResult result;
        const auto reference = parseLargeDataReferencePayload(payload);
        if (!reference) {
            result.plaintext.assign(payload.begin(), payload.end());
            result.success = true;
            return result;
        }
        if (!reference->encrypted) {
            result.errorMessage = "large-data reference is not encrypted";
            return result;
        }
        return fetchAndDecryptLargeData(reference->dataName, serviceName);
    }



    void ServiceProvider::PublishRequestAckMessageV2(const ndn::Name& requesterIdentity,
                                                     const ndn::Name& serviceName,
                                                     const ndn::Name& requestId,
                                                     bool status,
                                                     const std::string& msg,
                                                     const ndn::Buffer& payload,
                                                     const std::string& userToken,
                                                     const std::string& providerToken,
                                                     const RequestMessage* sourceRequest,
                                                     const AckDecision* ackDecision)
    {
        NDN_LOG_DEBUG("PublishRequestAckMessageV2: " << requesterIdentity.toUri()
                     << serviceName.toUri() << requestId.toUri());
        NDN_LOG_DEBUG("[ServiceProvider] ACK publish requestId="
                  << requestId.toUri()
                  << " userToken=" << userToken
                  << " providerToken=" << providerToken);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=ACK_PUBLISHED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " requesterName=" << requesterIdentity.toUri()
                  << " providerName=" << identity.toUri()
                  << " status=" << status);
        updateProviderRequestLifecycleState(
            requestId, serviceName,
            ProviderRequestLifecycleState::ACK_PUBLISHED);

        RequestAckMessage requestAckMessage;
        requestAckMessage.setStatus(status);
        requestAckMessage.setMessage(msg);
        requestAckMessage.setUserToken(userToken);
        requestAckMessage.setProviderToken(providerToken);
        requestAckMessage.setPolicyEpoch(getCurrentPolicyEpoch(serviceName));
        if (const auto version = getControllerVersion(serviceName)) {
            requestAckMessage.setControllerVersion(*version);
        }
        if (status) {
            // ACK advertises the Provider's RSA recipient certificate and
            // supported envelope algorithm; no request key is exposed here.
            requestAckMessage.setProviderEncryptionCertificate(
                makeEncryptionCertificateAdvertisement(identityCert));
        }
        // Deployment discovery is deliberately advisory: constructing this
        // bounded offer performs no fetch, load, warm, reservation, or handler
        // execution. Selection remains the first mutation authority.
        if (status && sourceRequest != nullptr && sourceRequest->hasDeploymentIntent()) {
            ProviderCapabilityOffer offer;
            offer.setField("providerIdentity", identity.toUri());
            offer.setField("providerBootEpoch",
                           identity.toUri() + ":" + std::to_string(m_processStartedAtUs));
            offer.setField("deploymentControlVersion", "1");
            offer.setField("secureStatusVersion", "1");
            offer.setField("intentDigest",
                           sourceRequest->getDeploymentIntent().computeDigest());
            offer.setField("observedAtUs", std::to_string(nowMicroseconds()));
            requestAckMessage.setProviderCapabilityOffer(offer);
        }
        if (status && ackDecision != nullptr) {
            if (ackDecision->selectionInputKeyOffer)
                requestAckMessage.setSelectionInputKeyOffer(
                    *ackDecision->selectionInputKeyOffer);
            if (ackDecision->reservationLease)
                requestAckMessage.setReservationLease(*ackDecision->reservationLease);
        }
        if (status && sourceRequest != nullptr &&
            sourceRequest->hasRequestCapabilities() &&
            ((sourceRequest->getRequestCapabilities().hasField("SelectionGatedInputV1") &&
              sourceRequest->getRequestCapabilities().getField("SelectionGatedInputV1") == "required") ||
             (sourceRequest->getRequestCapabilities().hasField("DIReservationSelectionV1") &&
              sourceRequest->getRequestCapabilities().getField("DIReservationSelectionV1") == "required") ||
             (sourceRequest->getRequestCapabilities().hasField("NDNSF_DATA_V1") &&
              sourceRequest->getRequestCapabilities().getField("NDNSF_DATA_V1") == "required") ||
             (sourceRequest->getRequestCapabilities().hasField("RequestScopedConfidentialityV1") &&
              sourceRequest->getRequestCapabilities().getField("RequestScopedConfidentialityV1") == "required")) &&
            !requestAckMessage.hasSelectionInputKeyOffer()) {
            const auto publicKey = identityCert.getPublicKey();
            ndn::Buffer publicKeyBuffer(publicKey.begin(), publicKey.end());
            SelectionInputKeyOffer offer;
            offer.setField("schemaVersion", "1");
            offer.setField("recipient", identity.toUri());
            offer.setField("recipientCertName", identityCert.getName().toUri());
            offer.setField("recipientPublicKey", selectionGatedHex(publicKeyBuffer));
            offer.setField("recipientCertDigest", sha256DigestString(publicKeyBuffer));
            offer.setField("providerBootEpoch",
                           identity.toUri() + ":" + std::to_string(m_processStartedAtUs));
            // Exact tensor Interests are routed to the Provider identity, not
            // to the unrelated SVS node identifier used by legacy PubSub.
            offer.setField("ndnsfDataV1EndpointPrefix", identity.toUri());
            requestAckMessage.setSelectionInputKeyOffer(offer);
        }
        if (status && sourceRequest != nullptr &&
            sourceRequest->hasStreamRequestOptions() &&
            !requestAckMessage.hasSelectionInputKeyOffer()) {
            const auto publicKey = identityCert.getPublicKey();
            ndn::Buffer publicKeyBuffer(publicKey.begin(), publicKey.end());
            SelectionInputKeyOffer offer;
            offer.setField("schemaVersion", "NDNSF-STREAM-GRANT-V1");
            offer.setField("recipient", identity.toUri());
            offer.setField("recipientCertName", identityCert.getName().toUri());
            offer.setField("recipientPublicKey", selectionGatedHex(publicKeyBuffer));
            offer.setField("recipientCertDigest", sha256DigestString(publicKeyBuffer));
            offer.setField("providerBootEpoch",
                           identity.toUri() + ":" + std::to_string(m_processStartedAtUs));
            requestAckMessage.setSelectionInputKeyOffer(offer);
        }
        if (!payload.empty()) {
            ndn::Buffer ackPayload(payload);
            requestAckMessage.setPayload(ackPayload, ackPayload.size());
        }

        ndn::Name name = makeRequestAckNameV2(identity,
                                              requesterIdentity,
                                              serviceName,
                                              requestId);
        ndn::Name nameWithoutPrefix =
            makeRequestAckNameWithoutPrefixV2(requesterIdentity,
                                              serviceName,
                                              requestId);
        PublishMessage(name, nameWithoutPrefix, requestAckMessage);
        prefetchSelectionMessageV2(requesterIdentity, serviceName, requestId);
    }

    void ServiceProvider::onServiceSelectionMessage(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        handleServiceSelectionMessage(subscription, true);
    }

    void ServiceProvider::prefetchSelectionMessageV2(const ndn::Name& requesterIdentity,
                                                     const ndn::Name& serviceName,
                                                     const ndn::Name& requestId)
    {
        if (!isTruthyEnv("NDNSF_SELECTION_TARGETED_PREFETCH")) {
            return;
        }

        const bool providerProjected =
            m_collaborationServices.find(serviceName) != m_collaborationServices.end() ||
            m_opaqueSelectionParticipants.find(serviceName) !=
                m_opaqueSelectionParticipants.end();
        const auto expressPrefetch =
            [this, requesterIdentity, serviceName, requestId]
            (const ndn::Name& selectionName, const char* selectionMode) {
            ndn::Interest interest(selectionName);
            interest.setCanBePrefix(false);
            interest.setMustBeFresh(false);
            interest.setInterestLifetime(ndn::time::milliseconds(
                std::max(100, intEnvOrDefault(
                    "NDNSF_SELECTION_TARGETED_PREFETCH_LIFETIME_MS", 10000))));

            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_TARGETED_PREFETCH_ISSUED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << requestId.toUri()
                          << " serviceName=" << serviceName.toUri()
                          << " requesterName=" << requesterIdentity.toUri()
                          << " providerName=" << identity.toUri()
                          << " selectionMode=" << selectionMode
                          << " selectionName=" << selectionName.toUri());

            m_face.expressInterest(
                interest,
                [this, requesterIdentity, serviceName, requestId,
                 selectionName, selectionMode]
                (const ndn::Interest&, const ndn::Data& data) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_TARGETED_PREFETCH_DATA timestamp_us="
                              << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " serviceName=" << serviceName.toUri()
                              << " requesterName=" << requesterIdentity.toUri()
                              << " providerName=" << identity.toUri()
                              << " selectionMode=" << selectionMode
                              << " selectionName=" << selectionName.toUri()
                              << " dataName=" << data.getName().toUri()
                              << " contentBytes=" << data.getContent().value_size());
                logControlTiming("provider", "SELECTION_TARGETED_PREFETCH_DATA", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"requesterName", requesterIdentity.toUri()},
                                  {"providerName", identity.toUri()},
                                  {"selectionName", selectionName.toUri()},
                                  {"contentBytes", std::to_string(data.getContent().value_size())}});
                ndn::Name producerPrefix(requesterIdentity);
                producerPrefix.appendNumber(0);
                std::optional<ndn::Data> packet(data);
                ndn::svs::SVSPubSub::SubscriptionData subData{
                    data.getName(),
                    data.getContent().value_bytes(),
                    producerPrefix,
                    0,
                    packet,
                };
                handleServiceSelectionMessage(subData, false);
                },
                [this, requestId, serviceName, requesterIdentity,
                 selectionName, selectionMode]
                (const ndn::Interest&, const ndn::lp::Nack&) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_TARGETED_PREFETCH_NACK timestamp_us="
                              << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " serviceName=" << serviceName.toUri()
                              << " requesterName=" << requesterIdentity.toUri()
                              << " providerName=" << identity.toUri()
                              << " selectionMode=" << selectionMode
                              << " selectionName=" << selectionName.toUri());
                },
                [this, requestId, serviceName, requesterIdentity,
                 selectionName, selectionMode]
                (const ndn::Interest&) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_TARGETED_PREFETCH_TIMEOUT timestamp_us="
                              << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " serviceName=" << serviceName.toUri()
                              << " requesterName=" << requesterIdentity.toUri()
                              << " providerName=" << identity.toUri()
                              << " selectionMode=" << selectionMode
                              << " selectionName=" << selectionName.toUri());
                });
        };

        // Every individual decision is now provider-bound, including a
        // one-provider generic selection.  Always prefetch that exact name.
        // Generic multi-selection retains the compact publication, so only
        // non-collaboration services need the second bounded prefetch.
        expressPrefetch(
            makeServiceSelectionNameV2(requesterIdentity, identity,
                                       serviceName, requestId),
            "provider-projection");
        if (!providerProjected) {
            expressPrefetch(
                makeCompactServiceSelectionNameV2(requesterIdentity,
                                                  serviceName, requestId),
                "compact-fallback");
        }
    }

    void ServiceProvider::handleServiceSelectionMessage(
        const ndn::svs::SVSPubSub::SubscriptionData& subscription,
        bool checkFreshness)
    {
        if(checkFreshness && !isFresh(subscription)) return;

        const auto decisionSelectionV2 =
            ndn_service_framework::parseServiceSelectionDecisionNameV2(subscription.name);
        auto compactSelectionV2 = decisionSelectionV2 ?
            std::optional<CompactServiceSelectionNameV2>{} :
            ndn_service_framework::parseCompactServiceSelectionNameV2(subscription.name);
        if (compactSelectionV2) {
            if (checkFreshness) {
                logValidatedPublicationAudit(
                    "provider", "SELECTION", subscription,
                    compactSelectionV2->requestId, compactSelectionV2->serviceName,
                    compactSelectionV2->requesterName, identity);
            }
            NDN_LOG_DEBUG("Received compact Service Selection Message: "
                          << subscription.name.toUri());
            if (m_timelineTrace) {
                logTimelineTrace("provider", "compact_selection_observed",
                                 compactSelectionV2->requestId,
                                 {{"serviceName", compactSelectionV2->serviceName.toUri()},
                                  {"requesterName", compactSelectionV2->requesterName.toUri()},
                                  {"providerName", identity.toUri()},
                                  {"selectionName", subscription.name.toUri()}});
            }
            const auto selectionKey = ndn::Name(compactSelectionV2->requesterName.toUri())
                                          .append(compactSelectionV2->serviceName)
                                          .append(compactSelectionV2->requestId);
            {
                std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
                if (m_selectedProviderRequests.find(selectionKey) !=
                        m_selectedProviderRequests.end() ||
                    m_selectionDecryptsInFlight.find(selectionKey) !=
                        m_selectionDecryptsInFlight.end()) {
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=COMPACT_SELECTION_DUPLICATE_DROPPED timestamp_us="
                              << nowMicroseconds()
                              << " requestId=" << compactSelectionV2->requestId.toUri()
                              << " serviceName=" << compactSelectionV2->serviceName.toUri()
                              << " requesterName=" << compactSelectionV2->requesterName.toUri()
                              << " providerName=" << identity.toUri()
                              << " pendingKey=" << selectionKey.toUri());
                    return;
                }
                m_selectionDecryptsInFlight.insert(selectionKey);
            }
            logControlTiming("provider", "SELECTION_OBSERVED",
                             compactSelectionV2->requestId,
                             {{"serviceName", compactSelectionV2->serviceName.toUri()},
                              {"requesterName", compactSelectionV2->requesterName.toUri()},
                              {"providerName", identity.toUri()},
                              {"selectionName", subscription.name.toUri()},
                              {"contentBytes", std::to_string(subscription.data.size())},
                              {"compactSelection", "1"}});

            if (subscription.data.size() == 0) {
                OnServiceSelectionMessageDecryptionErrorCallback(
                    compactSelectionV2->requesterName,
                    identity,
                    compactSelectionV2->serviceName,
                    compactSelectionV2->requestId,
                    "compact selection missing payload");
                return;
            }

            const auto decryptStartUs = nowMicroseconds();
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_START timestamp_us="
                      << decryptStartUs
                      << " requestId=" << compactSelectionV2->requestId.toUri()
                      << " requesterName=" << compactSelectionV2->requesterName.toUri()
                      << " providerName=" << identity.toUri()
                      << " serviceName=" << compactSelectionV2->serviceName.toUri()
                      << " selectionName=" << subscription.name.toUri()
                      << " compactSelection=1");
            if (decryptHybridMessage(
                    subscription.name,
                    ndn::Block(subscription.data),
                    [this, requesterName = compactSelectionV2->requesterName,
                     serviceName = compactSelectionV2->serviceName,
                     requestId = compactSelectionV2->requestId,
                     subscriptionName = ndn::Name(subscription.name),
                     decryptStartUs](const ndn::Buffer& buffer) {
                        const auto decryptEndUs = nowMicroseconds();
                        logCryptoDiag("provider", "selection",
                                      "decrypt", "hybrid", "success",
                                      decryptStartUs, decryptEndUs,
                                      subscriptionName, buffer.size());
                        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_DONE timestamp_us="
                                  << decryptEndUs
                                  << " requestId=" << requestId.toUri()
                                  << " requesterName=" << requesterName.toUri()
                                  << " providerName=" << identity.toUri()
                                  << " serviceName=" << serviceName.toUri()
                                  << " selectionName=" << subscriptionName.toUri()
                                  << " payloadBytes=" << buffer.size()
                                  << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                        decryptEndUs - decryptStartUs : 0)
                                  << " compactSelection=1");
                        OnServiceSelectionMessageDecryptionSuccessCallbackV2(
                            requesterName, identity, serviceName, requestId, buffer);
                    },
                    [this, requesterName = compactSelectionV2->requesterName,
                     serviceName = compactSelectionV2->serviceName,
                     requestId = compactSelectionV2->requestId,
                     decryptStartUs](const std::string& error) {
                        const auto decryptEndUs = nowMicroseconds();
                        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_FAILED timestamp_us="
                                  << decryptEndUs
                                  << " requestId=" << requestId.toUri()
                                  << " requesterName=" << requesterName.toUri()
                                  << " providerName=" << identity.toUri()
                                  << " serviceName=" << serviceName.toUri()
                                  << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                        decryptEndUs - decryptStartUs : 0)
                                  << " error=" << error
                                  << " compactSelection=1");
                        OnServiceSelectionMessageDecryptionErrorCallback(
                            requesterName, identity, serviceName,
                            requestId, error);
                    })) {
                return;
            }
            OnServiceSelectionMessageDecryptionErrorCallback(
                compactSelectionV2->requesterName,
                identity,
                compactSelectionV2->serviceName,
                compactSelectionV2->requestId,
                "invalid hybrid compact selection envelope");
            return;
        }

        std::optional<ServiceSelectionNameV2> selectionV2;
        if (decisionSelectionV2) {
            selectionV2 = ServiceSelectionNameV2{
                decisionSelectionV2->requesterName,
                decisionSelectionV2->providerName,
                decisionSelectionV2->serviceName,
                decisionSelectionV2->requestId};
        }
        else {
            selectionV2 =
                ndn_service_framework::parseServiceSelectionNameV2(subscription.name);
        }
        if (selectionV2) {
            if (!selectionV2->providerName.equals(identity)) {
                return;
            }
            if (checkFreshness) {
                logValidatedPublicationAudit(
                    "provider", "SELECTION", subscription,
                    selectionV2->requestId, selectionV2->serviceName,
                    selectionV2->requesterName, selectionV2->providerName);
            }
            NDN_LOG_DEBUG("Received Service Selection Message: "
                          << subscription.name.toUri());
            NDN_LOG_DEBUG("[ServiceProvider] selection received timestampMs="
                      << nowMilliseconds()
                      << " requestId=" << selectionV2->requestId.toUri()
                      << " providerName=" << selectionV2->providerName.toUri()
                      << " requesterName=" << selectionV2->requesterName.toUri()
                      << " serviceName=" << selectionV2->serviceName.toUri());
            if (m_timelineTrace) {
                logTimelineTrace("provider", "selection_observed",
                                 selectionV2->requestId,
                                 {{"serviceName", selectionV2->serviceName.toUri()},
                                  {"requesterName", selectionV2->requesterName.toUri()},
                                  {"providerName", selectionV2->providerName.toUri()},
                                  {"selectionName", subscription.name.toUri()}});
            }
            const auto selectionKey = ndn::Name(selectionV2->requesterName.toUri())
                                          .append(selectionV2->serviceName)
                                          .append(selectionV2->requestId);
            {
                std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
                if (m_selectedProviderRequests.find(selectionKey) !=
                        m_selectedProviderRequests.end() ||
                    m_selectionDecryptsInFlight.find(selectionKey) !=
                        m_selectionDecryptsInFlight.end()) {
                    NDN_LOG_DEBUG("Ignore duplicate V2 selection before decrypt for "
                                  << selectionKey.toUri());
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DUPLICATE_DROPPED timestamp_us="
                              << nowMicroseconds()
                              << " requestId=" << selectionV2->requestId.toUri()
                              << " serviceName=" << selectionV2->serviceName.toUri()
                              << " requesterName=" << selectionV2->requesterName.toUri()
                              << " providerName=" << selectionV2->providerName.toUri()
                              << " pendingKey=" << selectionKey.toUri());
                    return;
                }
                m_selectionDecryptsInFlight.insert(selectionKey);
            }
            logControlTiming("provider", "SELECTION_OBSERVED",
                             selectionV2->requestId,
                             {{"serviceName", selectionV2->serviceName.toUri()},
                              {"requesterName", selectionV2->requesterName.toUri()},
                              {"providerName", selectionV2->providerName.toUri()},
                              {"selectionName", subscription.name.toUri()},
                              {"contentBytes", std::to_string(subscription.data.size())},
                              {"compactSelection", "0"}});

            if(subscription.data.size() > 0){
                const auto decryptStartUs = nowMicroseconds();
                if (m_timelineTrace) {
                    logTimelineTrace("provider", "selection_decrypt_start",
                                     selectionV2->requestId,
                                     {{"serviceName", selectionV2->serviceName.toUri()}});
                }
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_START timestamp_us="
                          << decryptStartUs
                          << " requestId=" << selectionV2->requestId.toUri()
                          << " requesterName=" << selectionV2->requesterName.toUri()
                          << " providerName=" << selectionV2->providerName.toUri()
                          << " serviceName=" << selectionV2->serviceName.toUri()
                          << " selectionName=" << subscription.name.toUri());
                if (decryptHybridMessage(
                        subscription.name,
                        ndn::Block(subscription.data),
                        [this, requesterName = selectionV2->requesterName,
                         providerName = selectionV2->providerName,
                         serviceName = selectionV2->serviceName,
                         requestId = selectionV2->requestId,
                         subscriptionName = ndn::Name(subscription.name),
                         decryptStartUs](const ndn::Buffer& buffer) {
                            const auto decryptEndUs = nowMicroseconds();
                            if (m_timelineTrace) {
                                logTimelineTrace("provider", "selection_decrypt_done", requestId,
                                                 {{"serviceName", serviceName.toUri()},
                                                  {"duration_us",
                                                   std::to_string(decryptEndUs >= decryptStartUs ?
                                                                  decryptEndUs - decryptStartUs : 0)}});
                            }
                            logCryptoDiag("provider", "selection",
                                          "decrypt", "hybrid", "success",
                                          decryptStartUs, decryptEndUs,
                                          subscriptionName, buffer.size());
                            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_DONE timestamp_us="
                                      << decryptEndUs
                                      << " requestId=" << requestId.toUri()
                                      << " requesterName=" << requesterName.toUri()
                                      << " providerName=" << providerName.toUri()
                                      << " serviceName=" << serviceName.toUri()
                                      << " selectionName=" << subscriptionName.toUri()
                                      << " payloadBytes=" << buffer.size()
                                      << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                            decryptEndUs - decryptStartUs : 0));
                            OnServiceSelectionMessageDecryptionSuccessCallbackV2(
                                requesterName, providerName, serviceName,
                                requestId, buffer);
                        },
                        [this, requesterName = selectionV2->requesterName,
                         providerName = selectionV2->providerName,
                         serviceName = selectionV2->serviceName,
                         requestId = selectionV2->requestId,
                         decryptStartUs](const std::string& error) {
                            const auto decryptEndUs = nowMicroseconds();
                            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_FAILED timestamp_us="
                                      << decryptEndUs
                                      << " requestId=" << requestId.toUri()
                                      << " requesterName=" << requesterName.toUri()
                                      << " providerName=" << providerName.toUri()
                                      << " serviceName=" << serviceName.toUri()
                                      << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                            decryptEndUs - decryptStartUs : 0)
                                      << " error=" << error);
                            OnServiceSelectionMessageDecryptionErrorCallback(
                                requesterName, providerName, serviceName,
                                requestId, error);
                        })) {
                    return;
                }
                OnServiceSelectionMessageDecryptionErrorCallback(
                    selectionV2->requesterName,
                    selectionV2->providerName,
                    selectionV2->serviceName,
                    selectionV2->requestId,
                    "invalid hybrid selection envelope");
                return;
                activeNacConsumer().consume(subscription.name,
                                    makeNacInlineContentBlock(subscription.data),
                                    [this, requesterName = selectionV2->requesterName,
                                     providerName = selectionV2->providerName,
                                     serviceName = selectionV2->serviceName,
                                     requestId = selectionV2->requestId,
                                     subscriptionName = ndn::Name(subscription.name),
                                     decryptStartUs](const ndn::Buffer& buffer) {
                                        const auto decryptEndUs = nowMicroseconds();
                                        if (m_timelineTrace) {
                                            logTimelineTrace("provider", "selection_decrypt_done", requestId,
                                                             {{"serviceName", serviceName.toUri()},
                                                              {"duration_us",
                                                               std::to_string(decryptEndUs >= decryptStartUs ?
                                                                              decryptEndUs - decryptStartUs : 0)}});
                                        }
                                        logCryptoDiag("provider", "selection",
                                                      "decrypt", "normal", "success",
                                                      decryptStartUs, decryptEndUs,
                                                      subscriptionName, buffer.size());
                                        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_DONE timestamp_us="
                                                  << decryptEndUs
                                                  << " requestId=" << requestId.toUri()
                                                  << " requesterName=" << requesterName.toUri()
                                                  << " providerName=" << providerName.toUri()
                                                  << " serviceName=" << serviceName.toUri()
                                                  << " selectionName=" << subscriptionName.toUri()
                                                  << " payloadBytes=" << buffer.size()
                                                  << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                                        decryptEndUs - decryptStartUs : 0));
                                        OnServiceSelectionMessageDecryptionSuccessCallbackV2(
                                            requesterName, providerName, serviceName,
                                            requestId, buffer);
                                    },
                                    [this, requesterName = selectionV2->requesterName,
                                     providerName = selectionV2->providerName,
                                     serviceName = selectionV2->serviceName,
                                     requestId = selectionV2->requestId,
                                     subscriptionName = ndn::Name(subscription.name),
                                     decryptStartUs](const std::string& error) {
                                        const auto decryptEndUs = nowMicroseconds();
                                        logCryptoDiag("provider", "selection",
                                                      "decrypt", "normal", "failure",
                                                      decryptStartUs, decryptEndUs,
                                                      subscriptionName, 0, error);
                                        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_FAILED timestamp_us="
                                                  << decryptEndUs
                                                  << " requestId=" << requestId.toUri()
                                                  << " requesterName=" << requesterName.toUri()
                                                  << " providerName=" << providerName.toUri()
                                                  << " serviceName=" << serviceName.toUri()
                                                  << " selectionName=" << subscriptionName.toUri()
                                                  << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                                        decryptEndUs - decryptStartUs : 0)
                                                  << " error=" << error);
                                        OnServiceSelectionMessageDecryptionErrorCallback(
                                            requesterName, providerName, serviceName,
                                            requestId, error);
                                    });

            }else{
                const auto decryptStartUs = nowMicroseconds();
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_START timestamp_us="
                          << decryptStartUs
                          << " requestId=" << selectionV2->requestId.toUri()
                          << " requesterName=" << selectionV2->requesterName.toUri()
                          << " providerName=" << selectionV2->providerName.toUri()
                          << " serviceName=" << selectionV2->serviceName.toUri()
                          << " selectionName=" << subscription.name.toUri());
                activeNacConsumer().consume(subscription.name,
                                    [this, requesterName = selectionV2->requesterName,
                                     providerName = selectionV2->providerName,
                                     serviceName = selectionV2->serviceName,
                                     requestId = selectionV2->requestId,
                                     subscriptionName = ndn::Name(subscription.name),
                                     decryptStartUs](const ndn::Buffer& buffer) {
                                        const auto decryptEndUs = nowMicroseconds();
                                        logCryptoDiag("provider", "selection",
                                                      "decrypt", "normal", "success",
                                                      decryptStartUs, decryptEndUs,
                                                      subscriptionName, buffer.size());
                                        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_DONE timestamp_us="
                                                  << decryptEndUs
                                                  << " requestId=" << requestId.toUri()
                                                  << " requesterName=" << requesterName.toUri()
                                                  << " providerName=" << providerName.toUri()
                                                  << " serviceName=" << serviceName.toUri()
                                                  << " selectionName=" << subscriptionName.toUri()
                                                  << " payloadBytes=" << buffer.size()
                                                  << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                                        decryptEndUs - decryptStartUs : 0));
                                        OnServiceSelectionMessageDecryptionSuccessCallbackV2(
                                            requesterName, providerName, serviceName,
                                            requestId, buffer);
                                    },
                                    [this, requesterName = selectionV2->requesterName,
                                     providerName = selectionV2->providerName,
                                     serviceName = selectionV2->serviceName,
                                     requestId = selectionV2->requestId,
                                     subscriptionName = ndn::Name(subscription.name),
                                     decryptStartUs](const std::string& error) {
                                        const auto decryptEndUs = nowMicroseconds();
                                        logCryptoDiag("provider", "selection",
                                                      "decrypt", "normal", "failure",
                                                      decryptStartUs, decryptEndUs,
                                                      subscriptionName, 0, error);
                                        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_DECRYPT_FAILED timestamp_us="
                                                  << decryptEndUs
                                                  << " requestId=" << requestId.toUri()
                                                  << " requesterName=" << requesterName.toUri()
                                                  << " providerName=" << providerName.toUri()
                                                  << " serviceName=" << serviceName.toUri()
                                                  << " selectionName=" << subscriptionName.toUri()
                                                  << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                                        decryptEndUs - decryptStartUs : 0)
                                                  << " error=" << error);
                                        OnServiceSelectionMessageDecryptionErrorCallback(
                                            requesterName, providerName, serviceName,
                                            requestId, error);
                                    });
            }
            return;
        }

        NDN_LOG_WARN("Reject non-V2 service selection name: " << subscription.name);

    }

    ndn::Name ServiceProvider::getName()
    {
        return identity;
    }

    ndn::Name ServiceProvider::getSigningKeyName() const
    {
        return signingCert.getKeyName();
    }

    ndn::Name ServiceProvider::getSigningCertificateName() const
    {
        return signingCert.getName();
    }

    std::shared_ptr<LiveStreamPublisher>
    ServiceProvider::createLiveStream(const LiveStreamDefinition& definition)
    {
        if (definition.provider != identity) {
            throw std::invalid_argument(
                "LiveStream Provider must match ServiceProvider identity");
        }
        auto publisher = std::make_shared<LiveStreamPublisher>(
            definition, m_face, m_keyChain, m_signingInfo);
        publisher->start();
        return publisher;
    }

    std::shared_ptr<StreamPublisher>
    ServiceProvider::createStream(const StreamConfig& config)
    {
        return StreamPublisher::create(
          config, identity,
          [this] (const LiveStreamDefinition& definition) {
              if (definition.provider != identity) {
                  throw std::invalid_argument(
                      "Stream Provider must match ServiceProvider identity");
              }
              // The predictive high-level facade owns route registration.
              // createLiveStream() intentionally retains the Mapping-first
              // low-level lifecycle for internal/legacy Core callers.
              return std::make_shared<LiveStreamPublisher>(
                  definition, m_face, m_keyChain, m_signingInfo);
          });
    }

    void ServiceProvider::fetchPermissionsFromController(const ndn::Name& controllerPrefix)
    {
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            m_controllerPrefix = controllerPrefix;
        }
        fetchPolicyManifestFromController(controllerPrefix);

        ndn::Name interestName(controllerPrefix);
        interestName.append(ndn::Name("/NDNSF/PERMISSIONS/PROVIDER"));
        interestName.append(identity);

        ndn::Interest interest(interestName);
        interest.setCanBePrefix(true);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::milliseconds(permissionFetchLifetimeMs()));

        NDN_LOG_INFO("Fetch provider permissions: " << interestName
                     << " attempt=1/" << permissionFetchMaxAttempts());
        m_face.expressInterest(
            interest,
            std::bind(&ServiceProvider::onPermissionResponseData, this, _1, _2),
            [this](const ndn::Interest& interest, const ndn::lp::Nack&) {
                onPermissionResponseTimeout(interest, 1);
            },
            [this](const ndn::Interest& interest) {
                onPermissionResponseTimeout(interest, 1);
            });
    }

    void ServiceProvider::refreshProviderPermissionsAfterAdvance(
        const ndn::Name& serviceName)
    {
        // A withdrawn grant (identity or service-scoped revocation) is only
        // discovered through the scheduled PolicyStatus refresh: the signed
        // status for a served service advances to a newer ControllerVersion,
        // but the ProviderPermission table would otherwise keep the
        // bootstrap-era grants forever and OnRequest would keep serving a
        // revoked authorization.  Re-fetch the identity-wide permission
        // renewal whenever an already-installed status advances; the
        // controller omits withdrawn grants (an empty renewal for a fully
        // revoked identity) and applyPermissionResponse installs the
        // replacement table.
        if (m_isLocalMock) {
            return;
        }
        ndn::Name controllerPrefix;
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            controllerPrefix = m_controllerPrefix;
        }
        if (controllerPrefix.empty()) {
            return;
        }
        NDN_LOG_INFO("NDNSF_PROVIDER_PERMISSION_REVALIDATION role=provider"
                     << " serviceName=" << serviceName.toUri());
        fetchPermissionsFromController(controllerPrefix);
    }

    void ServiceProvider::applyPermissionResponse(const PermissionResponse& response)
    {
        if (response.getPermissionKind() != tlv::ProviderPermission) {
            NDN_LOG_ERROR("Ignoring non-provider PermissionResponse for "
                          << response.getTargetIdentity());
            return;
        }
        // PermissionResponse carries a controller-version hint, but it is not
        // authoritative for any single service.  A single response can contain
        // records from services at different policy epochs; rejecting it
        // against the process-wide version would let one service suppress
        // another service's renewal.  Exact per-service PolicyStatus validation
        // below owns freshness and monotonicity.

        // Keep the old service scopes available for status refresh when a
        // revoked Provider receives an empty permission renewal.  Without
        // this, replacing the table first would suppress the only fetch that
        // can install the new ControllerVersion and revocation state.
        std::set<std::string> previouslyKnownServices;
        std::set<std::string> previousRecords;
        for (const auto& existing : m_authorizations.snapshot()) {
            if (existing.permissionKind == tlv::ProviderPermission &&
                !existing.serviceName.empty()) {
                previouslyKnownServices.insert(existing.serviceName);
                previousRecords.insert(existing.providerServiceName + "\x1f" +
                                       existing.serviceName);
            }
        }

        std::vector<ServiceAuthorizationRecord> records;
        records.reserve(response.getEntries().size());
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
                tlv::ProviderPermission, response.getPolicyEpoch()});
        }
        if (!m_authorizations.replacePermissions(tlv::ProviderPermission,
                                                 response.getPolicyEpoch(),
                                                 records)) {
            NDN_LOG_ERROR("Rejected invalid or stale provider PermissionResponse epoch="
                          << response.getPolicyEpoch());
            return;
        }
        // Permission responses are scoped to this identity, but their
        // records are service-level.  Detect an actual table change before
        // requesting a replacement DKEY; a newer ControllerVersion caused by
        // another identity must not make this identity refetch its DKEY.
        std::set<std::string> currentRecords;
        for (const auto& record : records) {
            currentRecords.insert(record.providerServiceName + "\x1f" +
                                  record.serviceName);
        }
        if (previousRecords != currentRecords) {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            for (const auto& record : records)
                m_nacDkeyRefreshPendingServices.insert(record.serviceName);
            for (const auto& existing : previouslyKnownServices)
                m_nacDkeyRefreshPendingServices.insert(existing);
            NDN_LOG_INFO("NDNSF_NAC_DKEY_REFRESH_PENDING role=provider reason=grant-only");
        }
        m_currentPolicyEpoch = response.getPolicyEpoch();
        if (response.hasControllerVersion()) {
            adoptControllerVersion(response.getControllerVersion());
        }
        for (const auto& record : records) {
            NDN_LOG_WARN("Installed provider permission provider="
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
        scheduleSvsReinitializationAfterPermission();
    }

    bool ServiceProvider::hasProviderPermissionForService(const ndn::Name& serviceName) const
    {
        return hasProviderPermission(identity, serviceName, m_authorizations);
    }

    size_t ServiceProvider::getCurrentPolicyEpoch() const
    {
        return m_currentPolicyEpoch;
    }

    size_t ServiceProvider::getCurrentPolicyEpoch(const ndn::Name& serviceName) const
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

    bool ServiceProvider::installControllerStatus(const PolicyStatusData& status,
                                                  bool controllerSignatureValid)
    {
        const auto serviceKey = status.getServiceName().toUri();
        if (serviceKey.empty()) {
            return false;
        }
        bool accepted = false;
        bool versionChanged = false;
        // An advance beyond a status this process already installed (the
        // scheduled-refresh revocation-discovery path) must re-fetch the
        // ProviderPermission renewal.  A cold-bootstrap first install is not
        // an advance: the enclosing bootstrap already fetched permissions
        // before any serving began.
        bool advancedFromInstalledStatus = false;
        bool abeGenerationChanged = true;
        bool grantOnlyDkeyRefresh = false;
        {
            std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
            // Stage the revocation state and refresh coordinator together so a
            // rejected coordinator status cannot partially advance authority.
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
                    status, nowMilliseconds(), controllerSignatureValid)) {
                return false;
            }

            m_revocationStates[serviceKey] = std::move(*stagedState);
            m_policyRefreshCoordinators.insert_or_assign(
                serviceKey, std::move(*stagedCoordinator));
            if (!m_controllerVersion ||
                status.getControllerVersion().compare(*m_controllerVersion) > 0) {
                m_controllerVersion = status.getControllerVersion();
            }
            m_currentPolicyEpoch = std::max(
                m_currentPolicyEpoch,
                static_cast<size_t>(status.getControllerVersion().controllerEpoch));
            accepted = true;
            versionChanged = !previousVersion ||
                status.getControllerVersion().compare(*previousVersion) > 0;
            advancedFromInstalledStatus = versionChanged &&
                previousVersion.has_value();
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
            const auto pendingDkeyRefresh =
                m_nacDkeyRefreshPendingServices.find(serviceKey);
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
                    m_nacDkeyRefreshPendingServices.erase(serviceKey);
                else
                    m_nacDkeyRefreshPendingServices.erase(pendingDkeyRefresh);
            }
        }
        if (accepted) {
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
            if (advancedFromInstalledStatus) {
                // A newer ControllerVersion can withdraw this provider's own
                // grant (identity or service-scoped revocation); the renewal
                // fetch installs the replacement table so OnRequest begins
                // refusing the withdrawn service.
                refreshProviderPermissionsAfterAdvance(status.getServiceName());
            }
            scheduleControllerStatusRefresh(status.getServiceName(), status);
        }
        return accepted;
    }

    void ServiceProvider::persistAcceptedControllerStatus(
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

    void ServiceProvider::restorePersistedRuntimeStatuses()
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

    void ServiceProvider::invalidateControllerScopedCaches(
        const ndn::Name& serviceName,
        const ControllerVersion& version,
        bool abeGenerationChanged,
        const PolicyStatusData* status,
        bool grantOnlyDkeyRefresh)
    {
        if (serviceName.empty() || !version.isValid()) {
            return;
        }

        const auto serviceUri = serviceName.toUri();
        const auto hybridKeys = m_hybridMessageCrypto.invalidateService(serviceName);
        // NAC-ABE's DKEY and encrypted-CK caches are scoped to the local
        // identity/authority rather than to an NDNSF service name.  Clear the
        // external cache whenever a newer controller status is committed so
        // old authority material cannot be reused for a future request.  The
        // NDNSF caches below remain service-selective.
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
        // clearCache() removes the identity DKEY in addition to encrypted
        // service keys. A grant-only wave refreshes only the target DKEY; a
        // generation wave refetches everything (shared helper keeps the
        // equal-version install path from losing the refresh).
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
        auto keyContainsService = [&serviceName](const ndn::Name& key) {
            if (key.size() <= serviceName.size()) {
                return false;
            }
            // Pending provider keys are requester + service + request-id.
            // Search component-wise so identities containing a service-like
            // prefix do not become a cache invalidation boundary by accident.
            for (size_t offset = 1;
                 offset + serviceName.size() < key.size(); ++offset) {
                if (key.getSubName(offset, serviceName.size()) == serviceName) {
                    return true;
                }
            }
            return false;
        };

        std::vector<ndn::Name> affectedPendingKeys;
        {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            auto collect = [&affectedPendingKeys, &keyContainsService](
                               const auto& map) {
                for (const auto& item : map) {
                    if (keyContainsService(item.first)) {
                        affectedPendingKeys.push_back(item.first);
                    }
                }
            };
            collect(pendingRequests);
            collect(pendingProviderTokens);
            collect(m_pendingRequestTokenHashes);
            collect(m_streamBindings);
            for (const auto& pendingKey : m_recentProviderRequests) {
                if (keyContainsService(pendingKey)) {
                    affectedPendingKeys.push_back(pendingKey);
                }
            }
            for (const auto& pendingKey : m_selectedProviderRequests) {
                if (keyContainsService(pendingKey)) {
                    affectedPendingKeys.push_back(pendingKey);
                }
            }
        }
        std::sort(affectedPendingKeys.begin(), affectedPendingKeys.end());
        affectedPendingKeys.erase(std::unique(affectedPendingKeys.begin(),
                                              affectedPendingKeys.end()),
                                  affectedPendingKeys.end());
        for (const auto& pendingKey : affectedPendingKeys) {
            cleanupPendingRequestState(pendingKey, true);
        }

        size_t targetedTokens = 0;
        {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            for (auto it = m_targetedProviderTokens.begin();
                 it != m_targetedProviderTokens.end();) {
                if (it->second.serviceName == serviceName) {
                    ++targetedTokens;
                    it = m_targetedProviderTokens.erase(it);
                }
                else {
                    ++it;
                }
            }

            for (auto it = m_streamBindings.begin();
                 it != m_streamBindings.end();) {
                if (it->second.serviceName == serviceName) {
                    it = m_streamBindings.erase(it);
                }
                else {
                    ++it;
                }
            }
            for (auto it = m_streamPublishers.begin();
                 it != m_streamPublishers.end();) {
                if (keyContainsService(it->first)) {
                    it = m_streamPublishers.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        std::vector<ndn::Name> collaborationRequests;
        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
            for (const auto& item : m_collaborationServiceNamesByRequest) {
                if (item.second == serviceName) {
                    collaborationRequests.push_back(item.first);
                }
            }
            for (const auto& requestId : collaborationRequests) {
                m_collaborationServiceNamesByRequest.erase(requestId);
                // spec182: the collaboration registration binding shares the
                // request-identity lifetime of the names map.
                m_collaborationRegistrationStates.erase(requestId);
                m_collaborationDataByRequest.erase(requestId);
                m_collaborationScopeKeysByRequest.erase(requestId);
                m_collaborationScopeKeyDataNamesByRequest.erase(requestId);
                m_pendingEncryptedCollaborationData.erase(requestId);
            }
        }

        for (auto it = m_preparedDeployments.begin();
             it != m_preparedDeployments.end();) {
            if (it->second.serviceName == serviceName) {
                it = m_preparedDeployments.erase(it);
            }
            else {
                ++it;
            }
        }

        NDN_LOG_WARN("NDNSF_CONTROLLER_CACHE_INVALIDATED role=provider"
                     << " serviceName=" << serviceUri
                     << " generation=" << version.controllerGenerationTimestamp
                     << " epoch=" << version.controllerEpoch
                     << " pendingRequests=" << affectedPendingKeys.size()
                     << " hybridKeys=" << hybridKeys
                     << " targetedTokens=" << targetedTokens
                     << " collaborationRequests=" << collaborationRequests.size()
                     << " abeGenerationChanged=" << (abeGenerationChanged ? "true" : "false")
                     << " nacAbeCaches="
                     << (abeGenerationChanged ? "authority-shared-cleared" :
                         "retained-grant-only-same-generation")
                     << " runtimeFamilies=targeted-token,selection-binding,nonce,in-flight-request"
                     << " stateFamilies=abe,message-key,replay");
    }

    void ServiceProvider::refreshNacDkeyForControllerStatus(
        const ndn::Name& serviceName, const ControllerVersion& version,
        bool abeGenerationChanged, bool grantOnlyDkeyRefresh)
    {
        if (serviceName.empty() || !version.isValid()) {
            return;
        }
        const auto serviceUri = serviceName.toUri();
        // clearCache() removes the identity DKEY in addition to encrypted
        // service keys. Immediately schedule a fresh DKEY fetch after a
        // validated generation change. For grant-only changes the old key
        // remains active until the complete replacement is installed.
        // Consumer coalescing handles a constructor-time fetch still in
        // flight. A LocalMock without its fixture-owned Consumer defers to
        // its explicit bootstrap path.
        auto& nacConsumer = activeNacConsumer();
        const bool canRefreshDkey = !m_isLocalMock || m_testNacConsumer != nullptr;
        if ((abeGenerationChanged || grantOnlyDkeyRefresh) && canRefreshDkey) {
            if (grantOnlyDkeyRefresh)
                nacConsumer.refreshDecryptionKey();
            else
                nacConsumer.obtainDecryptionKey();
            NDN_LOG_INFO("NDNSF_NAC_DKEY_REFRESH_REQUESTED role=provider"
                         << " serviceName=" << serviceUri
                         << " epoch=" << version.controllerEpoch
                         << " reason=" << (abeGenerationChanged ?
                             "generation-change" : "grant-only"));
        }
        else if (abeGenerationChanged || grantOnlyDkeyRefresh) {
            NDN_LOG_INFO("NDNSF_NAC_DKEY_REFRESH_DEFERRED role=provider"
                         << " serviceName=" << serviceUri
                         << " epoch=" << version.controllerEpoch
                         << " reason=local-mock-bootstrap");
            scheduleDeferredDkeyRefreshRetry(serviceName);
        }
        else {
            NDN_LOG_DEBUG("NDNSF_NAC_DKEY_REFRESH_NOT_REQUIRED role=provider"
                         << " serviceName=" << serviceUri
                         << " epoch=" << version.controllerEpoch
                         << " reason=same-generation-no-grant");
        }
    }

    void ServiceProvider::scheduleControllerStatusRefresh(
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
                    NDN_LOG_INFO("NDNSF_POLICY_STATUS_REVALIDATION role=provider"
                                 << " serviceName=" << serviceName.toUri()
                                 << " generation=" << version.controllerGenerationTimestamp
                                 << " epoch=" << version.controllerEpoch);
                    // Under the knob the revalidation must be able to
                    // discover a newer controller epoch, so issue an
                    // unversioned must-be-fresh status fetch (the controller
                    // answers with its current status) instead of the
                    // exact-version confirmation fetch of the near-expiry
                    // schedule, which can never observe a version advance.
                    fetchPolicyStatusFromController(prefix, serviceName, 1,
                        revalidationPeriodMs > 0
                            ? std::optional<ControllerVersion>()
                            : std::optional<ControllerVersion>(version));
                }
            });
    }

    void ServiceProvider::scheduleDeferredDkeyRefreshRetry(const ndn::Name& serviceName)
    {
        const auto attempts = std::make_shared<size_t>(0);
        const auto retry = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weakRetry = retry;
        *retry = [this, serviceName, attempts, weakRetry] {
            if (*attempts >= 20) {
                NDN_LOG_WARN("NDNSF_NAC_DKEY_RETRY_EXHAUSTED role=provider"
                             << " serviceName=" << serviceName.toUri());
                return;
            }
            ++*attempts;
            if (activeNacConsumer().readyForDecryption())
                return;
            activeNacConsumer().obtainDecryptionKey();
            if (const auto self = weakRetry.lock()) {
                m_scheduler.schedule(ndn::time::milliseconds(250), *self);
            }
        };
        m_scheduler.schedule(ndn::time::milliseconds(250), *retry);
    }

    std::optional<ControllerVersion> ServiceProvider::getControllerVersion() const
    {
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        return m_controllerVersion;
    }

    std::optional<ControllerVersion>
    ServiceProvider::getControllerVersion(const ndn::Name& serviceName) const
    {
        if (serviceName.empty()) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        const auto it = m_revocationStates.find(serviceName.toUri());
        if (it != m_revocationStates.end() && it->second.hasCurrentStatus()) {
            return it->second.currentVersion();
        }
        if (m_controllerPrefix.empty()) {
            return m_controllerVersion;
        }
        return std::nullopt;
    }

    void ServiceProvider::adoptControllerVersion(const ControllerVersion& version)
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

    bool ServiceProvider::isAcceptablePolicyEpoch(size_t messageEpoch) const
    {
        return m_currentPolicyEpoch == 0 || messageEpoch == 0 ||
               messageEpoch == m_currentPolicyEpoch;
    }

    bool ServiceProvider::isAcceptablePolicyEpoch(const ndn::Name& serviceName,
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

    bool ServiceProvider::isAcceptableControllerVersion(
        const std::optional<ControllerVersion>& messageVersion) const
    {
        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        if (!m_controllerVersion) {
            // A configured Controller marks this as a protected runtime.  Do
            // not accept a missing message version before a signed status is
            // installed; controller-free LocalMock/unit callers keep the
            // compatibility behavior used by non-authority tests.
            return m_controllerPrefix.empty();
        }
        return messageVersion && *messageVersion == *m_controllerVersion;
    }

    bool ServiceProvider::isAcceptableControllerVersion(
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

    bool ServiceProvider::authorizeControllerTransition(
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
            makeProviderAuthorizationAttribute(serviceName)};

        std::lock_guard<std::mutex> lock(m_controllerVersionMutex);
        const auto it = m_revocationStates.find(serviceName.toUri());
        if (it == m_revocationStates.end()) {
            // Permissions alone are not an authority snapshot.  Fail closed
            // for configured Controller runtimes until this service has an
            // authenticated non-zero PolicyStatus.
            return m_controllerPrefix.empty();
        }
        const auto decision = it->second.authorize(subject, transition,
                                                   nowMilliseconds());
        if (!decision.allowed) {
            NDN_LOG_WARN("NDNSF_PROVIDER_REVOCATION_REJECT service="
                         << serviceName << " transition="
                         << static_cast<int>(transition)
                         << " reason=" << decision.reason);
        }
        return decision.allowed;
    }

    bool ServiceProvider::handlePermissionResponseData(const ndn::Data& data,
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

        if (response.getTargetIdentity() != identity.toUri()) {
            NDN_LOG_ERROR("Ignoring PermissionResponse for unexpected targetIdentity="
                          << response.getTargetIdentity()
                          << " expected=" << identity.toUri());
            return false;
        }

        if (response.getPermissionKind() != tlv::ProviderPermission) {
            NDN_LOG_ERROR("Ignoring non-provider PermissionResponse for "
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
                tlv::ProviderPermission, response.getPolicyEpoch()});
        }
        return permissionTable.replacePermissions(tlv::ProviderPermission,
                                                  response.getPolicyEpoch(),
                                                  records);
    }

    void ServiceProvider::OnServiceSelectionMessageDecryptionSuccessCallbackV2(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& msgId,
        const ndn::Buffer& buffer)
    {
        if (!providerName.equals(identity)) {
            NDN_LOG_WARN("Ignore V2 selection for non-local provider "
                         << providerName.toUri()
                         << " at " << identity.toUri());
            return;
        }

        auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());

        auto spanBuf = ndn::span<const uint8_t>(raw->data(), raw->size());
        auto [ok, block] = ndn::Block::fromBuffer(spanBuf);

        auto key = ndn::Name(requesterName.toUri())
                    .append(serviceName)
                    .append(msgId);
        auto clearSelectionDecryptInFlight = [&] {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            m_selectionDecryptsInFlight.erase(key);
        };

        if (!ok) {
            NDN_LOG_ERROR("Reject V2 selection with invalid wire block for "
                          << key.toUri());
            clearSelectionDecryptInFlight();
            return;
        }

        ServiceSelectionMessage message;
        if (!message.WireDecode(block)) {
            NDN_LOG_ERROR("Reject V2 selection with invalid message wire for "
                          << key.toUri());
            clearSelectionDecryptInFlight();
            return;
        }
        const auto messageVersion = message.hasControllerVersion() ?
            std::optional<ControllerVersion>(message.getControllerVersion()) :
            std::nullopt;
        // Do not make the first message carrying a newer ControllerVersion
        // disappear behind the old authorization state.  The hint is only a
        // trigger for an exact signed status fetch; it never changes authority
        // by itself, and the transition below still fails closed until that
        // status is installed.
        maybeRefreshControllerVersionHint(serviceName, messageVersion);
        if (!authorizeControllerTransition(serviceName,
                                           ProtectedTransition::SELECTION)) {
            NDN_LOG_ERROR("Reject Selection under revoked Controller status requestId="
                          << msgId.toUri());
            clearSelectionDecryptInFlight();
            return;
        }

        NDN_LOG_DEBUG("OnServiceSelectionMessageDecryptionSuccessCallbackV2: "
            << requesterName.toUri()
            << providerName.toUri()
            << serviceName.toUri()
            << msgId.toUri());
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_RECEIVED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << msgId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " requesterName=" << requesterName.toUri()
                  << " providerName=" << providerName.toUri());
        updateProviderRequestLifecycleState(
            msgId, serviceName,
            ProviderRequestLifecycleState::SELECTION_RECEIVED);

        const std::string selectionDigest = computeSelectionDigest(message);
        const auto opaqueParticipantIt =
            m_opaqueSelectionParticipants.find(serviceName);
        const bool hasOpaqueParticipant =
            opaqueParticipantIt != m_opaqueSelectionParticipants.end();
        const ndn::Buffer sharedAssignmentPayload = message.getAssignmentPayload();
        ndn::Buffer effectiveAssignmentPayload = sharedAssignmentPayload;
        std::string derivedRoleProviderFields;
        std::string receivedProviderToken = message.getProviderToken();
        std::string receivedProviderTokenProofHash;
        const auto rolesFromAssignmentPayload = [](const ndn::Buffer& payload) {
            std::vector<std::string> roles;
            std::vector<ndn::Buffer> assignmentItems;
            try {
                assignmentItems = decodeOpaqueAssignmentSet(payload);
            }
            catch (const std::exception&) {
                return roles;
            }
            for (const auto& item : assignmentItems) {
                CollaborationAssignmentEnvelope envelope;
                try {
                    if (decodeCollaborationAssignmentEnvelope(item, envelope)) {
                        if (!envelope.role.empty()) {
                            roles.push_back(envelope.role);
                        }
                        continue;
                    }
                }
                catch (const std::exception&) {
                    // Selection metadata derivation is best-effort. The
                    // common assignment parser will reject malformed
                    // envelopes before execution.
                }
                const auto fields = parseSemicolonFields(item);
                const auto roleIt = fields.find("role");
                if (roleIt != fields.end() && !roleIt->second.empty()) {
                    roles.push_back(roleIt->second);
                }
            }
            return roles;
        };
        const auto hasStructuredAssignmentEnvelope =
            [](const ndn::Buffer& payload) {
                std::vector<ndn::Buffer> assignmentItems;
                try {
                    assignmentItems = decodeOpaqueAssignmentSet(payload);
                }
                catch (const std::exception&) {
                    return false;
                }
                for (const auto& item : assignmentItems) {
                    CollaborationAssignmentEnvelope envelope;
                    try {
                        if (decodeCollaborationAssignmentEnvelope(item, envelope)) {
                            return true;
                        }
                    }
                    catch (const std::exception&) {
                        // Ignore malformed items here; the main parser owns
                        // the fail-closed validation path.
                    }
                }
                return false;
            };
        const auto isStructuredAssignmentPayload =
            [](const ndn::Buffer& payload) {
                std::vector<ndn::Buffer> assignmentItems;
                try {
                    assignmentItems = decodeOpaqueAssignmentSet(payload);
                }
                catch (const std::exception&) {
                    return false;
                }
                if (assignmentItems.empty()) {
                    return false;
                }
                for (const auto& item : assignmentItems) {
                    CollaborationAssignmentEnvelope envelope;
                    try {
                        if (!decodeCollaborationAssignmentEnvelope(item,
                                                                     envelope)) {
                            return false;
                        }
                    }
                    catch (const std::exception&) {
                        return false;
                    }
                }
                return true;
            };
        // One or more CollaborationAssignmentEnvelope values are already
        // canonical binary metadata. Do not route a single-envelope Provider
        // projection through the legacy semicolon-field merge below either:
        // appending text would corrupt the outer TLV and make parsing fall
        // back to the service name instead of the first role.
        bool structuredAssignmentPayload = false;
        if (!message.getProviderEntries().empty()) {
            bool hasLocalProviderEntry = false;
            for (const auto& entry : message.getProviderEntries()) {
                const auto entryRoles =
                    rolesFromAssignmentPayload(entry.assignmentPayload);
                if ((!hasOpaqueParticipant ||
                     hasStructuredAssignmentEnvelope(entry.assignmentPayload))) {
                    for (const auto& entryRole : entryRoles) {
                        derivedRoleProviderFields +=
                            "roleProvider." + entryRole + "=" +
                            entry.providerName.toUri() + ";";
                    }
                }
                if (!entry.providerName.equals(providerName)) {
                    continue;
                }
                hasLocalProviderEntry = true;
                receivedProviderTokenProofHash = entry.providerTokenHash;
                effectiveAssignmentPayload = entry.assignmentPayload;
                structuredAssignmentPayload =
                    isStructuredAssignmentPayload(effectiveAssignmentPayload);
                break;
            }
            if (!hasLocalProviderEntry) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=COMPACT_SELECTION_NOT_FOR_PROVIDER timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << msgId.toUri()
                          << " serviceName=" << serviceName.toUri()
                          << " requesterName=" << requesterName.toUri()
                          << " providerName=" << providerName.toUri());
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName,
                                               serviceName,
                                               msgId,
                                               "compact selection has no local provider entry");
                clearSelectionDecryptInFlight();
                return;
            }
        }
        if (!structuredAssignmentPayload && !hasOpaqueParticipant &&
            !sharedAssignmentPayload.empty() &&
            !message.getProviderEntries().empty()) {
            CollaborationAssignmentEnvelope envelope;
            if (decodeCollaborationAssignmentEnvelope(
                    effectiveAssignmentPayload, envelope)) {
                const std::string sharedAssignmentText(
                    reinterpret_cast<const char*>(sharedAssignmentPayload.data()),
                    sharedAssignmentPayload.size());
                const std::string opaqueText(
                    reinterpret_cast<const char*>(envelope.opaquePayload.data()),
                    envelope.opaquePayload.size());
                std::string mergedOpaque = opaqueText;
                if (!sharedAssignmentText.empty()) {
                    if (!mergedOpaque.empty() && mergedOpaque.back() != ';') {
                        mergedOpaque.push_back(';');
                    }
                    mergedOpaque += sharedAssignmentText;
                }
                envelope.opaquePayload = ndn::Buffer(
                    reinterpret_cast<const uint8_t*>(mergedOpaque.data()),
                    mergedOpaque.size());
                effectiveAssignmentPayload =
                    encodeCollaborationAssignmentEnvelope(envelope);
            }
            else {
                const std::string sharedAssignmentText(
                    reinterpret_cast<const char*>(sharedAssignmentPayload.data()),
                    sharedAssignmentPayload.size());
                const std::string entryAssignmentText(
                    reinterpret_cast<const char*>(effectiveAssignmentPayload.data()),
                    effectiveAssignmentPayload.size());
                const std::string mergedAssignment =
                    sharedAssignmentText + entryAssignmentText;
                effectiveAssignmentPayload = ndn::Buffer(
                    reinterpret_cast<const uint8_t*>(mergedAssignment.data()),
                    mergedAssignment.size());
            }
        }
        if ((hasOpaqueParticipant || structuredAssignmentPayload) &&
            !sharedAssignmentPayload.empty() &&
            !message.getProviderEntries().empty()) {
            const auto sharedFields =
                parseSemicolonFields(sharedAssignmentPayload);
            const bool onlyBoundedSharedMetadata =
                !sharedFields.empty() &&
                std::all_of(
                    sharedFields.begin(), sharedFields.end(),
                    [](const auto& field) {
                        static const std::string scopePrefix = "scopeKeyData.";
                        static const std::string roleProviderPrefix =
                            "roleProvider.";
                        const bool isScopeKey =
                            field.first.rfind(scopePrefix, 0) == 0 &&
                            !field.first.substr(scopePrefix.size()).empty();
                        const bool isRoleProvider =
                            field.first.rfind(roleProviderPrefix, 0) == 0 &&
                            !field.first.substr(roleProviderPrefix.size()).empty();
                        return (isScopeKey || isRoleProvider) &&
                               !field.second.empty();
                    });
            if (!onlyBoundedSharedMetadata) {
                updateSelectionExecutionStatus(
                    selectionDigest, SelectionExecutionState::Rejected,
                    providerName, serviceName, msgId,
                    "structured Selection shared metadata is not a bounded "
                    "scope-key reference set");
                clearSelectionDecryptInFlight();
                return;
            }
        }
        if (!structuredAssignmentPayload && !hasOpaqueParticipant &&
            !derivedRoleProviderFields.empty()) {
            CollaborationAssignmentEnvelope envelope;
            if (decodeCollaborationAssignmentEnvelope(
                    effectiveAssignmentPayload, envelope)) {
                std::string mergedOpaque(
                    reinterpret_cast<const char*>(envelope.opaquePayload.data()),
                    envelope.opaquePayload.size());
                if (!mergedOpaque.empty() && mergedOpaque.back() != ';') {
                    mergedOpaque.push_back(';');
                }
                mergedOpaque += derivedRoleProviderFields;
                envelope.opaquePayload = ndn::Buffer(
                    reinterpret_cast<const uint8_t*>(mergedOpaque.data()),
                    mergedOpaque.size());
                effectiveAssignmentPayload =
                    encodeCollaborationAssignmentEnvelope(envelope);
            }
            else {
                const std::string assignmentText(
                    reinterpret_cast<const char*>(effectiveAssignmentPayload.data()),
                    effectiveAssignmentPayload.size());
                if (assignmentText.find("roleProvider.") == std::string::npos) {
                    std::string mergedAssignment =
                        assignmentText + derivedRoleProviderFields;
                    effectiveAssignmentPayload = ndn::Buffer(
                        reinterpret_cast<const uint8_t*>(mergedAssignment.data()),
                        mergedAssignment.size());
                }
            }
        }
        // Deferred collaboration keeps generic role/provisioning metadata in a
        // Core-owned envelope, but an opaque Selection participant owns only
        // the exact application assignment bytes inside that envelope.  Do
        // not make external participants parse a Core wire wrapper.
        ndn::Buffer opaqueParticipantPayload = effectiveAssignmentPayload;
        if (hasOpaqueParticipant && !effectiveAssignmentPayload.empty()) {
            CollaborationAssignmentEnvelope envelope;
            try {
                if (decodeCollaborationAssignmentEnvelope(
                        effectiveAssignmentPayload, envelope)) {
                    opaqueParticipantPayload = std::move(envelope.opaquePayload);
                }
            }
            catch (const std::exception& error) {
                updateSelectionExecutionStatus(
                    selectionDigest, SelectionExecutionState::Rejected,
                    providerName, serviceName, msgId,
                    std::string("invalid opaque collaboration assignment envelope: ") +
                        error.what());
                clearSelectionDecryptInFlight();
                return;
            }
        }
        updateSelectionExecutionStatus(selectionDigest,
                                       SelectionExecutionState::Received,
                                       providerName,
                                       serviceName,
                                       msgId,
                                       "selection received");
        if (!isAcceptablePolicyEpoch(serviceName, message.getPolicyEpoch())) {
            NDN_LOG_ERROR("Reject V2 selection with stale policy epoch for "
                          << msgId.toUri()
                          << " receivedEpoch=" << message.getPolicyEpoch()
                          << " currentEpoch=" << m_currentPolicyEpoch);
            updateSelectionExecutionStatus(selectionDigest,
                                           SelectionExecutionState::Rejected,
                                           providerName,
                                           serviceName,
                                           msgId,
                                           "stale policy epoch");
            clearSelectionDecryptInFlight();
            return;
        }
        if (!isAcceptableControllerVersion(serviceName, messageVersion)) {
            NDN_LOG_ERROR("Reject V2 selection with stale ControllerVersion for "
                          << msgId.toUri());
            updateSelectionExecutionStatus(
                selectionDigest, SelectionExecutionState::Rejected,
                providerName, serviceName, msgId,
                "stale controller version");
            clearSelectionDecryptInFlight();
            return;
        }

        bool hasR1Decision = false;
        bool r1NotSelected = false;
        bool requestScopedSelection = false;
        std::string r1ReservationId;
        std::string r1DecisionDigest;
        uint64_t r1TombstoneRetainUntilMs = 0;
        ndn::Buffer r1ReceiptWire;
        if (message.hasSelectionDecision()) {
            const auto& decision = message.getSelectionDecision();
            const auto validBinding =
                decision.hasField("decision") &&
                decision.hasField("requester") &&
                decision.getField("requester") == requesterName.toUri() &&
                decision.hasField("requestId") &&
                decision.getField("requestId") == msgId.toUri() &&
                decision.hasField("attempt") &&
                decision.hasField("targetProvider") &&
                decision.getField("targetProvider") == providerName.toUri() &&
                decision.hasField("reservationId") &&
                decision.hasField("reservationDigest");
            if (!validBinding) {
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName, serviceName, msgId,
                                               "R1 SelectionDecision binding mismatch");
                clearSelectionDecryptInFlight();
                return;
            }
            const auto decisionValue = decision.getField("decision");
            if (decisionValue != "SELECTED" && decisionValue != "NOT_SELECTED") {
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName, serviceName, msgId,
                                               "unknown R1 SelectionDecision");
                clearSelectionDecryptInFlight();
                return;
            }
            hasR1Decision = true;
            r1NotSelected = decisionValue == "NOT_SELECTED";
            r1ReservationId = decision.getField("reservationId");
            r1DecisionDigest = decision.computeDigest();
        }

        if (m_timelineTrace) {
            logTimelineTrace("provider", "provider_token_validate_start", msgId,
                             {{"serviceName", serviceName.toUri()}});
        }
        RequestMessage selectedRequest;
        const std::string providerTokenHash =
            m_useTokens ? (!receivedProviderTokenProofHash.empty() ?
                           receivedProviderTokenProofHash :
                           replayTokenHash("SELECTION", requesterName,
                                           serviceName, receivedProviderToken)) : "";
        std::optional<GenericCommittedSelectionView> opaqueCommitted;
        std::shared_ptr<OpaqueSelectionParticipant> committedParticipant;
        bool opaqueReplay = false;
        {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            if (hasR1Decision) {
                const auto accepted =
                    m_r1AcceptedSelectionDecisions.find(r1ReservationId);
                if (accepted != m_r1AcceptedSelectionDecisions.end()) {
                    const bool exactDuplicate =
                        accepted->second.decisionDigest == r1DecisionDigest &&
                        accepted->second.providerTokenHash == providerTokenHash &&
                        accepted->second.decision ==
                            (r1NotSelected ? "NOT_SELECTED" : "SELECTED");
                    updateSelectionExecutionStatus(
                        selectionDigest,
                        exactDuplicate ? SelectionExecutionState::Completed :
                                         SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        exactDuplicate ? "duplicate immutable R1 decision" :
                                         "conflicting immutable R1 decision");
                    if (exactDuplicate) {
                        m_selectionExecutionStatuses[selectionDigest].decisionReceipt =
                            accepted->second.receiptWire;
                    }
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
            }
            if (hasOpaqueParticipant && m_genericSelectionTxnStore) {
                const auto transactionId =
                    GenericSelectionTxnStore::digest({
                        reinterpret_cast<const uint8_t*>(
                            selectionDigest.data()),
                        selectionDigest.size()});
                const auto committed =
                    m_genericSelectionTxnStore->findCommitted(transactionId);
                const auto payloadDigest =
                    GenericSelectionTxnStore::digest({
                        opaqueParticipantPayload.data(),
                        opaqueParticipantPayload.size()});
                if (committed &&
                    committed->selectionIdentity == selectionDigest &&
                    committed->selectionPayloadDigest == payloadDigest &&
                    committed->providerIdentity.equals(providerName) &&
                    committed->serviceName.equals(serviceName) &&
                    committed->requestId.equals(msgId) &&
                    committed->attempt == message.getAttempt()) {
                    opaqueCommitted = committed;
                    committedParticipant = opaqueParticipantIt->second;
                    opaqueReplay = true;
                    m_selectionExecutionStatuses[
                        selectionDigest].decisionReceipt =
                            committed->acceptancePayload;
                    m_selectionDecryptsInFlight.erase(key);
                    goto opaque_selection_committed;
                }
            }
            if (m_selectedProviderRequests.find(key) !=
                    m_selectedProviderRequests.end() ||
                (!providerTokenHash.empty() &&
                     m_consumedProviderTokenHashes.find(providerTokenHash) !=
                         m_consumedProviderTokenHashes.end())) {
                NDN_LOG_DEBUG("Ignore replayed V2 selection for " << key.toUri());
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName,
                                               serviceName,
                                               msgId,
                                               "replayed selection or provider token");
                m_selectionDecryptsInFlight.erase(key);
                return;
            }
            auto it = pendingRequests.find(key);
            if (it == pendingRequests.end()) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_NO_PENDING timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << msgId.toUri()
                          << " serviceName=" << serviceName.toUri()
                          << " requesterName=" << requesterName.toUri()
                          << " providerName=" << providerName.toUri()
                          << " pendingKey=" << key.toUri());
                NDN_LOG_INFO("No pending V2 request for " << key.toUri());
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Unknown,
                                               providerName,
                                               serviceName,
                                               msgId,
                                               "no pending request for selection");
                m_selectionDecryptsInFlight.erase(key);
                return;
            }

            auto providerTokenIt = pendingProviderTokens.find(key);
            bool providerTokenAccepted = true;
            if (m_useTokens) {
                providerTokenAccepted = false;
                if (providerTokenIt != pendingProviderTokens.end()) {
                    if (!receivedProviderToken.empty() &&
                        receivedProviderToken == providerTokenIt->second) {
                        providerTokenAccepted = true;
                    }
                    else if (!receivedProviderTokenProofHash.empty()) {
                        const auto expectedProofHash =
                            computeSelectionProviderTokenProofHash(requesterName,
                                                                   providerName,
                                                                   serviceName,
                                                                   providerTokenIt->second);
                        providerTokenAccepted =
                            receivedProviderTokenProofHash == expectedProofHash;
                    }
                }
            }
            if (!providerTokenAccepted) {
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=SELECTION_REJECTED_PROVIDER_TOKEN timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << msgId.toUri()
                          << " serviceName=" << serviceName.toUri()
                          << " requesterName=" << requesterName.toUri()
                          << " providerName=" << providerName.toUri()
                          << " pendingKey=" << key.toUri()
                          << " expectedTokenPresent="
                          << (providerTokenIt != pendingProviderTokens.end())
                          << " receivedTokenPresent="
                          << (!receivedProviderToken.empty() ||
                              !receivedProviderTokenProofHash.empty()));
                NDN_LOG_ERROR("Reject V2 selection with mismatched ProviderToken for "
                              << key.toUri());
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName,
                                               serviceName,
                                               msgId,
                                               "provider token mismatch");
                m_selectionDecryptsInFlight.erase(key);
                return;
            }
            if (hasR1Decision) {
                const auto lease = pendingReservationLeases.find(key);
                const bool reservationMatches =
                    lease != pendingReservationLeases.end() &&
                    lease->second.hasField("reservationId") &&
                    lease->second.getField("reservationId") == r1ReservationId &&
                    lease->second.computeDigest() ==
                        message.getSelectionDecision().getField("reservationDigest") &&
                    (!lease->second.hasField("attempt") ||
                     lease->second.getField("attempt") ==
                         message.getSelectionDecision().getField("attempt")) &&
                    (!lease->second.hasField("providerBootEpoch") ||
                     (message.getSelectionDecision().hasField("providerBootEpoch") &&
                      lease->second.getField("providerBootEpoch") ==
                          message.getSelectionDecision().getField("providerBootEpoch")));
                if (!reservationMatches) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        "R1 reservation or provider boot binding mismatch");
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
            }
            selectedRequest = *(it->second);
            requestScopedSelection =
                selectedRequest.hasRequestCapabilities() &&
                selectedRequest.getRequestCapabilities().hasField(
                    "RequestScopedConfidentialityV1") &&
                selectedRequest.getRequestCapabilities().getField(
                    "RequestScopedConfidentialityV1") == "required";
            if (requestScopedSelection && (hasOpaqueParticipant || hasR1Decision)) {
                updateSelectionExecutionStatus(
                    selectionDigest, SelectionExecutionState::Rejected,
                    providerName, serviceName, msgId,
                    "request-scoped confidentiality does not support opaque/R1 Selection");
                m_selectionDecryptsInFlight.erase(key);
                return;
            }
            if (hasOpaqueParticipant) {
                if (!m_genericSelectionTxnStore) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        "opaque Selection transaction store unavailable");
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
                std::chrono::steady_clock::time_point localDeadline;
                uint64_t expiresAtUnixMs = 0;
                {
                    std::lock_guard<std::mutex> deadlineLock(
                        m_pendingCleanupDeadlineMutex);
                    const auto deadline =
                        m_pendingCleanupDeadlines.find(key);
                    const auto expiry =
                        m_pendingCleanupExpiryUnixMs.find(key);
                    if (deadline == m_pendingCleanupDeadlines.end() ||
                        expiry == m_pendingCleanupExpiryUnixMs.end()) {
                        updateSelectionExecutionStatus(
                            selectionDigest,
                            SelectionExecutionState::Rejected,
                            providerName, serviceName, msgId,
                            "opaque Selection has no original deadline");
                        m_selectionDecryptsInFlight.erase(key);
                        return;
                    }
                    localDeadline = deadline->second;
                    expiresAtUnixMs = expiry->second;
                }
                AuthenticatedSelectionContext transactionContext;
                transactionContext.transactionId =
                    GenericSelectionTxnStore::digest({
                        reinterpret_cast<const uint8_t*>(
                            selectionDigest.data()),
                        selectionDigest.size()});
                transactionContext.serviceName = serviceName;
                transactionContext.requestId = msgId;
                transactionContext.attempt = message.getAttempt();
                transactionContext.selectionIdentity = selectionDigest;
                transactionContext.selectionPayloadDigest =
                    GenericSelectionTxnStore::digest({
                        opaqueParticipantPayload.data(),
                        opaqueParticipantPayload.size()});
                transactionContext.providerIdentity = providerName;
                transactionContext.providerBootEpoch =
                    std::to_string(m_processStartedAtUs);
                transactionContext.localDeadline = localDeadline;
                transactionContext.expiresAtUnixMs = expiresAtUnixMs;
                transactionContext.providerTokenRecordRef =
                    providerTokenHash.empty() ?
                        key.toUri() + ":token-disabled" :
                        providerTokenHash;
                const auto pendingLease =
                    pendingReservationLeases.find(key);
                if (pendingLease != pendingReservationLeases.end()) {
                    transactionContext.leaseRecordRef =
                        pendingLease->second.computeDigest();
                }
                try {
                    committedParticipant = opaqueParticipantIt->second;
                    opaqueCommitted = m_genericSelectionTxnStore->commit(
                        transactionContext,
                        {opaqueParticipantPayload.data(),
                         opaqueParticipantPayload.size()},
                        *committedParticipant,
                        providerTokenAccepted,
                        true,
                        false);
                }
                catch (const std::exception& error) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        std::string("opaque Selection transaction rejected: ") +
                            error.what());
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
                ++m_cleanupInvocationCount;
                m_selectedProviderRequests.insert(key);
                if (!providerTokenHash.empty()) {
                    m_consumedProviderTokenHashes.insert(providerTokenHash);
                    m_selectedProviderTokenHashes[key] = providerTokenHash;
                }
                auto requestTokenHashIt =
                    m_pendingRequestTokenHashes.find(key);
                if (requestTokenHashIt !=
                    m_pendingRequestTokenHashes.end()) {
                    m_recentProviderRequestTokenHashes.erase(
                        requestTokenHashIt->second);
                    m_pendingRequestTokenHashes.erase(requestTokenHashIt);
                }
                pendingRequests.erase(it);
                pendingProviderTokens.erase(key);
                pendingReservationLeases.erase(key);
                m_recentProviderRequests.erase(key);
                m_selectionDecryptsInFlight.erase(key);
                m_selectionExecutionStatuses[
                    selectionDigest].decisionReceipt =
                        opaqueCommitted->acceptancePayload;
            }
            if (opaqueCommitted)
                goto opaque_selection_committed;
            const bool gatesInput = selectedRequest.hasRequestCapabilities() &&
                selectedRequest.getRequestCapabilities().hasField("SelectionGatedInputV1") &&
                selectedRequest.getRequestCapabilities().getField("SelectionGatedInputV1") == "required";
            if (gatesInput && !r1NotSelected) {
                if (!selectedRequest.hasEncryptedRequestInput() ||
                    !message.hasSelectionInputKeyGrant()) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        std::string("SelectionGatedInputV1 missing ") +
                        (!selectedRequest.hasEncryptedRequestInput() ?
                            "encrypted input" : "key grant"));
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
                const auto& grant = message.getSelectionInputKeyGrant();
                const auto publicKey = identityCert.getPublicKey();
                ndn::Buffer publicKeyBuffer(publicKey.begin(), publicKey.end());
                const bool grantMatches =
                    grant.hasField("recipient") &&
                    grant.getField("recipient") == providerName.toUri() &&
                    grant.hasField("recipientCertName") &&
                    grant.getField("recipientCertName") == identityCert.getName().toUri() &&
                    grant.hasField("recipientCertDigest") &&
                    grant.getField("recipientCertDigest") == sha256DigestString(publicKeyBuffer) &&
                    grant.hasField("wrappedInputKey") &&
                    grant.hasField("encryptedInputDigest") &&
                    grant.getField("encryptedInputDigest") ==
                        selectedRequest.getEncryptedRequestInput().computeDigest() &&
                    grant.hasField("requestId") &&
                    grant.getField("requestId") == msgId.toUri() &&
                    (!hasR1Decision ||
                     (grant.hasField("reservationId") &&
                      grant.getField("reservationId") == r1ReservationId));
                if (!grantMatches) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        "Selection input key grant binding mismatch");
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
                const auto wrapped = selectionGatedUnhex(grant.getField("wrappedInputKey"));
                auto& activeKeyChain = m_testSigningKeyChain ?
                    *m_testSigningKeyChain : m_keyChain;
                const auto inputKey = unwrapSelectionGatedInputKey(
                    wrapped, identityCert.getName(), activeKeyChain);
                ndn::Buffer plaintext;
                if (!decryptSelectionGatedInput(
                      selectedRequest.getEncryptedRequestInput(), inputKey,
                      requesterName, serviceName, msgId, plaintext)) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        "Selection input authentication failed");
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
                selectedRequest.setPayload(plaintext, plaintext.size());
            }
            if (hasR1Decision && !r1NotSelected) {
                const auto& decision = message.getSelectionDecision();
                if (!message.hasDeploymentPlan() ||
                    !decision.hasField("globalPlanDigest") ||
                    decision.getField("globalPlanDigest") !=
                        message.getDeploymentPlan().computeDigest() ||
                    !message.hasRecipientEncryptedAssignment() ||
                    !effectiveAssignmentPayload.empty()) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        "R1 selected assignment is missing, plaintext, or plan-unbound");
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
                const auto aad = recipientAssignmentAssociatedData(
                    requesterName, providerName, serviceName, msgId,
                    r1ReservationId, message.getDeploymentPlan().computeDigest());
                if (!decryptRecipientAssignment(
                      message.getRecipientEncryptedAssignment(), providerName,
                      identityCert.getName(), m_keyChain, aad,
                      effectiveAssignmentPayload)) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        "recipient assignment authentication failed");
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
            }
            if (hasR1Decision) {
                SelectionDecisionReceipt receipt;
                const auto handler = m_r1SelectionDecisionHandlers.find(serviceName);
                try {
                    if (handler != m_r1SelectionDecisionHandlers.end()) {
                        receipt = handler->second(message.getSelectionDecision());
                    }
                    else {
                        receipt.setField("schemaVersion", "1");
                        receipt.setField("decisionDigest", r1DecisionDigest);
                        receipt.setField("reservationId", r1ReservationId);
                        receipt.setField("provider", providerName.toUri());
                        receipt.setField("acceptedState",
                                         r1NotSelected ? "RELEASE_ACCEPTED" :
                                                         "COMMIT_ACCEPTED");
                        receipt.setField("reason", "AUTHENTICATED");
                        receipt.setField("sequence", "1");
                        if (message.getSelectionDecision().hasField("providerBootEpoch"))
                            receipt.setField(
                                "providerBootEpoch",
                                message.getSelectionDecision().getField("providerBootEpoch"));
                    }
                }
                catch (const std::exception& e) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        std::string("R1 reservation transition rejected: ") + e.what());
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
                if (!receipt.hasField("decisionDigest") ||
                    receipt.getField("decisionDigest") != r1DecisionDigest ||
                    !receipt.hasField("reservationId") ||
                    receipt.getField("reservationId") != r1ReservationId) {
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        "R1 reservation transition returned an unbound receipt");
                    m_selectionDecryptsInFlight.erase(key);
                    return;
                }
                const auto receiptBlock = receipt.WireEncode();
                r1ReceiptWire = ndn::Buffer(receiptBlock.data(), receiptBlock.size());
                const auto pendingLease = pendingReservationLeases.find(key);
                if (pendingLease != pendingReservationLeases.end() &&
                    pendingLease->second.hasField("expiresAtMs")) {
                    try {
                        r1TombstoneRetainUntilMs = std::stoull(
                            pendingLease->second.getField("expiresAtMs"));
                    }
                    catch (const std::exception&) {
                        r1TombstoneRetainUntilMs = 0;
                    }
                }
                m_r1AcceptedSelectionDecisions.emplace(
                    r1ReservationId,
                    R1AcceptedSelectionDecision{
                        r1DecisionDigest, providerTokenHash,
                        r1NotSelected ? "NOT_SELECTED" : "SELECTED",
                        r1ReceiptWire, r1TombstoneRetainUntilMs});
                if (!r1NotSelected)
                    m_r1ReservationByRequest[key] = r1ReservationId;
            }
            ++m_cleanupInvocationCount;
            if (!r1NotSelected)
                m_selectedProviderRequests.insert(key);
            if (!providerTokenHash.empty()) {
                m_consumedProviderTokenHashes.insert(providerTokenHash);
                m_selectedProviderTokenHashes[key] = providerTokenHash;
            }
            auto requestTokenHashIt = m_pendingRequestTokenHashes.find(key);
            if (requestTokenHashIt != m_pendingRequestTokenHashes.end()) {
                m_recentProviderRequestTokenHashes.erase(requestTokenHashIt->second);
                m_pendingRequestTokenHashes.erase(requestTokenHashIt);
            }
            pendingRequests.erase(it);
            pendingProviderTokens.erase(key);
            pendingReservationLeases.erase(key);
            m_recentProviderRequests.erase(key);
            m_selectionDecryptsInFlight.erase(key);
        }
        if (requestScopedSelection) {
            // The Selection transaction is committed above; the Provider now
            // fetches the exact User-signed encrypted Input Data before
            // dispatching application code.  Keeping this branch ahead of
            // legacy payload/assignment dispatch prevents an empty discovery
            // payload from reaching an ordinary handler.
            fetchRequestScopedInputAndDispatch(
                requesterName, providerName, serviceName, msgId,
                std::move(selectedRequest), message, effectiveAssignmentPayload,
                selectionDigest);
            return;
        }
opaque_selection_committed:
        if (opaqueCommitted) {
            if (m_useTokens && !opaqueReplay)
                ++m_tokenConsumeCount;
            try {
                committedParticipant->onCommitted(*opaqueCommitted);
                updateSelectionExecutionStatus(
                    selectionDigest, SelectionExecutionState::Queued,
                    providerName, serviceName, msgId,
                    "opaque Selection committed; participant projection queued");
                m_selectionExecutionStatuses[
                    selectionDigest].decisionReceipt =
                        opaqueCommitted->acceptancePayload;
            }
            catch (const std::exception& error) {
                updateSelectionExecutionStatus(
                    selectionDigest, SelectionExecutionState::Failed,
                    providerName, serviceName, msgId,
                    std::string(
                        "opaque Selection accepted then projection failed: ") +
                        error.what());
                m_selectionExecutionStatuses[
                    selectionDigest].decisionReceipt =
                        opaqueCommitted->acceptancePayload;
            }
            // A replay returns the durable acceptance without re-entering the
            // application.  The first commit continues through the existing
            // generic CollaborationContext/Response path with the exact
            // opaque assignment bytes; Core does not interpret them.
            if (opaqueReplay) {
                return;
            }
        }
        if (hasR1Decision && r1TombstoneRetainUntilMs > 0) {
            const auto nowMs = nowMilliseconds();
            const auto delayMs = r1TombstoneRetainUntilMs > nowMs ?
                r1TombstoneRetainUntilMs - nowMs : 1;
            m_scheduler.schedule(ndn::time::milliseconds(delayMs),
                [this, reservationId = r1ReservationId,
                 decisionDigest = r1DecisionDigest] {
                    std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
                    const auto found =
                        m_r1AcceptedSelectionDecisions.find(reservationId);
                    if (found != m_r1AcceptedSelectionDecisions.end() &&
                        found->second.decisionDigest == decisionDigest &&
                        (found->second.retainUntilMs == 0 ||
                         nowMilliseconds() >= found->second.retainUntilMs)) {
                        m_r1AcceptedSelectionDecisions.erase(found);
                    }
                });
        }
        if (r1NotSelected) {
            updateSelectionExecutionStatus(selectionDigest,
                                           SelectionExecutionState::Completed,
                                           providerName, serviceName, msgId,
                                           "authenticated R1 reservation not selected");
            m_selectionExecutionStatuses[selectionDigest].decisionReceipt =
                r1ReceiptWire;
            return;
        }
        if (hasR1Decision) {
            m_selectionExecutionStatuses[selectionDigest].decisionReceipt =
                r1ReceiptWire;
        }
        if (m_timelineTrace) {
            logTimelineTrace("provider", "provider_token_validate_done", msgId,
                             {{"serviceName", serviceName.toUri()},
                              {"valid", "true"}});
        }
        if (m_useTokens && !opaqueCommitted) {
            ++m_tokenConsumeCount;
        }

        // A streamed collaboration grants the event key only to its terminal
        // role. A selected nonterminal role still executes its structured
        // assignment, but has no authority to publish user-facing events.
        // Ordinary streamed selections and any unstructured assignment remain
        // fail-closed when the grant is absent.
        const bool missingRequiredStreamGrant =
            selectedRequest.hasStreamRequestOptions() &&
            !message.hasStreamEventKeyGrant() &&
            !structuredAssignmentPayload;
        const bool rejectedStreamGrant =
            selectedRequest.hasStreamRequestOptions() &&
            message.hasStreamEventKeyGrant() &&
            !initializeStreamPublisher(requesterName, providerName, serviceName,
                                       msgId, selectedRequest, message,
                                       selectionDigest);
        if (missingRequiredStreamGrant || rejectedStreamGrant) {
            updateSelectionExecutionStatus(selectionDigest,
                                           SelectionExecutionState::Rejected,
                                           providerName, serviceName, msgId,
                                           "stream event-key grant binding rejected");
            return;
        }

        // Deployment-capable requests take the additive selection-gated path.
        // Legacy V2 requests (no DeploymentIntent) continue directly to the
        // existing handler path below.
        if (selectedRequest.hasDeploymentIntent()) {
            if (!message.hasDeploymentPlan()) {
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName, serviceName, msgId,
                                               "deployment Selection missing DeploymentPlan");
                return;
            }
            const auto& plan = message.getDeploymentPlan();
            if (!plan.hasField("requestId") ||
                plan.getField("requestId") != msgId.toUri() ||
                !plan.hasField("requesterIdentity") ||
                plan.getField("requesterIdentity") != requesterName.toUri() ||
                !plan.hasField("intentDigest") ||
                plan.getField("intentDigest") !=
                    selectedRequest.getDeploymentIntent().computeDigest()) {
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName, serviceName, msgId,
                                               "DeploymentPlan binding mismatch");
                return;
            }
            bool localMember = false;
            std::string localRole;
            for (size_t i = 0; i < DeploymentControlMessage::MAX_FIELDS; ++i) {
                const auto prefix = "member." + std::to_string(i) + ".";
                if (!plan.hasField(prefix + "provider")) continue;
                if (plan.getField(prefix + "provider") == providerName.toUri()) {
                    localMember = true;
                    localRole = plan.hasField(prefix + "role") ?
                        plan.getField(prefix + "role") : "primary";
                    break;
                }
            }
            if (!localMember || !m_deploymentPrepareHandler) {
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName, serviceName, msgId,
                                               localMember ?
                                                   "deployment preparation handler not registered" :
                                                   "Provider absent from exact DeploymentPlan");
                return;
            }
            if (hasR1Decision) {
                // R1 readiness is local state, not a network-wide barrier.
                // Preparation may verify/fetch/load/warm, then execution falls
                // through to the normal collaboration handler where direct
                // predecessor data gates non-source work.
                (void)m_deploymentPrepareHandler(
                    requesterName, providerName, serviceName, msgId,
                    selectedRequest, plan, selectionDigest);
                updateSelectionExecutionStatus(
                    selectionDigest, SelectionExecutionState::Queued,
                    providerName, serviceName, msgId,
                    "R1 local preparation complete; dependency-gated");
            }
            else {
            ProviderReadyMessage ready = m_deploymentPrepareHandler(
                requesterName, providerName, serviceName, msgId,
                selectedRequest, plan, selectionDigest);
            // The Core owns immutable protocol bindings; applications own only
            // the generic verify/load/warm work and operation identifiers.
            ready.setField("requestId", msgId.toUri());
            ready.setField("attempt", plan.getField("attempt"));
            ready.setField("selectionDigest", selectionDigest);
            ready.setField("deploymentPlanDigest", plan.computeDigest());
            ready.setField("providerIdentity", providerName.toUri());
            ready.setField("providerBootEpoch", std::to_string(m_processStartedAtUs));
            ready.setField("role", localRole);
            if (!ready.hasField("readySequence")) ready.setField("readySequence", "1");
            if (!ready.hasField("issuedAtUs"))
                ready.setField("issuedAtUs", std::to_string(nowMicroseconds()));
            if (!ready.hasField("expiresAtUs"))
                ready.setField("expiresAtUs", std::to_string(
                    nowMicroseconds() + static_cast<uint64_t>(m_pendingRequestTimeoutGrace.count()) * 1000));
            PreparedDeploymentExecution prepared;
            prepared.requesterName = requesterName;
            prepared.providerName = providerName;
            prepared.serviceName = serviceName;
            prepared.requestId = msgId;
            prepared.request = selectedRequest;
            prepared.plan = plan;
            prepared.ready = ready;
            prepared.selectionDigest = selectionDigest;
            m_preparedDeployments.emplace(selectionDigest, std::move(prepared));
            updateSelectionExecutionStatus(selectionDigest,
                                           SelectionExecutionState::Queued,
                                           providerName, serviceName, msgId,
                                           "deployment READY; awaiting activation");
            if (m_providerReadyPublisher) {
                m_providerReadyPublisher(requesterName, ready);
            }
            else {
                std::string statusHandle;
                for (size_t i = 0; i < DeploymentControlMessage::MAX_FIELDS; ++i) {
                    const auto prefix = "member." + std::to_string(i) + ".";
                    if (plan.hasField(prefix + "provider") &&
                        plan.getField(prefix + "provider") == providerName.toUri() &&
                        plan.hasField(prefix + "statusHandle")) {
                        statusHandle = plan.getField(prefix + "statusHandle");
                        break;
                    }
                }
                if (!isValidOpaqueControlHandle(statusHandle)) {
                    m_preparedDeployments.erase(selectionDigest);
                    updateSelectionExecutionStatus(
                        selectionDigest, SelectionExecutionState::Rejected,
                        providerName, serviceName, msgId,
                        "DeploymentPlan has no valid local StatusHandle");
                    return;
                }
                publishProviderReady(requesterName, ready, statusHandle);
            }
            return;
            }
        }

        for (const auto& requestID : message.getRequestIDs()) {
            const ndn::Name requestId(requestID);
            auto collabService = m_collaborationServices.find(serviceName);
            if (!hasService(serviceName) &&
                collabService == m_collaborationServices.end()) {
                NDN_LOG_INFO("No V2 dynamic handler for " << serviceName.toUri());
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName,
                                               serviceName,
                                               requestId,
                                               "service handler not found");
                continue;
            }

            const auto leaseValidation =
                validateGenericAdmissionLeaseForSelection(requesterName,
                                                          providerName,
                                                          serviceName,
                                                          requestId,
                                                          selectedRequest,
                                                          message,
                                                          effectiveAssignmentPayload);
            if (!leaseValidation.status) {
                std::cout << "NDNSF_ADMISSION_LEASE_REJECTED"
                          << " provider=" << providerName.toUri()
                          << " requester=" << requesterName.toUri()
                          << " service=" << serviceName.toUri()
                          << " requestId=" << requestId.toUri()
                          << " leaseId=" << leaseValidation.leaseId
                          << " reason=" << leaseValidation.reasonCode
                          << std::endl;
                NDN_LOG_WARN("NDNSF_ADMISSION_LEASE_REJECTED provider="
                             << providerName.toUri()
                             << " requester=" << requesterName.toUri()
                             << " service=" << serviceName.toUri()
                             << " requestId=" << requestId.toUri()
                             << " leaseId=" << leaseValidation.leaseId
                             << " reason=" << leaseValidation.reasonCode);
                NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=ADMISSION_LEASE_REJECTED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << requestId.toUri()
                          << " serviceName=" << serviceName.toUri()
                          << " requesterName=" << requesterName.toUri()
                          << " providerName=" << providerName.toUri()
                          << " leaseId=" << leaseValidation.leaseId
                          << " reason=" << leaseValidation.reasonCode);
                updateSelectionExecutionStatus(selectionDigest,
                                               SelectionExecutionState::Rejected,
                                               providerName,
                                               serviceName,
                                               requestId,
                                               "admission lease rejected: " +
                                                   leaseValidation.reasonCode);
                continue;
            }
            if (leaseValidation.reasonCode != "NOT_REQUIRED") {
                std::cout << "NDNSF_ADMISSION_LEASE_ACCEPTED"
                          << " provider=" << providerName.toUri()
                          << " requester=" << requesterName.toUri()
                          << " service=" << serviceName.toUri()
                          << " requestId=" << requestId.toUri()
                          << " leaseId=" << leaseValidation.leaseId
                          << std::endl;
                NDN_LOG_INFO("NDNSF_ADMISSION_LEASE_ACCEPTED provider="
                             << providerName.toUri()
                             << " requester=" << requesterName.toUri()
                             << " service=" << serviceName.toUri()
                             << " requestId=" << requestId.toUri()
                             << " leaseId=" << leaseValidation.leaseId);
            }

            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PROVIDER_EXECUTE_START timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " requesterName=" << requesterName.toUri()
                      << " providerName=" << providerName.toUri());
            if (m_timelineTrace) {
                logTimelineTrace("provider", "service_execution_start", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"requesterName", requesterName.toUri()},
                                  {"providerName", providerName.toUri()}});
            }
            updateProviderRequestLifecycleState(
                requestId, serviceName,
                ProviderRequestLifecycleState::EXECUTION_STARTED);
            updateSelectionExecutionStatus(selectionDigest,
                                           SelectionExecutionState::Queued,
                                           providerName,
                                           serviceName,
                                           requestId,
                                           "handler queued");
            m_selectedOutstandingRequests.fetch_add(1, std::memory_order_relaxed);
            RequestMessage requestCopy = selectedRequest;
            if (collabService != m_collaborationServices.end()) {
                // A V3 assignment set contains one sealed JSON projection per
                // local role. Dispatch those envelopes separately; legacy
                // binary/semicolon assignment sets still represent one local
                // execution context and must retain their existing behavior.
                std::vector<ndn::Buffer> rolePayloads;
                bool v3AssignmentSet = structuredAssignmentPayload;
                if (structuredAssignmentPayload) {
                    for (const auto& item :
                         decodeOpaqueAssignmentSet(effectiveAssignmentPayload)) {
                        CollaborationAssignmentEnvelope envelope;
                        if (decodeCollaborationAssignmentEnvelope(item, envelope)) {
                            rolePayloads.push_back(item);
                            const auto opaqueText = std::string(
                                reinterpret_cast<const char*>(
                                  envelope.opaquePayload.data()),
                                envelope.opaquePayload.size());
                            const auto first = opaqueText.find_first_not_of(
                                " \t\r\n");
                            const auto reference =
                                parseLargeDataReferencePayload(
                                    envelope.opaquePayload);
                            const bool inlineV3 =
                                first != std::string::npos &&
                                opaqueText[first] == '{';
                            const bool externalV3 =
                                reference && reference->encrypted &&
                                reference->objectType ==
                                  "application/vnd.ndnsf.collaboration-assignment-v1";
                            if (!inlineV3 && !externalV3) {
                                v3AssignmentSet = false;
                            }
                        }
                        else {
                            v3AssignmentSet = false;
                        }
                    }
                }
                if (!v3AssignmentSet || rolePayloads.empty()) {
                    rolePayloads.clear();
                    rolePayloads.push_back(effectiveAssignmentPayload);
                }
                bool dispatchedAny = false;
                for (const auto& rolePayload : rolePayloads) {
                    auto assignment =
                        parseCollaborationAssignment(serviceName, rolePayload);
                    // The Provider-entry projection is Core-owned metadata. It
                    // remains available even when the application registers an
                    // opaque Selection participant; the participant still sees
                    // only its exact envelope opaquePayload below.
                    for (const auto& entry : message.getProviderEntries()) {
                        for (const auto& entryRole :
                             rolesFromAssignmentPayload(entry.assignmentPayload)) {
                            assignment.roleProviders[entryRole] = entry.providerName;
                        }
                    }
                    // Structured deferred assignments carry exact
                    // provider-scoped scope-key references in their envelope.
                    if ((hasOpaqueParticipant || structuredAssignmentPayload) &&
                        !sharedAssignmentPayload.empty()) {
                        for (const auto& field :
                             parseSemicolonFields(sharedAssignmentPayload)) {
                            static const std::string prefix = "scopeKeyData.";
                            static const std::string roleProviderPrefix =
                                "roleProvider.";
                            if (field.first.rfind(roleProviderPrefix, 0) == 0 &&
                                !field.first.substr(roleProviderPrefix.size()).empty() &&
                                !field.second.empty()) {
                                assignment.roleProviders[
                                    field.first.substr(roleProviderPrefix.size())] =
                                      ndn::Name(field.second);
                            }
                            else if (assignment.scopeKeys.empty() &&
                                     assignment.scopeKeyDataNames.empty() &&
                                     field.first.rfind(prefix, 0) == 0 &&
                                     !field.first.substr(prefix.size()).empty() &&
                                     !field.second.empty()) {
                                assignment.scopeKeyDataNames[
                                    field.first.substr(prefix.size())] =
                                      ndn::Name(field.second);
                            }
                        }
                    }
                    assignment.selectionDigest = selectionDigest;
                    dispatchedAny = dispatchCollaborationExecutionAsync(
                        requesterName, providerName, serviceName, requestId,
                        requestCopy, std::move(assignment), selectionDigest) ||
                      dispatchedAny;
                }
                if (dispatchedAny) {
                    continue;
                }
            }
            std::shared_ptr<RegistrationState> inlineRegistrationState;
            if (dispatchRequestExecutionAsync(requesterName,
                                              providerName,
                                              serviceName,
                                              requestId,
                                              requestCopy,
                                              selectionDigest,
                                              &inlineRegistrationState)) {
                continue;
            }
            // spec182: a pool-0 inline dispatch applies the same generation
            // fence as the async path; refusal already published its failure.
            if (!gateInlineRequestExecution(
                    requesterName, providerName, serviceName, requestId,
                    requestCopy, selectionDigest, inlineRegistrationState)) {
                continue;
            }

            updateSelectionExecutionStatus(selectionDigest,
                                           SelectionExecutionState::Running,
                                           providerName,
                                           serviceName,
                                           requestId,
                                           "handler running inline");
            auto response = dispatchRequest(requesterName,
                                            providerName,
                                            serviceName,
                                            requestId,
                                            requestCopy);
            finishRequestExecutionOnEventLoop(requesterName,
                                              providerName,
                                              serviceName,
                                              requestId,
                                              requestCopy,
                                              std::move(response),
                                              selectionDigest,
                                              inlineRegistrationState);
        }
    }


    void ServiceProvider::OnServiceSelectionMessageDecryptionErrorCallback(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& msgId,
        const std::string& reason)
    {
        const auto key = ndn::Name(requesterName.toUri())
                             .append(serviceName)
                             .append(msgId);
        {
            std::lock_guard<std::mutex> lock(m_pendingRequestMutex);
            m_selectionDecryptsInFlight.erase(key);
        }
        // log error
        NDN_LOG_ERROR("OnServiceSelectionMessageDecryptionErrorCallback: "
                      << requesterName.toUri() << providerName.toUri()
                      << serviceName.toUri() << msgId.toUri()
                      << " reason: " << reason);

    }

    void ServiceProvider::onStreamEvent(
        const ndn::svs::SVSPubSub::SubscriptionData& subscription)
    {
        if (!subscription.packet) return;
        const auto parsed = parseInvocationEventName(subscription.packet->getName());
        if (!parsed || !parsed->producer.equals(identity)) return;
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=STREAM_EVENT_OBSERVED "
                      << "requestId=" << parsed->requestId.toUri()
                      << " cursor=" << parsed->cursor);
    }

    void ServiceProvider::registerNDNSFMessages()
    {
        // log register
        NDN_LOG_WARN("Register NDNSF Messages in ndn-svs");
        for(auto serviceName:m_serviceNames){
            registerRequestSubscription(ndn::Name(serviceName));
        }
        // Selection names carry the selected provider and service in their
        // suffix, so one provider-wide subscription covers services that are
        // registered after init() as well as the legacy init-time list.  The
        // previous per-service registration silently missed selections for
        // dynamically added scoped DI services.
        std::string selectionRegex = "^(<>*)<NDNSF><SELECTION>(<>*)$";
        NDN_LOG_DEBUG(selectionRegex);
        m_svsps->subscribeWithRegex(
            ndn::Regex(selectionRegex),
            std::bind(&ServiceProvider::onServiceSelectionMessage, this, _1),
            true, false);
        std::string collabRegex = "^(<>*)<NDNSF><COLLAB>(<>*)$";
        NDN_LOG_DEBUG(collabRegex);
        m_svsps->subscribeWithRegex(ndn::Regex(collabRegex),
                                    std::bind(&ServiceProvider::onCollaborationDataMessage, this, _1),
                                    true, false);
        // Stream events are already signed Data packets. Providers subscribe
        // with packet delivery enabled so an exact-name retry can be serviced
        // from the retained publisher without decoding a synthetic payload.
        std::string eventRegex = "^(<>*)<NDNSF><EVENT>(<>*)$";
        m_svsps->subscribeWithRegex(ndn::Regex(eventRegex),
                                    std::bind(&ServiceProvider::onStreamEvent, this, _1),
                                    true, true);
    }

    void ServiceProvider::registerRequestSubscription(const ndn::Name& serviceName)
    {
        if (serviceName.empty() || !m_svsps) {
            return;
        }
        ndn::Name regexServiceName(serviceName);
        const std::string regex =
            "^(<>*)<NDNSF><REQUEST>" +
            ndn_service_framework::NameToRegexString(regexServiceName) +
            "(<>*)$";
        // V2 requests are published as:
        //   /<requester>/NDNSF/REQUEST/<serviceName...>/<requestId>
        NDN_LOG_WARN("[ServiceProvider] SVS request subscription regex="
                     << regex);
        NDN_LOG_DEBUG(regex);
        m_svsps->subscribeWithRegex(
            ndn::Regex(regex),
            std::bind(&ServiceProvider::OnRequest, this, _1),
            true, false);
    }

    bool ServiceProvider::isFresh(const ndn::svs::SVSPubSub::SubscriptionData& subscription)
    {
        const ndn::Name& producerPrefix = subscription.producerPrefix;

        if (producerPrefix.size() < 1)
            return false;

        std::string lastComponentStr = producerPrefix[-1].toUri();
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

        std::lock_guard<std::mutex> lock(svs_mutex);
        auto it = m_sessionIDMap.find(basePrefix);
        if (it != m_sessionIDMap.end() && it->second.first > sessionID) {
            return false;
        }

        // A higher producer session resets all per-name frontiers.  Within a
        // session, independent publications may legitimately arrive out of
        // order; reject only an older sequence for the same publication name.
        if (it == m_sessionIDMap.end() || it->second.first < sessionID) {
            m_sessionIDMap[basePrefix] = {sessionID, subscription.seqNo};
            m_publicationSeqMap[basePrefix].clear();
        }
        else {
            it->second.second = std::max(it->second.second, subscription.seqNo);
        }

        auto& byName = m_publicationSeqMap[basePrefix];
        const auto publicationIt = byName.find(subscription.name);
        if (publicationIt != byName.end() && subscription.seqNo <= publicationIt->second) {
            return false;
        }
        byName[subscription.name] = subscription.seqNo;
        return true;
    }

}
