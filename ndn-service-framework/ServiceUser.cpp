#include "ServiceUser.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validator-null.hpp>
#include <ndn-cxx/security/transform/public-key.hpp>
#include <ndn-cxx/util/sha256.hpp>

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.ServiceUser);

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

        uint64_t
        nowMicroseconds()
        {
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
        sha256DigestString(const ndn::Buffer& payload)
        {
            ndn::util::Sha256 digest;
            if (!payload.empty()) {
                digest << std::string(reinterpret_cast<const char*>(payload.data()),
                                      payload.size());
            }
            return "sha256:" + digest.toString();
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

        ndn::Name
        makeLargeDataNameWithoutPrefix(const ndn::Name& serviceName,
                                       const ndn::Name& requestId,
                                       const std::string& objectId)
        {
            ndn::Name name("/NDNSF/LARGE-DATA");
            name.append(serviceName).append(requestId).append(objectId);
            return name;
        }

        ndn::Name
        makeLargeDataName(const ndn::Name& userPrefix,
                          const ndn::Name& serviceName,
                          const ndn::Name& requestId,
                          const std::string& objectId)
        {
            ndn::Name name(userPrefix);
            name.append(makeLargeDataNameWithoutPrefix(serviceName, requestId, objectId));
            return name;
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
        : ServiceUser(face,
                      std::move(group_prefix),
                      getExistingEncryptionCertificateOrThrow(identityCert),
                      getExistingSigningCertificateOrFallback(identityCert),
                      std::move(attrAuthorityCertificate),
                      std::move(trustSchemaPath))
    {
    }

    ServiceUser::ServiceUser(ndn::Face &face,
                             ndn::Name group_prefix,
                             ndn::security::Certificate encryptionCert,
                             ndn::security::Certificate signingCert,
                             ndn::security::Certificate attrAuthorityCertificate,
                             std::string trustSchemaPath) :
        m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(encryptionCert.getIdentity()),
        validator(std::make_shared<MessageValidator>(
          trustSchemaPath, group_prefix, &face)),
        identityCert(encryptionCert),
        signingCert(signingCert),
        // nac_validator(std::move(ndn::security::ValidatorNull())),
        nacConsumer(m_face, m_keyChain, nac_validator, encryptionCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, encryptionCert, attrAuthorityCertificate),
        m_IMS(50000)
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
                                      m_keyChain,
                                      std::bind(&ServiceUser::processNDNSDServiceInfoCallback, this, _1));
        }

        nac_validator.load(trustSchemaPath);

        nacConsumer.obtainDecryptionKey();


        // Serve NDNSF and ck messages using IMS
        m_face.setInterestFilter(ndn::Name(identity.toUri()).append("NDNSF"),
            std::bind(&ServiceUser::onInterest, this, _1, _2),
            std::bind(&ServiceUser::onPrefixRegisterFailure, this, _1, _2));
        m_face.setInterestFilter(ndn::Name(identity.toUri()).append("CK"),
            std::bind(&ServiceUser::onInterest, this, _1, _2),
            std::bind(&ServiceUser::onPrefixRegisterFailure, this, _1, _2));

        m_signingInfo = ndn::security::signingByCertificate(signingCert);

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

        while(!nacConsumer.readyForDecryption()){
            // log waiting for decryption key
            nacConsumer.obtainDecryptionKey();
            NDN_LOG_INFO("Waiting for decryption key");
            face.processEvents(ndn::time::milliseconds(1000));
        }



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
        nacConsumer(m_face, m_keyChain, nac_validator, encryptionCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, encryptionCert, attrAuthorityCertificate),
        m_IMS(50000),
        m_configManager("/tmp/ndnsf-service-user-local-mock.conf")
    {
        ensureSameIdentity(encryptionCert, signingCert, "ServiceUser");
        if (!isRsaCertificate(encryptionCert)) {
            throw std::invalid_argument("ServiceUser encryptionCert must be RSA for NAC-ABE");
        }
        m_signingInfo = ndn::security::signingByCertificate(signingCert);
    }

    void ServiceUser::init()
    {
        registerNDNSFMessages();
    }

    ServiceUser::~ServiceUser()
    {
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
	            const auto mapping = m_svsps->getMappingFetchStats();
	            const auto publication = m_svsps->getPublicationFetchStats();
	            const auto piggy = m_svsps->getPiggybackStats();
	            NDN_LOG_INFO("NDNSF_SVS_DELIVERY_STATS role=user"
	                         << " malformed=" << rejection.malformedEnvelope
	                         << " signaturePolicy=" << rejection.signaturePolicy
	                         << " vectorDecode=" << rejection.vectorDecode
	                         << " mappingQueued=" << mapping.queued
	                         << " mappingPending=" << mapping.pending
	                         << " mappingDispatched=" << mapping.dispatched
	                         << " mappingData=" << mapping.data
	                         << " mappingNacks=" << mapping.nacks
	                         << " mappingTimeouts=" << mapping.timeouts
	                         << " mappingRetries=" << mapping.retries
	                         << " publicationQueued=" << publication.queued
	                         << " publicationPending=" << publication.pending
	                         << " publicationDispatched=" << publication.dispatched
	                         << " publicationData=" << publication.data
	                         << " publicationNacks=" << publication.nacks
	                         << " publicationTimeouts=" << publication.timeouts
	                         << " publicationRetries=" << publication.retries
	                         << " piggyReceived=" << piggy.received
	                         << " piggyDelivered=" << piggy.delivered
	                         << " publicationFallbacks=" << piggy.publicationFetchFallbacks
	                         << " publicationRetryActivations=" << piggy.publicationRetryActivations);
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
        ndn::Interest interest(makeSelectionStatusQueryName(providerName,
                                                            serviceName,
                                                            selectionDigest));
        interest.setCanBePrefix(false);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::milliseconds(
            std::max(1, timeoutMs)));

        const auto timeoutHandler = std::move(onTimeout);
        m_face.expressInterest(
            interest,
            [this, providerName, serviceName, selectionDigest,
             onStatus = std::move(onStatus), timeoutHandler](
                const ndn::Interest&, const ndn::Data& data) {
                validator->validate(
                    data,
                    [providerName, serviceName, selectionDigest, onStatus](
                        const ndn::Data& validatedData) {
                        if (!isSignedByIdentity(validatedData, providerName)) {
                            return;
                        }
                        auto status = parseSelectionExecutionStatusPayload(
                            validatedData, providerName, selectionDigest);
                        if (!status.providerName.equals(providerName) ||
                            !status.serviceName.equals(serviceName) ||
                            status.selectionDigest != selectionDigest) {
                            return;
                        }
                        if (onStatus) onStatus(status);
                    },
                    [timeoutHandler](const ndn::Data& badData,
                                     const ndn::security::ValidationError&) {
                        if (timeoutHandler) timeoutHandler(badData.getName());
                    });
            },
            [timeoutHandler](
                const ndn::Interest& interest, const ndn::lp::Nack&) {
                if (timeoutHandler) {
                    timeoutHandler(interest.getName());
                }
            },
            [timeoutHandler](const ndn::Interest& interest) {
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
        output.reserve(pending->second.selectionStatusesByProvider.size());
        for (const auto& item : pending->second.selectionStatusesByProvider) {
            output.push_back(item.second);
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
            [this, requestId, providerUri](
                const SelectionExecutionStatus& status) {
                auto call = m_pendingCalls.find(requestId);
                if (call == m_pendingCalls.end()) {
                    return;
                }
                call->second.selectionStatusesByProvider[providerUri] = status;
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
                [this, requestId] (const SelectionExecutionStatus& status) {
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
                [this, requestId, providerName, serviceName, selectionDigest](
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

    void ServiceUser::setTimelineTrace(bool enabled)
    {
        m_timelineTrace = enabled;
        if (enabled) {
            setenv("NDNSF_TIMELINE_TRACE", "1", 1);
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
        fetchPolicyManifestFromController(controllerPrefix);

        ndn::Name interestName(controllerPrefix);
        interestName.append(ndn::Name("/NDNSF/PERMISSIONS/USER"));
        interestName.append(identity);

        ndn::Interest interest(interestName);
        interest.setCanBePrefix(true);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::milliseconds(permissionFetchLifetimeMs()));

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

    void ServiceUser::applyPermissionResponse(const PermissionResponse& response)
    {
        if (response.getPermissionKind() != tlv::UserPermission) {
            NDN_LOG_ERROR("Ignoring non-user PermissionResponse for "
                          << response.getTargetIdentity());
            return;
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
                tlv::UserPermission, response.getPolicyEpoch()});
        }
        if (!m_authorizations.replacePermissions(tlv::UserPermission,
                                                 response.getPolicyEpoch(),
                                                 records)) {
            NDN_LOG_ERROR("Rejected invalid or stale user PermissionResponse epoch="
                          << response.getPolicyEpoch());
            return;
        }
        m_currentPolicyEpoch = response.getPolicyEpoch();
        for (const auto& record : records) {
            NDN_LOG_INFO("Installed user permission provider="
                         << record.providerServiceName
                         << " service=" << record.serviceName
                         << " policyEpoch=" << record.policyEpoch);
        }
    }

    size_t ServiceUser::getCurrentPolicyEpoch() const
    {
        return m_currentPolicyEpoch;
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

        if (response.getTargetIdentity() != identity.toUri()) {
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
        requestMessage.setPayload(const_cast<ndn::Buffer&>(payload), payload.size());
        requestMessage.setStrategy(strategy);
        requestMessage.setPolicyEpoch(m_currentPolicyEpoch);
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
                              {"strategy", std::to_string(strategy)}});
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
        const ndn::time::milliseconds& freshness)
    {
        LargeDataPublishResult result;
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
            result.objectId = "object-" + RandomString(16);
        }

        ndn::Name encryptedDataName =
            makeLargeDataName(identity, ctx.serviceName, ctx.requestId, result.objectId);
        encryptedDataName.appendVersion();
        const std::vector<std::string> attributes = {
            "/SERVICE" + ctx.serviceName.toUri()
        };

        try {
            const auto messageType = std::string("REQUEST-LARGE");
            const auto accessAttribute = std::string("/SERVICE") + ctx.serviceName.toUri();
            auto key = m_hybridMessageCrypto.getOrCreateSendKey(
                ctx.serviceName, identity, accessAttribute, messageType, m_hybridCryptoCounters);

            HybridMessageEnvelope envelope;
            envelope.setKeyId(key.keyId);
            envelope.setEpochId(key.epochId);
            envelope.setMessageType(messageType);

            if (m_hybridMessageCrypto.shouldAttachWrappedKey(key.keyId)) {
                ndn::nacabe::SPtrVector<ndn::Data> contentData;
                ndn::nacabe::SPtrVector<ndn::Data> ckData;
                std::tie(contentData, ckData) =
                    nacProducer.produce(key.keyName,
                                        attributes,
                                        ndn::span<const uint8_t>(key.key.data(), key.key.size()),
                                        m_signingInfo);
                auto wrapped = mergeDataContents(contentData);
                if (wrapped.empty()) {
                    result.errorMessage = "NAC-ABE produced no wrapped large-data MessageKey";
                    return result;
                }
                serveDataWithIMS(contentData, ckData);
                m_hybridMessageCrypto.cacheWrappedSendKey(
                    key.keyId, ndn::Buffer(wrapped.data(), wrapped.size()));
                ++m_hybridCryptoCounters.nac_abe_key_wrap_count;
            }

            const std::string adText = encryptedDataName.toUri() + "|" +
                                       messageType + "|" + ctx.serviceName.toUri();
            const ndn::Buffer ad(reinterpret_cast<const uint8_t*>(adText.data()),
                                 adText.size());
            auto encrypted = hybridAesGcmEncrypt(
                key.key,
                ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                ndn::span<const uint8_t>(ad.data(), ad.size()));
            envelope.setNonce(encrypted.nonce);
            envelope.setCipherText(encrypted.ciphertext);
            envelope.setAuthTag(encrypted.tag);

            auto envelopeBlock = envelope.WireEncode();
            ndn::Buffer encoded(envelopeBlock.begin(), envelopeBlock.end());

            ndn::Segmenter segmenter(m_keyChain, m_signingInfo);
            auto segments = segmenter.segment(
                ndn::span<const uint8_t>(encoded.data(), encoded.size()),
                encryptedDataName,
                7000,
                freshness);
            if (segments.empty()) {
                result.errorMessage = "large-data hybrid segmenter produced no segments";
                return result;
            }

            const bool activePut =
                !isTruthyEnv("NDNSF_REQUEST_LARGE_DISABLE_ACTIVE_PUT");
            for (const auto& data : segments) {
                {
                    std::lock_guard<std::mutex> lock(_cache_mutex);
                    m_IMS.insert(*data, freshness);
                }
                if (activePut) {
                    m_face.put(*data);
                }
            }

            result.encryptedDataName = encryptedDataName;
            NDN_LOG_INFO("LARGE_DATA_PUBLISH_SEGMENTS"
                      << " name=" << result.encryptedDataName.toUri()
                      << " plaintextBytes=" << plaintext.size()
                      << " envelopeBytes=" << encoded.size()
                      << " segments=" << segments.size()
                      << " wrappedKeyAttached=" << envelope.hasWrappedMessageKey()
                      << " activePut=" << activePut);
            result.success = true;
        }
        catch (const std::exception& e) {
            result.errorMessage = e.what();
        }
        return result;
    }

    ndn::Name ServiceUser::publishSignedAppData(
        const ndn::Name& dataName,
        const ndn::Buffer& payload,
        const ndn::time::milliseconds& freshness)
    {
        ndn::Name allowedPrefix(identity);
        allowedPrefix.append("NDNSF").append("DI");
        if (!allowedPrefix.isPrefixOf(dataName) || dataName.size() <= allowedPrefix.size()) {
            throw std::invalid_argument(
                "signed APP Data name must be below the local /NDNSF/DI prefix");
        }
        if (freshness <= ndn::time::milliseconds::zero()) {
            throw std::invalid_argument("signed APP Data freshness must be positive");
        }

        // InMemoryStorageEntry retains Data through shared_from_this(); a stack
        // Data would deterministically throw std::bad_weak_ptr on insertion.
        auto data = std::make_shared<ndn::Data>(dataName);
        data->setFreshnessPeriod(freshness);
        data->setContent(payload);
        m_keyChain.sign(*data, m_signingInfo);
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            m_IMS.insert(*data, freshness);
        }
        NDN_LOG_INFO("Published signed APP Data name=" << dataName
                     << " bytes=" << payload.size());
        return dataName;
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
        const ndn::time::milliseconds& freshness)
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
        if (!hasUserPermissionForRequest(providers, serviceName)) {
            NDN_LOG_ERROR("Reject request without user permission serviceName="
                          << serviceName.toUri());
            return ndn::Name();
        }
        auto selectionGatedInputKey = prepareSelectionGatedInput(
            requestMessage, serviceName, requestId);
        PendingCall pendingCall;
        pendingCall.providers = providers;
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
        pendingCall.selectionGatedInputKey = std::move(selectionGatedInputKey);
        pendingCall.strategy = strategy;
        pendingCall.timeoutMs = timeoutMs;
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

        admitOrQueuePendingCall(requestId, false, false);
        return requestId;
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
        const auto serviceUri = serviceName.toUri();
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
            pool.push_back(TargetedTokenPair{providerIt->second, userIt->second});
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
                poolIt == m_targetedTokenPools.end() ||
                !controlIt->second.observed ||
                controlIt->second.refillInFlight) {
                return;
            }
            const auto capacity = std::max<size_t>(1, controlIt->second.capacity);
            const auto lowWatermark = std::max<size_t>(
                1, std::min<size_t>(8, capacity / 4));
            if (poolIt->second.size() > lowWatermark) {
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

        const ndn::Name requestId = makeRequestId();
        TargetedTokenPair tokenPair;
        const auto& requestTokens = requestMessage.getTokens();
        const bool forceRefill = requestTokens.find("targeted.refill") !=
                                 requestTokens.end();
        bool refillOwner = false;
        bool hasCachedTargetedToken =
            !forceRefill &&
            (!m_useTokens || popTargetedTokenPair(provider, serviceName, tokenPair));
        if (hasCachedTargetedToken) {
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
                bootstrapTokens["targeted.batch_hint"] = std::to_string(
                    getTargetedTokenBatchHint(provider, serviceName));
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

        PendingCall pendingCall;
        pendingCall.providers = {provider};
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
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
            throw;
        }
        if (requestUsesTargetedFastPath) {
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

        PendingCall pendingCall;
        pendingCall.serviceName = serviceName;
        pendingCall.selectionGatedInputKey = prepareSelectionGatedInput(
            requestMessage, serviceName, requestId);
        pendingCall.requestMessage = requestMessage;
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

        PendingCall pendingCall;
        pendingCall.serviceName = serviceName;
        pendingCall.selectionGatedInputKey = prepareSelectionGatedInput(
            requestMessage, serviceName, requestId);
        pendingCall.requestMessage = requestMessage;
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

        PendingCall pendingCall;
        pendingCall.providers = providers;
        pendingCall.serviceName = serviceName;
        pendingCall.selectionGatedInputKey = prepareSelectionGatedInput(
            requestMessage, serviceName, requestId);
        pendingCall.requestMessage = requestMessage;
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

            PendingCall pendingCall;
            pendingCall.providers = providers;
            pendingCall.serviceName = serviceName;
            pendingCall.selectionGatedInputKey = prepareSelectionGatedInput(
                requestMessage, serviceName, requestId);
            pendingCall.requestMessage = requestMessage;
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

    ndn::Name ServiceUser::BeginCollaboration(
        const ServiceName& service,
        const RequestPayload& initialRequest,
        int ackCollectionTimeMs,
        int timeoutMs,
        CollaborationAckClosedHandler onAckClosed,
        ResponseHandler onFinalResponse,
        TimeoutHandler onTimeout,
        const RequestId& requestedRequestId,
        CollaborationAckCoverageHandler onAckCoverage)
    {
        if (!onAckClosed || ackCollectionTimeMs <= 0 ||
            timeoutMs <= ackCollectionTimeMs) {
            throw std::invalid_argument(
                "deferred collaboration requires valid ACK and request deadlines");
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

        PendingCall pendingCall;
        pendingCall.serviceName = service;
        pendingCall.requestMessage = std::move(requestMessage);
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
        m_pendingCalls[requestId] = std::move(pendingCall);

        updateRequestLifecycleState(
            requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=COLLAB_DEFERRED_REQUEST_CREATED"
                  << " timestamp_us=" << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << service.toUri());
        admitOrQueuePendingCall(requestId, true, true);
        return requestId;
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
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=COLLAB_PLAN_COMMITTED"
                  << " timestamp_us=" << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " ackClosedDigest=" << ackClosedDigest
                  << " planDigest=" << commitDigest
                  << " selectedCount=" << selected.size());
        if (!evaluateAckSelection(requestId)) {
            throw std::runtime_error(
                "committed collaboration plan produced no Selection");
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
        auto releaseResponseDecryptInFlight = [&] {
            removeName(pendingCall->second.responseDecryptProvidersInFlight,
                       providerName);
        };
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
        if (!isAcceptablePolicyEpoch(responseMessage.getPolicyEpoch())) {
            NDN_LOG_ERROR("Reject response with stale policy epoch for requestId="
                          << requestId.toUri()
                          << " receivedEpoch=" << responseMessage.getPolicyEpoch()
                          << " currentEpoch=" << m_currentPolicyEpoch);
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

        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_DONE timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri());
        storeTargetedTokenPairs(providerName,
                                pendingCall->second.serviceName,
                                responseMessage);
        pendingCall->second.responseValidatedAtUs = nowMicroseconds();
        handleResponse(requestId, providerName, responseMessage);
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

    bool ServiceUser::handleDecryptedResponseByName(
        const ndn::Name& responseName,
        const ndn_service_framework::ResponseMessage& responseMessage)
    {
        auto parsedV2 = ndn_service_framework::parseResponseNameV2(responseName);
        if (parsedV2) {
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
                    auto transportValidator = std::make_shared<ndn::security::ValidatorNull>();
                    auto fetcher = ndn::SegmentFetcher::start(m_face,
                                                               interest,
                                                               *transportValidator,
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

        HybridMessageEnvelope envelope;
        try {
            ndn::Block block(*encodedEnvelope);
            if (!envelope.WireDecode(block)) {
                errorMessage = "large response hybrid envelope decode failed";
                return std::nullopt;
            }
        }
        catch (const std::exception& e) {
            errorMessage = std::string("large response hybrid envelope parse failed: ") + e.what();
            return std::nullopt;
        }

        ndn::Name providerName;
        ndn::Name requestId;
        if (auto parsedV2 = parseResponseNameV2(responseName)) {
            providerName = parsedV2->providerName;
            requestId = parsedV2->requestId;
        }
        else {
            errorMessage = "large response reference requires a V2 response name";
            return std::nullopt;
        }
        auto decryptCompleted = std::make_shared<std::atomic<bool>>(false);
        auto decryptMutex = std::make_shared<std::mutex>();
        auto decryptCv = std::make_shared<std::condition_variable>();
        auto decryptError = std::make_shared<std::string>();
        auto plaintext = std::make_shared<ndn::Buffer>();

        auto finishDecrypt = [this, envelope, responseName, serviceName, requestId, providerName,
                              plaintext, decryptCompleted, decryptMutex, decryptCv, decryptError](
                                 const ndn::Buffer& key) mutable {
            const auto ad = hybridAssociatedData(responseName,
                                                 envelope.getMessageType(),
                                                 requestId,
                                                 serviceName,
                                                 providerName,
                                                 envelope.getKeyId(),
                                                 envelope.getEpochId());
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
                [this, envelope, serviceName, providerName, finishDecrypt, decryptCompleted,
                 decryptMutex, decryptCv, decryptError]() mutable {
                    const auto keyDataName = makeHybridMessageKeyDataName(
                        serviceName, providerName,
                        std::string("/PERMISSION") + serviceName.toUri(),
                        envelope.getEpochId());
                    nacConsumer.consume(
                        keyDataName,
                        makeNacInlineContentBlock(envelope.getWrappedMessageKey()),
                        [this, envelope, finishDecrypt](const ndn::Buffer& unwrappedKey) mutable {
                            m_hybridMessageCrypto.cacheReceiveKey(envelope.getKeyId(),
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
                [this, serviceName, providerName, envelope, finishDecrypt, decryptCompleted,
                 decryptMutex, decryptCv, decryptError]() mutable {
                    const auto keyDataName = makeHybridMessageKeyDataName(
                        serviceName, providerName,
                        std::string("/PERMISSION") + serviceName.toUri(),
                        envelope.getEpochId());
                    nacConsumer.consume(
                        keyDataName,
                        [this, envelope, finishDecrypt](const ndn::Buffer& unwrappedKey) mutable {
                            m_hybridMessageCrypto.cacheReceiveKey(envelope.getKeyId(),
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

        const auto decryptDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::unique_lock<std::mutex> decryptLock(*decryptMutex);
        decryptCv->wait_until(decryptLock, decryptDeadline,
                              [&decryptCompleted] { return decryptCompleted->load(); });
        if (!decryptCompleted->load()) {
            errorMessage = "large response hybrid decrypt timed out for " +
                           reference->dataName.toUri();
            return std::nullopt;
        }
        if (!decryptError->empty()) {
            errorMessage = "large response hybrid decrypt failure for " +
                           serviceName.toUri() + ": " + *decryptError;
            return std::nullopt;
        }

        if (reference->plaintextSize != 0 &&
            plaintext->size() != reference->plaintextSize) {
            errorMessage = "large response size mismatch for " + reference->dataName.toUri();
            return std::nullopt;
        }
        if (!reference->digest.empty() &&
            sha256DigestString(*plaintext) != reference->digest) {
            errorMessage = "large response digest mismatch for " + reference->dataName.toUri();
            return std::nullopt;
        }

        ResponseMessage resolved(responseMessage);
        ndn::Buffer payload(*plaintext);
        resolved.setPayload(payload, payload.size());
        NDN_LOG_INFO("LARGE_RESPONSE_REFERENCE_RESOLVED"
                     << " name=" << reference->dataName.toUri()
                     << " serviceName=" << serviceName.toUri()
                     << " plaintextBytes=" << payload.size());
        return resolved;
    }

    void ServiceUser::dispatchDecryptedResponseByName(const ndn::Name& responseName,
                                                      const ndn::Name& requestId,
                                                      const ndn::Buffer& buffer,
                                                      const std::string& dataName,
                                                      const std::string& signerCertificate,
                                                      const std::string& wireDigest)
    {
        auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());
        auto decodeAndFinish = [this, responseName, requestId, raw, dataName,
                                signerCertificate, wireDigest]() mutable {
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
                dataName, signerCertificate, wireDigest);

            boost::asio::post(m_face.getIoContext(),
                [this, responseName, requestId,
                 raw,
                 responseMessage = std::move(responseMessage)]() mutable {
                    finishDecryptedResponseByName(responseName, requestId,
                                                  std::move(responseMessage));
                });
        };

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
                dataName, signerCertificate, wireDigest);
            finishDecryptedResponseByName(responseName, requestId,
                                          std::move(responseMessage));
        }
    }

    void ServiceUser::finishDecryptedResponseByName(
        const ndn::Name& responseName,
        const ndn::Name&,
        ndn_service_framework::ResponseMessage responseMessage)
    {
        ndn::Name serviceName;
        ndn::Name requestId;
        ndn::Name providerName;
        auto parsedV2 = ndn_service_framework::parseResponseNameV2(responseName);
        if (!parsedV2) {
            NDN_LOG_WARN("Reject non-V2 response name: " << responseName);
            return;
        }
        serviceName = parsedV2->serviceName;
        requestId = parsedV2->requestId;
        providerName = parsedV2->providerName;
        if (parseLargeDataReferencePayload(responseMessage.getPayload())) {
            auto pendingIt = m_pendingCalls.find(requestId);
            if (pendingIt == m_pendingCalls.end()) {
                ++m_runtimeDiagnostics.callbackSkippedNoPending;
                ++m_runtimeDiagnostics.responseAfterPendingTimeout;
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_LARGE_REFERENCE_SKIPPED"
                              << " timestamp_us=" << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " reason=no_pending_call"
                              << " responseName=" << responseName.toUri());
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
                                if (!handleDecryptedResponseByName(responseName, failure)) {
                                    NDN_LOG_DEBUG("OnResponse: no pending async callback for "
                                                  << responseName);
                                }
                                return;
                            }
                            if (!handleDecryptedResponseByName(responseName, *resolved)) {
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
        const ndn_service_framework::RequestAckMessage& ackMessage)
    {
        auto parsedV2 = ndn_service_framework::parseRequestAckNameV2(ackName);
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

            if (!hasUserPermissionForProvider(parsedV2->providerName,
                                              pendingCall->second.serviceName)) {
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
            if (!isAcceptablePolicyEpoch(ackMessage.getPolicyEpoch())) {
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

            if (m_useTokens &&
                ackMessage.getStatus() && ackMessage.getProviderToken().empty()) {
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

            pendingCall->second.providerTokens[parsedV2->providerName.toUri()] =
                ackMessage.getProviderToken();
            const uint64_t ackStartUs = pendingCall->second.publishedAtUs != 0 ?
                pendingCall->second.publishedAtUs : pendingCall->second.createdAtUs;
            if (ackStartUs != 0 && ackReceiveUs >= ackStartUs) {
                m_networkTelemetry.updateAckRtt(
                    parsedV2->providerName,
                    parsedV2->serviceName,
                    static_cast<double>(ackReceiveUs - ackStartUs) / 1000.0);
            }
            pendingCall->second.requestAcks.push_back(
                StoredAck{parsedV2->providerName,
                          parsedV2->serviceName,
                          parsedV2->requestId,
                          ackMessage});
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
                        handleAckCollectionTimeout(parsedV2->requestId);
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
        return false;
    }

    bool ServiceUser::handleRequestAckByName(const ndn::Name& ackName,
                                             const ndn::Block& ackBlock)
    {
        ndn_service_framework::RequestAckMessage ackMessage;
        if (!ackMessage.WireDecode(ackBlock)) {
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
        message.setPolicyEpoch(m_currentPolicyEpoch);
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
                                  successfulAckCount)}});
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
        selected = selected && hasSelectedCandidate;

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
                                  pendingCall->second.successfulAckProviders.size())}});
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
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=COLLAB_ACK_CLOSED"
                  << " timestamp_us=" << closure.closedAtUs
                  << " requestId=" << requestId.toUri()
                  << " candidateCount=" << closure.candidates.size()
                  << " digest=" << closure.digest);
        try {
            pendingCall.collaborationAckClosedHandler(closure);
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

    bool ServiceUser::handleAckCollectionTimeout(const ndn::Name& requestId)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
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
            pendingCall->second.ackDecryptsInFlight > 0 &&
            pendingCall->second.ackSelectionDeferrals < 5) {
            ++pendingCall->second.ackSelectionDeferrals;
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=ACK_SELECTION_DEFERRED_IN_FLIGHT timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " inFlight=" << pendingCall->second.ackDecryptsInFlight
                      << " deferrals=" << pendingCall->second.ackSelectionDeferrals);
            m_scheduler.schedule(ndn::time::milliseconds(20), [this, requestId]() {
                handleAckCollectionTimeout(requestId);
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

    bool ServiceUser::evaluateCustomAckSelection(PendingCall& pendingCall)
    {
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

            const auto selectedParticipants =
                pendingCall.collaborationDeferred ?
                pendingCall.collaborationCommittedParticipants :
                pendingCall.collaborationPlan.participantSelector->select(
                    candidates, pendingCall.collaborationPlan.roles);
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
                for (const auto& storedAck : candidateAcks) {
                    if (!storedAck.providerName.equals(participant.provider) ||
                        !storedAck.serviceName.equals(participant.service) ||
                        !storedAck.requestId.equals(participant.ack.requestId) ||
                        !ackEquals(storedAck.message, participant.ack.ack) ||
                        !storedAck.message.getStatus()) {
                        continue;
                    }

                    pendingCall.customSelectedAcks.push_back(storedAck);
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
                    if (pendingCall.collaborationDeferred) {
                        CollaborationAssignmentEnvelope envelope;
                        envelope.role = participant.role;
                        envelope.assignedArtifact = participant.assignedArtifact;
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
                        envelope.opaquePayload = std::move(assignment);
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
                    NDN_LOG_INFO("NDNSF_COLLAB_ASSIGNMENT_SELECTED requestId="
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

            const auto selectedCandidates = pendingCall.ackCandidatesHandler(candidates);
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

                    pendingCall.customSelectedAcks.push_back(storedAck);
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

            const auto selectedMessages = pendingCall.acksHandler(ackMessages);
            for (const auto& selectedMessage : selectedMessages) {
                const auto* storedAck = findStoredAck(pendingCall, selectedMessage);
                if (storedAck == nullptr || !storedAck->message.getStatus()) {
                    continue;
                }

                pendingCall.customSelectedAcks.push_back(*storedAck);
                addUniqueName(pendingCall.successfulAckProviders, storedAck->providerName);
                addUniqueName(pendingCall.expectedResponseProviders, storedAck->providerName);
                if (pendingCall.selectedProvider.empty()) {
                    pendingCall.selectedProvider = storedAck->providerName;
                }
            }
        }

        if (usesR1ReservationSelection(pendingCall)) {
            // The timeout-closure path publishes one exact-target decision
            // for every reservation-bearing positive ACK.
        }
        else if (pendingCall.isCollaboration &&
            pendingCall.customSelectedAcks.size() > 1) {
            // A collaboration assignment is an independently authorized
            // provider projection.  Combining every opaque assignment into
            // one hybrid/SVS publication can exceed NDN's single-packet wire
            // budget after encryption, key wrapping, and signatures.  Keep
            // one durable request and one Selection phase, but publish one
            // bounded provider-specific projection per selected participant.
            for (const auto& selectedAck : pendingCall.customSelectedAcks) {
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

        return !pendingCall.customSelectedAcks.empty();
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
            return !pendingCall.expectedResponseProviders.empty();
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
        const auto expectedController = extractPermissionControllerIdentity(interest);
        validator->validate(
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

    void ServiceUser::fetchPolicyManifestFromController(const ndn::Name& controllerPrefix,
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
        const auto expectedController = extractPermissionControllerIdentity(interest);
        validator->validate(
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

    void ServiceUser::OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) {
            return;
        }
        // log message
        NDN_LOG_DEBUG("OnRequestAck: " << subscription.name);

        auto ackV2 = parseRequestAckNameV2(subscription.name);
        if (ackV2) {
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
            if (m_timelineTrace) {
                logTimelineTrace("user", "first_ack_observed", ackV2->requestId,
                                 {{"providerName", ackV2->providerName.toUri()},
                                  {"serviceName", ackV2->serviceName.toUri()},
                                  {"ackName", subscription.name.toUri()}});
            }
            logAckMatchAttempt(ackV2->requestId,
                               subscription.name,
                               ackV2->providerName,
                               ackReceiveUs,
                               "pre_decrypt");
            auto pendingCall = m_pendingCalls.find(ackV2->requestId);
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
            if (pendingCall == m_pendingCalls.end() ||
                pendingCall->second.hasResponse ||
                pendingCall->second.timedOut ||
                ((pendingCall->second.providerSelected ||
                  !pendingCall->second.selectedProvider.empty()) &&
                 !mayCollectResponseRetryCandidate)) {
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

            const bool collectAckCandidates =
                shouldTrackAckDecrypt(pendingCall->second);
            if (collectAckCandidates) {
                const auto deadlineUs = pendingCall->second.ackWindowDeadlineUs;
                if (pendingCall->second.ackWindowExpired &&
                    deadlineUs > 0 &&
                    ackReceiveUs > deadlineUs) {
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
                    OnRequestAckDecryptionSuccessCallback(ackV2->providerName,
                                                          ackV2->serviceName,
                                                          ackV2->requestId,
                                                          plaintext);
                    return;
                }
                if (decryptHybridMessage(
                        subscription.name,
                        ndn::Block(subscription.data),
                        [this, providerName = ackV2->providerName,
                         serviceName = ackV2->serviceName,
                         requestId = ackV2->requestId,
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
                            OnRequestAckDecryptionSuccessCallback(providerName,
                                                                  serviceName,
                                                                  requestId,
                                                                  buffer);
                        },
                        [this, providerName = ackV2->providerName,
                         serviceName = ackV2->serviceName,
                         requestId = ackV2->requestId,
                         subscriptionName = ndn::Name(subscription.name),
                         decryptStartUs](const std::string& error) {
                            const auto decryptEndUs = nowMicroseconds();
                            logCryptoDiag("user", "ack", "decrypt", "hybrid",
                                          "failure", decryptStartUs, decryptEndUs,
                                          subscriptionName, 0, error);
                            OnRequestAckDecryptionErrorCallback(providerName,
                                                                serviceName,
                                                                requestId,
                                                                error);
                        })) {
                    return;
                }
                OnRequestAckDecryptionErrorCallback(ackV2->providerName,
                                                    ackV2->serviceName,
                                                    ackV2->requestId,
                                                    "invalid hybrid ACK envelope");
                return;
                nacConsumer.consume(
                            ndn::Name(subscription.name),
                            makeNacInlineContentBlock(subscription.data),
                            [this, providerName = ackV2->providerName,
                             serviceName = ackV2->serviceName,
                             requestId = ackV2->requestId,
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
                                                                      buffer);
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
            }else{
                const auto decryptStartUs = nowMicroseconds();
                nacConsumer.consume(
                            ndn::Name(subscription.name),
                            [this, providerName = ackV2->providerName,
                             serviceName = ackV2->serviceName,
                             requestId = ackV2->requestId,
                             subscriptionName = ndn::Name(subscription.name),
                             decryptStartUs](const ndn::Buffer& buffer) {
                                const auto decryptEndUs = nowMicroseconds();
                                logCryptoDiag("user", "ack", "decrypt", "normal",
                                              "success", decryptStartUs, decryptEndUs,
                                              subscriptionName, buffer.size());
                                OnRequestAckDecryptionSuccessCallback(providerName,
                                                                      serviceName,
                                                                      requestId,
                                                                      buffer);
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

        NDN_LOG_WARN("Reject non-V2 ACK name: " << subscription.name);
    }

    void ServiceUser::OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) {
            return;
        }

        NDN_LOG_DEBUG("OnResponse: " << subscription.name);

        ndn::Name requesterName, providerName, ServiceName, RequestId;
        auto resultsV2 = ndn_service_framework::parseResponseNameV2(subscription.name);
        if (!resultsV2) {
            NDN_LOG_WARN("Reject non-V2 response name: " << subscription.name);
            return;
        }
        requesterName = resultsV2->requesterName;
        providerName = resultsV2->providerName;
        ServiceName = resultsV2->serviceName;
        RequestId = resultsV2->requestId;

        logValidatedPublicationAudit(
            "user", "RESPONSE", subscription,
            RequestId, ServiceName, requesterName, providerName);

        const ndn::Name responseName(subscription.name);
        auto responsePending = m_pendingCalls.find(RequestId);
        if (responsePending == m_pendingCalls.end()) {
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
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_DUPLICATE_SKIPPED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << RequestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " responseName=" << responseName.toUri());
            return;
        }
        if (expectMultipleResponses &&
            !containsName(responsePending->second.expectedResponseProviders, providerName)) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=RESPONSE_REJECTED_UNSELECTED_PROVIDER timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << RequestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " responseName=" << responseName.toUri());
            return;
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
        std::string responseDataName = responseName.toUri();
        std::string responseSignerCertificate;
        std::string responseWireDigest = sha256DigestString(
            ndn::Buffer(subscription.data.begin(), subscription.data.end()));
        if (subscription.packet) {
            responseDataName = subscription.packet->getName().toUri();
            const auto& signatureInfo = subscription.packet->getSignatureInfo();
            if (signatureInfo.hasKeyLocator() &&
                signatureInfo.getKeyLocator().getType() == ndn::tlv::Name) {
                responseSignerCertificate =
                    signatureInfo.getKeyLocator().getName().toUri();
            }
            const auto wire = subscription.packet->wireEncode();
            responseWireDigest = sha256DigestString(ndn::Buffer(
                wire.data(), wire.data() + wire.size()));
        }
        auto onSuccess = [this, responseName, RequestId, decryptStartUs,
                          responseDataName, responseSignerCertificate,
                          responseWireDigest](const ndn::Buffer& buffer) {
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
                responseSignerCertificate, responseWireDigest);
        };
        auto onError = [this, providerName, ServiceName, RequestId,
                        responseName, decryptStartUs](const std::string& error) {
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
                    responseSignerCertificate, responseWireDigest);
                return;
            }
            if (decryptHybridMessage(responseName,
                                     ndn::Block(subscription.data),
                                     onSuccess,
                                     onError)) {
                return;
            }
            onError("invalid hybrid response envelope");
            return;
            nacConsumer.consume(
                        ndn::Name(subscription.name),
                        makeNacInlineContentBlock(subscription.data),
                        onSuccess,
                        onError);
        }else{
            nacConsumer.consume(
                        ndn::Name(subscription.name),
                        onSuccess,
                        onError);
        }
    }

void ServiceUser::OnRequestAckDecryptionSuccessCallback(
    const ndn::Name& providerName,
    const ndn::Name& serviceName,
    const ndn::Name& requestID,
    const ndn::Buffer& buffer)
{
    auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());
    auto decodeAndPost = [this, providerName, serviceName, requestID, raw]() mutable {
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

        boost::asio::post(m_face.getIoContext(),
            [this, providerName, serviceName, requestID,
             AckMessage = std::move(AckMessage), error = std::move(error)]() mutable {
                if (!error.empty()) {
                    NDN_LOG_ERROR("RequestAckMessage decode failed: " << error);
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
                                            requestID, std::move(AckMessage));
            });
    };

    const bool queued =
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
    ndn_service_framework::RequestAckMessage AckMessage)
{
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
        if (!handleRequestAckByName(ackName, AckMessage)) {
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
        // log error
        NDN_LOG_ERROR("OnRequestAckDecryptionErrorCallback: "
                      << providerName.toUri() << serviceName.toUri()
                      << requestID.toUri() << " with error: " << error);
        auto pendingCall = m_pendingCalls.find(requestID);
        if (pendingCall != m_pendingCalls.end() &&
            (pendingCall->second.acksHandler ||
             pendingCall->second.ackCandidatesHandler) &&
            pendingCall->second.ackDecryptsInFlight > 0) {
            --pendingCall->second.ackDecryptsInFlight;
        }
    }

    void ServiceUser::PublishServiceSelectionMessageV2(const ndn::Name& providerName,
                                                          const ndn::Name& serviceName,
                                                          const ndn::Name& requestId)
    {
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
        selectionMessage.setPolicyEpoch(m_currentPolicyEpoch);
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
            if (pendingIt->second.requestMessage.hasDeploymentIntent()) {
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
            auto assignmentIt =
                pendingIt->second.collaborationAssignments.find(providerName.toUri());
            if (assignmentIt != pendingIt->second.collaborationAssignments.end()) {
                providerEntry.assignmentPayload = assignmentIt->second;
                NDN_LOG_INFO("NDNSF_SELECTION_ASSIGNMENT_ATTACHED requestId="
                             << requestId.toUri()
                             << " providerName=" << providerName.toUri()
                             << " serviceName=" << serviceName.toUri()
                             << " payloadBytes="
                             << providerEntry.assignmentPayload.size()
                             << " compact=0");
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
        selectionMessage.addProviderEntry(providerEntry);
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

        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_PUBLISH_ATTEMPT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " messageName=" << serviceSelectionName.toUri());
        try {
            PublishMessage(serviceSelectionName,
                           serviceSelectionNameWithoutPrefix,
                           selectionMessage);
        }
        catch (const std::exception& e) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_PUBLISH_FAILED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " reason=exception"
                      << " error=" << e.what());
            throw;
        }
        if (pendingIt != m_pendingCalls.end()) {
            pendingIt->second.selectionPublishedAtUs = nowMicroseconds();
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
            if (pendingIt->second.trackSelectionStatus &&
                pendingIt->second.selectionStatusOptions.enabled) {
                scheduleSelectionStatusQuery(requestId, providerName, selectionDigest);
            }
        }
        updateRequestLifecycleState(requestId, RequestLifecycleState::SELECTION_PUBLISHED);
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
        selectionMessage.setPolicyEpoch(m_currentPolicyEpoch);
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
        std::map<std::string, std::string> sharedScopeKeyFields;
        for (const auto& field :
             parseSemicolonFields(
                 pendingIt->second.collaborationPlan.sharedAssignmentMetadata)) {
            static const std::string scopeKeyPrefix = "scopeKeyData.";
            if (field.first.rfind(scopeKeyPrefix, 0) == 0 &&
                !field.first.substr(scopeKeyPrefix.size()).empty() &&
                !field.second.empty()) {
                sharedScopeKeyFields[field.first] = field.second;
            }
        }
        for (const auto& selectedAck : selectedAcks) {
            SelectionProviderEntry entry;
            entry.providerName = selectedAck.providerName;
            auto tokenIt =
                pendingIt->second.providerTokens.find(selectedAck.providerName.toUri());
            if (m_useTokens && tokenIt == pendingIt->second.providerTokens.end()) {
                for (const auto& fallbackAck : selectedAcks) {
                    PublishServiceSelectionMessageV2(fallbackAck.providerName,
                                                     fallbackAck.serviceName,
                                                     fallbackAck.requestId);
                }
                return;
            }
            if (m_useTokens) {
                entry.providerTokenHash =
                    computeSelectionProviderTokenProofHash(identity,
                                                           selectedAck.providerName,
                                                           serviceName,
                                                           tokenIt->second);
            }
            auto assignmentIt =
                pendingIt->second.collaborationAssignments.find(
                    selectedAck.providerName.toUri());
            if (assignmentIt != pendingIt->second.collaborationAssignments.end()) {
                entry.assignmentPayload = assignmentIt->second;
                NDN_LOG_INFO("NDNSF_SELECTION_ASSIGNMENT_ATTACHED requestId="
                             << requestId.toUri()
                             << " providerName=" << selectedAck.providerName.toUri()
                             << " serviceName=" << serviceName.toUri()
                             << " payloadBytes="
                             << entry.assignmentPayload.size()
                             << " compact=1");
                for (const auto& field : parseSemicolonFields(assignmentIt->second)) {
                    static const std::string scopeKeyPrefix = "scopeKeyData.";
                    if (field.first.rfind(scopeKeyPrefix, 0) == 0 && !field.second.empty()) {
                        sharedScopeKeyFields[field.first] = field.second;
                    }
                }
            }
            auto genericAssignmentIt =
                pendingIt->second.selectionAssignmentPayloads.find(
                    selectedAck.providerName.toUri());
            if (genericAssignmentIt != pendingIt->second.selectionAssignmentPayloads.end()) {
                entry.assignmentPayload =
                    mergeSelectionAssignmentPayloads(entry.assignmentPayload,
                                                     genericAssignmentIt->second);
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_ASSIGNMENT_PAYLOAD_ATTACHED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << requestId.toUri()
                          << " providerName=" << selectedAck.providerName.toUri()
                          << " serviceName=" << serviceName.toUri()
                          << " payloadBytes=" << entry.assignmentPayload.size()
                          << " compact=1");
            }
            selectionMessage.addProviderEntry(entry);
        }
        if (!sharedScopeKeyFields.empty()) {
            std::string payload;
            for (const auto& field : sharedScopeKeyFields) {
                payload += field.first + "=" + field.second + ";";
            }
            selectionMessage.setAssignmentPayload(
                ndn::Buffer(reinterpret_cast<const uint8_t*>(payload.data()),
                            payload.size()));
        }

        const auto selectionName =
            makeCompactServiceSelectionNameV2(identity, serviceName, requestId);
        const auto selectionNameWithoutPrefix =
            makeCompactServiceSelectionNameWithoutPrefixV2(serviceName, requestId);
        const std::string selectionDigest = computeSelectionDigest(selectionMessage);

        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=COMPACT_SELECTION_PUBLISH_ATTEMPT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " selectedCount=" << selectedAcks.size()
                  << " messageName=" << selectionName.toUri());
        PublishMessage(selectionName, selectionNameWithoutPrefix, selectionMessage);

        pendingIt->second.selectionPublishedAtUs = nowMicroseconds();
        for (const auto& selectedAck : selectedAcks) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=CUSTOM_ACK_SELECTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << selectedAck.requestId.toUri()
                      << " providerName=" << selectedAck.providerName.toUri()
                      << " serviceName=" << selectedAck.serviceName.toUri()
                      << " providerTokenPresent="
                      << (pendingIt->second.providerTokens.find(
                              selectedAck.providerName.toUri()) !=
                          pendingIt->second.providerTokens.end())
                      << " compactSelection=1");
            addUniqueName(pendingIt->second.selectionPublishedProviders,
                          selectedAck.providerName);
            pendingIt->second.selectionDigestsByProvider[selectedAck.providerName.toUri()] =
                selectionDigest;
            SelectionExecutionStatus status;
            status.providerName = selectedAck.providerName;
            status.serviceName = serviceName;
            status.requestId = requestId;
            status.selectionDigest = selectionDigest;
            status.state = SelectionExecutionState::Unknown;
            status.message = "compact selection published; awaiting provider status";
            status.updatedAtUs = nowMicroseconds();
            pendingIt->second.selectionStatusesByProvider[selectedAck.providerName.toUri()] =
                status;
            if (pendingIt->second.trackSelectionStatus &&
                pendingIt->second.selectionStatusOptions.enabled) {
                scheduleSelectionStatusQuery(requestId,
                                             selectedAck.providerName,
                                             selectionDigest);
            }
        }
        updateRequestLifecycleState(requestId, RequestLifecycleState::SELECTION_PUBLISHED);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=COMPACT_SELECTION_PUBLISHED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " selectedCount=" << selectedAcks.size()
                  << " messageName=" << selectionName.toUri());
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
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            if (auto data = m_IMS.find(interest)) {
                dataToSend.emplace(*data);
            }
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

    void ServiceUser::onPrefixRegisterFailure(const ndn::Name &prefix, const std::string &reason)
    {
        // log error
        NDN_LOG_ERROR("Prefix registration failed for prefix " << prefix.toUri() << " reason: " << reason);
    }

    bool ServiceUser::handleProviderReadyInterest(const ndn::Interest& interest)
    {
        const auto parsed = parseProviderReadyName(interest.getName());
        if (!parsed) return false;
        nac_validator.validate(
            interest,
            [this, parsed](const ndn::Interest& validated) {
                ProviderReadyMessage ready;
                bool accepted = ready.WireDecode(validated.getApplicationParameters());
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
                            signatureInfo->getKeyLocator().getName()).toUri() != provider) {
                        accepted = false;
                        reason = "ProviderReady signer identity mismatch";
                    }
                    bool exactMember = false;
                    bool exactHandle = false;
                    for (size_t i = 0; i < DeploymentControlMessage::MAX_FIELDS; ++i) {
                        const auto prefix = "member." + std::to_string(i) + ".";
                        if (!plan.hasField(prefix + "provider")) continue;
                        if (plan.getField(prefix + "provider") == provider &&
                            plan.hasField(prefix + "role") &&
                            plan.getField(prefix + "role") == role) {
                            exactMember = true;
                            exactHandle = plan.hasField(prefix + "statusHandle") &&
                                plan.getField(prefix + "statusHandle") == parsed->controlHandle;
                            break;
                        }
                    }
                    const auto selectionIt =
                        pending->selectionDigestsByProvider.find(provider);
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
                        memberKey = provider + "|" + role;
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
                m_keyChain.sign(data, m_signingInfo);
                m_face.put(data);
                if (accepted && pending != nullptr) {
                    maybeActivateReadyDeployment(requestId, *pending);
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
        ndn::Interest command(makeExecutionActivateName(
            provider, DeploymentControlMessage::VERSION, controlHandle));
        command.setMustBeFresh(true);
        command.setCanBePrefix(false);
        command.setInterestLifetime(ndn::time::milliseconds(500));
        command.setApplicationParameters(activation.WireEncode());
        m_keyChain.sign(command, m_signingInfo);
        auto retry = [this, provider, controlHandle, activation, attempt] {
            m_scheduler.schedule(ndn::time::milliseconds(100 * (attempt + 1)),
                [this, provider, controlHandle, activation, attempt] {
                    publishExecutionActivate(provider, controlHandle, activation, attempt + 1);
                });
        };
        m_face.expressInterest(
            command,
            [this, activation, retry](const ndn::Interest&, const ndn::Data& data) {
                nac_validator.validate(
                    data,
                    [activation, retry](const ndn::Data& validated) {
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

    void ServiceUser::onInterest(const ndn::InterestFilter &, const ndn::Interest &interest)
    {
        NDN_LOG_DEBUG("Received Interest: " << interest.getName().toUri());
        if (handleProviderReadyInterest(interest)) {
            return;
        }
        replyFromIMS(interest);

    }
    void ServiceUser::serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data> &contentData, ndn::nacabe::SPtrVector<ndn::Data> &ckData)
    {
        //log data
        NDN_LOG_DEBUG("serveDataWithIMS: " << contentData.size() << " " << ckData.size());
        std::lock_guard<std::mutex> lock(_cache_mutex);
        // contentData is now served by svsps
        for (auto data : contentData)
        {
            m_IMS.insert(*data);
        }
        for (auto data : ckData)
        {
            m_IMS.insert(*data);
        }
    }

    void ServiceUser::publishHybridMessage(const ndn::Name& messageName,
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

    void ServiceUser::publishHybridEncodedMessage(const ndn::Name& messageName,
                                                  ndn::Buffer plaintext)
    {
        ndn::Name serviceName;
        ndn::Name requestId;
        ndn::Name senderPrefix = identity;
        if (auto request = parseRequestNameV2(messageName)) {
            serviceName = request->serviceName;
            requestId = request->requestId;
        }
        else if (auto selection = parseCompactServiceSelectionNameV2(messageName)) {
            serviceName = selection->serviceName;
            requestId = selection->requestId;
        }
        else if (auto selection = parseServiceSelectionNameV2(messageName)) {
            serviceName = selection->serviceName;
            requestId = selection->requestId;
        }
        else {
            NDN_LOG_ERROR("Hybrid publish unsupported message name: " << messageName);
            return;
        }

        const auto messageType = hybridMessageTypeForName(messageName);
        const auto accessAttribute = hybridAccessAttributeForName(messageName, serviceName);
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
            logTimelineTrace("user", cryptoStageForName(messageName) + "_crypto_start", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"messageName", messageName.toUri()},
                              {"mode", "hybrid"}});
        }

        // Publish the NAC-ABE wrapped MessageKey once under its deterministic
        // epoch name. Packets carry only the compact key/epoch identifiers;
        // receivers recover the key by fetching that name when needed.
        if (m_hybridMessageCrypto.shouldAttachWrappedKey(key.keyId)) {
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
                nacProducer.produce(key.keyName,
                                    std::vector<std::string>{accessAttribute},
                                    ndn::span<const uint8_t>(key.key.data(), key.key.size()),
                                    m_signingInfo);
            auto wrapped = mergeDataContents(contentData);
            if (wrapped.empty()) {
                NDN_LOG_ERROR("Hybrid MessageKey wrap produced empty content for "
                              << messageName.toUri());
                return;
            }
            ndn::Buffer wrappedBuffer(wrapped.data(), wrapped.size());
            serveDataWithIMS(contentData, ckData);
            m_hybridMessageCrypto.cacheWrappedSendKey(key.keyId, wrappedBuffer);
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
                    NDN_LOG_ERROR("[NDNSF_HYBRID] role=user event=HYBRID_PUBLISH_FAILED"
                                  << " messageName=" << messageName.toUri()
                                  << " reason=" << error);
                    return;
                }
                if (m_timelineTrace) {
                    logTimelineTrace("user", "aes_gcm_encrypt_done", requestId,
                                     {{"serviceName", serviceName.toUri()},
                                      {"messageType", messageType},
                                      {"duration_us", std::to_string(aesEndUs >= aesStartUs ?
                                                                     aesEndUs - aesStartUs : 0)}});
                    logTimelineTrace("user", cryptoStageForName(messageName) + "_crypto_done",
                                     requestId,
                                     {{"serviceName", serviceName.toUri()},
                                      {"messageName", messageName.toUri()},
                                      {"mode", "hybrid"}});
                }
                ++m_hybridCryptoCounters.symmetric_encrypt_count;
                if (m_useTokens) {
                    if (messageType == "REQUEST" || messageType == "RESPONSE") {
                        ++m_hybridCryptoCounters.user_token_symmetric_encrypt_count;
                    }
                    if (messageType == "ACK" || messageType == "SELECTION") {
                        ++m_hybridCryptoCounters.provider_token_symmetric_encrypt_count;
                    }
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
                    ndn::Name rid;
                    ndn::Name svc;
                    if (auto request = parseRequestNameV2(messageName)) {
                        rid = request->requestId;
                        svc = request->serviceName;
                    }
                    else if (auto selection = parseCompactServiceSelectionNameV2(messageName)) {
                        rid = selection->requestId;
                        svc = selection->serviceName;
                    }
                    else if (auto selection = parseServiceSelectionNameV2(messageName)) {
                        rid = selection->requestId;
                        svc = selection->serviceName;
                    }
                    if (!rid.empty()) {
                        logTimelineTrace("user", cryptoStageForName(messageName) + "_publish_start",
                                         rid,
                                         {{"serviceName", svc.toUri()},
                                          {"messageName", messageName.toUri()},
                                          {"mode", "hybrid"}});
                    }
                }
                const bool asyncPublish = useAsyncSvsPublish();
                const auto publishedSeqNo = publishSvs(m_svsps, messageName, contentBlock);
                NDN_LOG_TRACE("[NDNSF_TRACE] role=user event="
                              << (asyncPublish ? "SVS_PUBLISH_ACCEPTED" : "SVS_PUBLISH_DONE")
                              << " timestamp_us=" << nowMicroseconds()
                              << " requestId=" << requestId.toUri()
                              << " messageName=" << messageName.toUri()
                              << " seqNo=" << publishedSeqNo
                              << " mode=hybrid-message-crypto");
                if (messageType == "SELECTION" &&
                    isTruthyEnv("NDNSF_SELECTION_TARGETED_PREFETCH")) {
                    try {
                        ndn::Data directSelectionData(messageName);
                        directSelectionData.setFreshnessPeriod(ndn::time::milliseconds(
                            std::max(100, intEnvOrDefault("NDNSF_SELECTION_TARGETED_FRESHNESS_MS", 10000))));
                        directSelectionData.setContent(
                            ndn::span<const uint8_t>(buffer.data(), buffer.size()));
                        m_keyChain.sign(directSelectionData, m_signingInfo);
                        m_face.put(directSelectionData);
                        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SELECTION_DIRECT_PUT timestamp_us="
                                      << nowMicroseconds()
                                      << " requestId=" << requestId.toUri()
                                      << " serviceName=" << serviceName.toUri()
                                      << " messageName=" << messageName.toUri()
                                      << " contentBytes=" << buffer.size());
                        logControlTiming("user", "SELECTION_DIRECT_PUT", requestId,
                                         {{"serviceName", serviceName.toUri()},
                                          {"messageType", messageType},
                                          {"messageName", messageName.toUri()},
                                          {"contentBytes", std::to_string(buffer.size())}});
                    }
                    catch (const std::exception& e) {
                        NDN_LOG_WARN("[NDNSF_HYBRID] role=user event=SELECTION_DIRECT_PUT_FAILED"
                                     << " messageName=" << messageName.toUri()
                                     << " reason=" << e.what());
                    }
                }
                logControlTiming("user", "SVS_PUBLISH_DONE", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"messageType", messageType},
                                  {"messageName", messageName.toUri()},
                                  {"contentBytes", std::to_string(contentBlock.value_size())},
                                  {"mode", "hybrid-message-crypto"}});
                if (m_timelineTrace) {
                    ndn::Name rid;
                    ndn::Name svc;
                    if (auto request = parseRequestNameV2(messageName)) {
                        rid = request->requestId;
                        svc = request->serviceName;
                    }
                    else if (auto selection = parseCompactServiceSelectionNameV2(messageName)) {
                        rid = selection->requestId;
                        svc = selection->serviceName;
                    }
                    else if (auto selection = parseServiceSelectionNameV2(messageName)) {
                        rid = selection->requestId;
                        svc = selection->serviceName;
                    }
                    if (!rid.empty()) {
                        logTimelineTrace("user", cryptoStageForName(messageName) + "_publish_done",
                                         rid,
                                         {{"serviceName", svc.toUri()},
                                          {"messageName", messageName.toUri()},
                                          {"mode", "hybrid"}});
                    }
                }
            });
        };
        if (m_handlerPool.getThreadCount() == 0 ||
            !m_handlerPool.post(encryptAndPost)) {
            encryptAndPost();
        }
    }

    bool ServiceUser::decryptHybridMessage(const ndn::Name& messageName,
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
        if (auto ack = parseRequestAckNameV2(messageName)) {
            serviceName = ack->serviceName;
            requestId = ack->requestId;
            senderPrefix = ack->providerName;
        }
        else if (auto response = parseResponseNameV2(messageName)) {
            serviceName = response->serviceName;
            requestId = response->requestId;
            senderPrefix = response->providerName;
        }
        else {
            return false;
        }

        const auto accessAttribute = hybridAccessAttributeForName(messageName, serviceName);
        const auto keyDataName = makeHybridMessageKeyDataName(
            serviceName, senderPrefix, accessAttribute, envelope.getEpochId());

        auto finish = [this, envelope, messageName, serviceName, requestId,
                       senderPrefix, decryptEntryUs, onSuccess = std::move(onSuccess),
                       onError = std::move(onError)](const ndn::Buffer& key) mutable {
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
            if (decryptPool.getThreadCount() == 0 ||
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
            auto onKey = [this, envelope, finish = std::move(finish), requestId,
                          unwrapStartUs, keyDataName](const ndn::Buffer& unwrappedKey) mutable {
                                    logHybridCryptoTiming("user", "hybrid_decrypt_key_unwrap_done", requestId,
                                                          {{"messageType", envelope.getMessageType()},
                                                           {"source", envelope.hasWrappedMessageKey() ?
                                                                      "inline" : "named-fetch"},
                                                           {"keyName", keyDataName.toUri()},
                                                           {"unwrapUs", std::to_string(timelineSteadyMicroseconds() - unwrapStartUs)},
                                                           {"keyBytes", std::to_string(unwrappedKey.size())}});
                                    m_hybridMessageCrypto.cacheReceiveKey(envelope.getKeyId(),
                                                                          envelope.getEpochId(),
                                                                          unwrappedKey);
                                    finish(unwrappedKey);
                                };
            auto onKeyError = [onError = std::move(onError), keyDataName](const std::string& error) {
                                    if (onError) {
                                        onError("hybrid MessageKey " +
                                                std::string(keyDataName.empty() ? "unwrap" :
                                                            "fetch/unwrap") +
                                                " failed: " + error);
                                    }
                                };
            if (envelope.hasWrappedMessageKey()) {
                nacConsumer.consume(keyDataName,
                                    makeNacInlineContentBlock(envelope.getWrappedMessageKey()),
                                    std::move(onKey), std::move(onKeyError));
            }
            else {
                nacConsumer.consume(keyDataName, std::move(onKey), std::move(onKeyError));
            }
        }
        catch (const std::exception& e) {
            if (onError) {
                onError("hybrid MessageKey unwrap failed: " + std::string(e.what()));
            }
        }
        return true;
    }

    void ServiceUser::PublishMessage(const ndn::Name &messageName, const ndn::Name &messageNameWithoutPrefix,AbstractMessage &message)
    {
        // log message
        NDN_LOG_DEBUG("PublishMessage: " << messageName.toUri());
        if (m_svsps == nullptr) {
            NDN_LOG_DEBUG("PublishMessage skipped because SVS publisher is not initialized");
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
        if (auto request = parseRequestNameV2(messageName)) {
            timelineRequestId = request->requestId;
            timelineServiceName = request->serviceName;
        }
        else if (auto selection = parseServiceSelectionNameV2(messageName)) {
            timelineRequestId = selection->requestId;
            timelineServiceName = selection->serviceName;
        }
        const auto plaintextBlock = message.WireEncode();
        const bool usePlaintext =
            (stage == "ack" && plaintextAckDiagEnabled()) ||
            (stage == "response" && plaintextResponseDiagEnabled());
        const auto encryptStartUs = nowMicroseconds();
        if (m_timelineTrace && !timelineRequestId.empty()) {
            logTimelineTrace("user", stage + "_crypto_start", timelineRequestId,
                             {{"serviceName", timelineServiceName.toUri()},
                              {"messageName", messageName.toUri()}});
        }
        if (usePlaintext) {
            const auto encryptEndUs = nowMicroseconds();
            if (m_timelineTrace && !timelineRequestId.empty()) {
                logTimelineTrace("user", stage + "_crypto_done", timelineRequestId,
                                 {{"serviceName", timelineServiceName.toUri()},
                                  {"messageName", messageName.toUri()},
                                  {"duration_us",
                                   std::to_string(encryptEndUs >= encryptStartUs ?
                                                  encryptEndUs - encryptStartUs : 0)}});
            }
            logCryptoDiag("user", stage, "encrypt", "plaintext", "success",
                          encryptStartUs, encryptEndUs, messageName,
                          plaintextBlock.size());

            auto buffer = ndn::Buffer(plaintextBlock.begin(), plaintextBlock.end());
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SVS_PUBLISH_QUEUED timestamp_us="
                      << nowMicroseconds()
                      << " messageName=" << messageName.toUri()
                      << " contentBytes=" << buffer.size()
                      << " contentSegments=0"
                      << " ckSegments=0");
            const auto queuedAtUs = nowMicroseconds();
            boost::asio::post(m_face.getIoContext(),
                [this, messageName, queuedAtUs, buffer = std::move(buffer)]() mutable {
                    ndn::Block contentBlock(buffer);
                    const auto beginUs = nowMicroseconds();
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SVS_PUBLISH_BEGIN timestamp_us="
                              << beginUs
                              << " messageName=" << messageName.toUri()
                              << " contentBytes=" << contentBlock.value_size()
                              << " eventLoopLagUs=" << (beginUs >= queuedAtUs ?
                                                         beginUs - queuedAtUs : 0));
                    if (m_timelineTrace) {
                        ndn::Name requestId;
                        ndn::Name serviceName;
                        if (auto request = parseRequestNameV2(messageName)) {
                            requestId = request->requestId;
                            serviceName = request->serviceName;
                        }
                        else if (auto selection = parseServiceSelectionNameV2(messageName)) {
                            requestId = selection->requestId;
                            serviceName = selection->serviceName;
                        }
                        if (!requestId.empty()) {
                            logTimelineTrace("user", cryptoStageForName(messageName) + "_publish_start",
                                             requestId,
                                             {{"serviceName", serviceName.toUri()},
                                              {"messageName", messageName.toUri()}});
                        }
                    }
                    publishSvs(m_svsps, messageName, contentBlock);
                    if (m_timelineTrace) {
                        ndn::Name requestId;
                        ndn::Name serviceName;
                        if (auto request = parseRequestNameV2(messageName)) {
                            requestId = request->requestId;
                            serviceName = request->serviceName;
                        }
                        else if (auto selection = parseServiceSelectionNameV2(messageName)) {
                            requestId = selection->requestId;
                            serviceName = selection->serviceName;
                        }
                        if (!requestId.empty()) {
                            logTimelineTrace("user", cryptoStageForName(messageName) + "_publish_done",
                                             requestId,
                                             {{"serviceName", serviceName.toUri()},
                                              {"messageName", messageName.toUri()}});
                        }
                    }
                    NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SVS_PUBLISH_DONE timestamp_us="
                              << nowMicroseconds()
                              << " messageName=" << messageName.toUri());
                    NDN_LOG_TRACE("Message Published: " << messageName.toUri()
                                 << " " << contentBlock.value_size());
                });
            return;
        }

        std::vector<uint8_t> plaintext(plaintextBlock.begin(), plaintextBlock.end());
        ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=PRODUCE_STARTED timestamp_us="
                  << nowMicroseconds()
                  << " messageName=" << messageName.toUri()
                  << " stage=" << stage
                  << " mode=synchronous-user-publish"
                  << " plaintextBytes=" << plaintext.size());
        try {
            std::tie(contentData, ckData) =
                nacProducer.produce(
                    messageNameWithoutPrefix,
                    *results,
                    ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                    m_signingInfo);
            const auto encryptEndUs = nowMicroseconds();
            if (m_timelineTrace && !timelineRequestId.empty()) {
                logTimelineTrace("user", stage + "_crypto_done", timelineRequestId,
                                 {{"serviceName", timelineServiceName.toUri()},
                                  {"messageName", messageName.toUri()},
                                  {"duration_us",
                                   std::to_string(encryptEndUs >= encryptStartUs ?
                                                  encryptEndUs - encryptStartUs : 0)}});
            }
            logCryptoDiag("user", stage, "encrypt",
                          "synchronous-user-publish", "success",
                          encryptStartUs, encryptEndUs,
                          messageName, plaintext.size());
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=PRODUCE_COMPLETED timestamp_us="
                      << encryptEndUs
                      << " messageName=" << messageName.toUri()
                      << " stage=" << stage
                      << " mode=synchronous-user-publish"
                      << " contentSegments=" << contentData.size()
                      << " ckSegments=" << ckData.size());
        }
        catch (const std::exception& e) {
            const auto encryptEndUs = nowMicroseconds();
            if (m_timelineTrace && !timelineRequestId.empty()) {
                logTimelineTrace("user", stage + "_crypto_done", timelineRequestId,
                                 {{"serviceName", timelineServiceName.toUri()},
                                  {"messageName", messageName.toUri()},
                                  {"status", "failure"},
                                  {"duration_us",
                                   std::to_string(encryptEndUs >= encryptStartUs ?
                                                  encryptEndUs - encryptStartUs : 0)}});
            }
            logCryptoDiag("user", stage, "encrypt",
                          "synchronous-user-publish", "failure",
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
            NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=PRODUCE_EMPTY_CONTENT timestamp_us="
                      << nowMicroseconds()
                      << " messageName=" << messageName.toUri()
                      << " stage=" << stage
                      << " mode=synchronous-user-publish"
                      << " contentSegments=" << contentData.size()
                      << " ckSegments=" << ckData.size());
            return;
        }
        const auto queuedAtUs = nowMicroseconds();
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SVS_PUBLISH_QUEUED timestamp_us="
                  << queuedAtUs
                  << " messageName=" << messageName.toUri()
                  << " contentBytes=" << buffer.size()
                  << " contentSegments=" << contentData.size()
                  << " ckSegments=" << ckData.size()
                  << " mode=synchronous-user-publish");

        serveDataWithIMS(contentData, ckData);
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=IMS_INSERT_DONE timestamp_us="
                  << nowMicroseconds()
                  << " messageName=" << messageName.toUri()
                  << " contentSegments=" << contentData.size()
                  << " ckSegments=" << ckData.size()
                  << " mode=synchronous-user-publish");
        ndn::Block contentBlock(buffer);
        const auto beginUs = nowMicroseconds();
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SVS_PUBLISH_BEGIN timestamp_us="
                  << beginUs
                  << " messageName=" << messageName.toUri()
                  << " contentBytes=" << contentBlock.value_size()
                  << " eventLoopLagUs=" << (beginUs >= queuedAtUs ?
                                             beginUs - queuedAtUs : 0)
                  << " mode=synchronous-user-publish");
        if (m_timelineTrace && !timelineRequestId.empty()) {
            logTimelineTrace("user", stage + "_publish_start", timelineRequestId,
                             {{"serviceName", timelineServiceName.toUri()},
                              {"messageName", messageName.toUri()}});
        }
        publishSvs(m_svsps, messageName, contentBlock);
        if (m_timelineTrace && !timelineRequestId.empty()) {
            logTimelineTrace("user", stage + "_publish_done", timelineRequestId,
                             {{"serviceName", timelineServiceName.toUri()},
                              {"messageName", messageName.toUri()}});
        }
        NDN_LOG_TRACE("[NDNSF_TRACE] role=user event=SVS_PUBLISH_DONE timestamp_us="
                  << nowMicroseconds()
                  << " messageName=" << messageName.toUri()
                  << " mode=synchronous-user-publish");
        NDN_LOG_TRACE("Message Published: " << messageName.toUri()
                     << " " << contentBlock.value_size());
    }
    void ServiceUser::registerNDNSFMessages()
    {

        // log register
        NDN_LOG_INFO("Register NDNSF Messages in ndn-svs");

        // V2 ACK/RESPONSE subscriptions match every provider and parse counted
        // requester/service names from the message body.
        std::string regex_str = "^(<>*)<NDNSF><ACK>(<>*)$";
        NDN_LOG_INFO(regex_str);
        m_svsps->subscribeWithRegex(ndn::Regex(regex_str),
                                    std::bind(&ServiceUser::OnRequestAck, this, _1),
                                    true, false);
        std::string regex_str2 = "^(<>*)<NDNSF><RESPONSE>(<>*)$";
        NDN_LOG_INFO(regex_str2);
        m_svsps->subscribeWithRegex(ndn::Regex(regex_str2),
                                    std::bind(&ServiceUser::OnResponse, this, _1),
                                    true, false);

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
