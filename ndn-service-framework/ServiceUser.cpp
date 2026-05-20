#include "ServiceUser.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.ServiceUser);

    namespace
    {
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
                if (component == "COORDINATION") {
                    return "coordination";
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
    }

    ServiceUser::ServiceUser(ndn::Face &face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath) : 
        m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(identityCert.getIdentity()),
        validator(std::make_shared<MessageValidator>(trustSchemaPath)),
        identityCert(identityCert),
        // nac_validator(std::move(ndn::security::ValidatorNull())),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        m_IMS(6000)
    {
        if (std::getenv("NDNSF_DISABLE_NDNSD") == nullptr) {
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

        m_signingInfo = ndn::security::signingByCertificate(identityCert);

        ndn::svs::SecurityOptions secOpts(m_keyChain);
        secOpts.interestSigner = std::make_shared<CommandInterestSigner>(m_keyChain);
        secOpts.interestSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.interestSigner->signingInfo.setSigningKeyName(identityCert.getKeyName());
        secOpts.dataSigner->signingInfo.setSigningCertName(identityCert.getName());
        secOpts.dataSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.pubSigner->signingInfo.setSigningCertName(identityCert.getName());
        secOpts.pubSigner->signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);
        secOpts.validator = validator;
        secOpts.encapsulatedDataValidator = validator;

        // Do not fetch publications older than 10 seconds
        ndn::svs::SVSPubSubOptions opts;
        #ifdef USE_TIMESTAMP
        opts.useTimestamp = true;
        // opts.maxPubAge = ndn::time::seconds(0);
        #else
        opts.useTimestamp = false;
        #endif

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
            const bool enableParallelSync =
                std::getenv("NDNSF_SVS_PARALLEL_SYNC") == nullptr ||
                isTruthyEnv("NDNSF_SVS_PARALLEL_SYNC");
            if (enableParallelSync) {
                const int workers = std::max(1, intEnvOrDefault("NDNSF_SVS_PARALLEL_WORKERS", 2));
                const int queue = std::max(1, intEnvOrDefault("NDNSF_SVS_PARALLEL_QUEUE", 256));
                m_svsps->getSVSync().getCore().setParallelSyncProcessing(
                    true, static_cast<size_t>(workers), static_cast<size_t>(queue));
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_SYNC enabled role=user workers="
                             << workers << " queue=" << queue);
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
        : m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(identityCert.getIdentity()),
        m_keyChain(),
        m_svsps(nullptr),
        validator(std::make_shared<MessageValidator>(trustSchemaPath)),
        nac_validator(m_face),
        identityCert(identityCert),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        m_IMS(6000),
        m_configManager("/tmp/ndnsf-service-user-local-mock.conf")
    {
        m_signingInfo = ndn::security::signingByCertificate(identityCert);
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
                         << " mainBlockingMs=" << stats.syncInterestMainThreadBlockingMs);
        }
        m_handlerPool.shutdown();
    }

    void ServiceUser::setRequestPublisher(RequestPublisher publisher)
    {
        m_requestPublisher = std::move(publisher);
    }

    void ServiceUser::setRequestLifecycleCallback(RequestLifecycleCallback callback)
    {
        m_requestLifecycleCallback = std::move(callback);
    }

    void ServiceUser::setHandlerThreads(size_t n)
    {
        m_handlerPool.setThreadCount(n);
        NDN_LOG_WARN("Response callback worker threads: " << n);
    }

    size_t ServiceUser::getHandlerThreads() const
    {
        return m_handlerPool.getThreadCount();
    }

    size_t ServiceUser::getHandlerQueueDepth() const
    {
        return m_handlerPool.getQueueSize();
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
        case RequestLifecycleState::COORDINATION_PUBLISHED: return "COORDINATION_PUBLISHED";
        case RequestLifecycleState::RESPONSE_OBSERVED: return "RESPONSE_OBSERVED";
        case RequestLifecycleState::RESPONSE_DECRYPTED: return "RESPONSE_DECRYPTED";
        case RequestLifecycleState::CALLBACK_FIRED: return "CALLBACK_FIRED";
        case RequestLifecycleState::COMPLETED: return "COMPLETED";
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
        m_adaptiveAdmissionWindow = m_adaptiveAdmissionOptions.enabled ?
            m_adaptiveAdmissionOptions.initialWindow :
            m_adaptiveAdmissionOptions.maxWindow;
        m_adaptiveAdmissionBaselineLatencyMs = 0.0;
        m_adaptiveAdmissionPreviousQueueDelayMs = 0.0;
        m_adaptiveAdmissionQueueDelayOverTargetIntervals = 0;
        NDN_LOG_WARN("Adaptive admission control: "
                     << (m_adaptiveAdmissionOptions.enabled ? "enabled" : "disabled")
                     << " window=" << m_adaptiveAdmissionWindow
                     << " min=" << m_adaptiveAdmissionOptions.minWindow
                     << " max=" << m_adaptiveAdmissionOptions.maxWindow
                     << " hardInflight=" << m_adaptiveAdmissionOptions.hardInflightLimit
                     << " controlIntervalMs="
                     << m_adaptiveAdmissionOptions.controlIntervalMs
                     << " targetLatencyMs="
                     << m_adaptiveAdmissionOptions.targetLatencyMs);
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
            if (status.coordinationPublishTimestampUs == 0) {
                status.coordinationPublishTimestampUs = pending.coordinationPublishedAtUs;
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
        case RequestLifecycleState::COORDINATION_PUBLISHED:
            status.coordinationPublishTimestampUs = nowUs;
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
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_LIFECYCLE_STATE timestamp_us="
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
    }

    void ServiceUser::setPendingCallTimeoutGrace(ndn::time::milliseconds grace)
    {
        m_pendingCallTimeoutGrace = std::max(ndn::time::milliseconds(0), grace);
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

    void ServiceUser::setUseHybridMessageCrypto(bool enabled)
    {
        m_useHybridMessageCrypto = enabled;
        NDN_LOG_WARN("Hybrid message crypto: "
                     << (m_useHybridMessageCrypto ? "enabled" : "disabled"));
    }

    bool ServiceUser::getUseHybridMessageCrypto() const
    {
        return m_useHybridMessageCrypto;
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
        case AckSelectionStrategy::AllResponders:
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
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=PENDING_CLEANUP timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " strategyState=" << (m_strategyMap.find(requestId) != m_strategyMap.end())
                  << " ackInfoState=" << (m_AckInfoMap.find(requestId) != m_AckInfoMap.end()));
        m_strategyMap.erase(requestId);
        m_AckInfoMap.erase(requestId);
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
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_PENDING_CREATED timestamp_us="
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
        else if (record.timedOut) {
            updateRequestLifecycleState(requestId, RequestLifecycleState::TIMED_OUT, reason);
        }
        else {
            updateRequestLifecycleState(requestId, RequestLifecycleState::CANCELLED_OR_DROPPED, reason);
        }

        const char* event = record.completed ?
            "REQUEST_PENDING_COMPLETED" :
            (record.timedOut ? "REQUEST_PENDING_TIMEOUT" : "REQUEST_PENDING_ERASED");
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=" << event
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_PENDING_ERASED"
                      << " timestamp_us=" << eraseAtUs
                      << " requestId=" << requestId.toUri()
                      << " callId=" << requestId.toUri()
                      << " reason=" << reason
                      << " pendingCallsSizeBefore=" << m_pendingCalls.size()
                      << " threadId=" << currentThreadIdForTrace());
        }
        releaseAdaptiveAdmissionSlot(requestId, pendingCall->second, reason, eraseAtUs);
        m_pendingCalls.erase(pendingCall);
        cleanupPendingCallState(requestId);
    }

    bool ServiceUser::hasReachedLatePipelineStage(const PendingCall& pendingCall) const
    {
        return pendingCall.firstAckAtUs != 0 ||
               !pendingCall.requestAcks.empty() ||
               pendingCall.providerSelected ||
               !pendingCall.selectedProvider.empty() ||
               pendingCall.coordinationScheduledAtUs != 0 ||
               pendingCall.coordinationPublishedAtUs != 0 ||
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
        auto timeoutHandler = pendingCall->second.timeoutHandler;
        erasePendingCallWithTrace(requestId, pendingCall, "timeout");

        if (timeoutHandler) {
            timeoutHandler(requestId);
        }
    }

    void ServiceUser::scheduleRequestTimeout(const ndn::Name& requestId, int timeoutMs)
    {
        if (timeoutMs <= 0) {
            return;
        }

        m_scheduler.schedule(ndn::time::milliseconds(timeoutMs), [this, requestId]() {
            auto pendingCall = m_pendingCalls.find(requestId);
            if (pendingCall == m_pendingCalls.end()) {
                return;
            }
            if (pendingCall->second.hasResponse) {
                erasePendingCallWithTrace(requestId, pendingCall, "timeout_after_response");
                return;
            }

            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=TIMEOUT_FIRED timestamp_us="
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
                      << " coordinationScheduledAtUs="
                      << pendingCall->second.coordinationScheduledAtUs
                      << " coordinationPublishedAtUs="
                      << pendingCall->second.coordinationPublishedAtUs
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

            if (hasReachedLatePipelineStage(pendingCall->second) &&
                m_pendingCallTimeoutGrace.count() > 0) {
                pendingCall->second.timeoutGraceActive = true;
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=TIMEOUT_GRACE_STARTED timestamp_us="
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

        const size_t activeLimit = std::max<size_t>(
            1,
            std::min(m_adaptiveAdmissionWindow,
                     m_adaptiveAdmissionOptions.hardInflightLimit));
        if (m_adaptiveAdmissionInflight >= activeLimit) {
            updateRequestLifecycleState(requestId, RequestLifecycleState::ADMISSION_DELAYED);
            m_adaptiveAdmissionQueue.push_back(requestId);
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ADMISSION_QUEUED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " inflight=" << m_adaptiveAdmissionInflight
                      << " window=" << m_adaptiveAdmissionWindow
                      << " hardInflight="
                      << m_adaptiveAdmissionOptions.hardInflightLimit
                      << " queueDepth=" << m_adaptiveAdmissionQueue.size());
            return;
        }

        publishAdmittedPendingCall(requestId);
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

        PublishRequestV2(providers, serviceName, requestId, payload, strategy);

        pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            return;
        }
        if (scheduleAckTimeout && !pendingCall->second.ackTimeoutScheduled) {
            pendingCall->second.ackTimeoutScheduled = true;
            if (ackTimeoutMs > 0) {
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

    void ServiceUser::drainAdaptiveAdmissionQueue()
    {
        if (!m_adaptiveAdmissionOptions.enabled) {
            return;
        }

        const size_t activeLimit = std::max<size_t>(
            1,
            std::min(m_adaptiveAdmissionWindow,
                     m_adaptiveAdmissionOptions.hardInflightLimit));
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
        const size_t activeLimit = std::max<size_t>(
            1,
            std::min(m_adaptiveAdmissionWindow,
                     m_adaptiveAdmissionOptions.hardInflightLimit));
        const bool queueBacklogged = !m_adaptiveAdmissionQueue.empty();
        const bool aboveWindow = m_adaptiveAdmissionInflight > activeLimit;
        const double averageLatencyMs =
            m_adaptiveAdmissionIntervalLatencyCount == 0 ? 0.0 :
            m_adaptiveAdmissionIntervalLatencySumMs /
                static_cast<double>(m_adaptiveAdmissionIntervalLatencyCount);
        const double targetLatencyMs =
            static_cast<double>(m_adaptiveAdmissionOptions.targetLatencyMs);
        const double p50LatencyMs =
            percentileLatency(m_adaptiveAdmissionIntervalLatenciesMs, 50.0);
        const double p95LatencyMs =
            percentileLatency(m_adaptiveAdmissionIntervalLatenciesMs, 95.0);
        const double maxLatencyMs =
            m_adaptiveAdmissionIntervalLatenciesMs.empty() ? 0.0 :
            *std::max_element(m_adaptiveAdmissionIntervalLatenciesMs.begin(),
                              m_adaptiveAdmissionIntervalLatenciesMs.end());
        if (p50LatencyMs > 0.0) {
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
        const double queueDelayTargetMs = std::max(50.0, 0.35 * targetLatencyMs);
        const double queueDelaySevereMs = std::max(100.0, 0.75 * targetLatencyMs);
        if (queueDelayMs > queueDelayTargetMs) {
            ++m_adaptiveAdmissionQueueDelayOverTargetIntervals;
        }
        else {
            m_adaptiveAdmissionQueueDelayOverTargetIntervals = 0;
        }
        const bool queuePressure =
            (queueBacklogged &&
            m_adaptiveAdmissionInflight >=
                static_cast<size_t>(std::ceil(static_cast<double>(activeLimit) * 0.8)));
        const bool queueSevere =
            m_adaptiveAdmissionQueue.size() >= m_adaptiveAdmissionOptions.hardInflightLimit;
        const bool demandBacklogged =
            queueBacklogged || m_adaptiveAdmissionIntervalBackpressure > 0;
        const bool latencyCongested =
            queueDelayMs > queueDelayTargetMs &&
            (queueDelayGradientMs > 0.0 ||
             (demandBacklogged &&
              m_adaptiveAdmissionQueueDelayOverTargetIntervals >= 2));
        const bool latencySevere =
            queueDelayMs > queueDelaySevereMs ||
            (maxLatencyMs > 0.0 && maxLatencyMs > 2.0 * targetLatencyMs);
        if (latencyCongested) {
            m_adaptiveAdmissionIntervalCongested = true;
        }
        if (latencySevere || queueSevere) {
            m_adaptiveAdmissionIntervalSevere = true;
        }

        if (m_adaptiveAdmissionIntervalTimeouts > 0) {
            m_adaptiveAdmissionWindow = std::max(
                m_adaptiveAdmissionOptions.minWindow,
                static_cast<size_t>(std::ceil(
                    static_cast<double>(m_adaptiveAdmissionWindow) *
                    m_adaptiveAdmissionOptions.severeMdFactor)));
        }
        else if (m_adaptiveAdmissionIntervalCongested) {
            const size_t decreaseStep = m_adaptiveAdmissionOptions.aiStep *
                (m_adaptiveAdmissionIntervalSevere ? 2 : 1);
            m_adaptiveAdmissionWindow = std::max(
                m_adaptiveAdmissionOptions.minWindow,
                m_adaptiveAdmissionWindow > decreaseStep ?
                    m_adaptiveAdmissionWindow - decreaseStep :
                    m_adaptiveAdmissionOptions.minWindow);
        }
        else if (m_adaptiveAdmissionIntervalSuccesses > 0 &&
                 (p95LatencyMs == 0.0 ||
                  queueDelayMs < 0.85 * queueDelayTargetMs ||
                  (!demandBacklogged &&
                   queueDelayMs < queueDelayTargetMs &&
                   queueDelayGradientMs <= -10.0)) &&
                 (queuePressure ||
                  demandBacklogged ||
                  m_adaptiveAdmissionInflight >=
                    static_cast<size_t>(std::ceil(static_cast<double>(activeLimit) * 0.8)) ||
                  p95LatencyMs < 0.8 * targetLatencyMs)) {
            m_adaptiveAdmissionWindow = std::min(
                m_adaptiveAdmissionOptions.maxWindow,
                m_adaptiveAdmissionWindow + m_adaptiveAdmissionOptions.aiStep);
        }

        NDN_LOG_INFO("[NDNSF_ADMISSION] window=" << m_adaptiveAdmissionWindow
                  << " oldWindow=" << oldWindow
                  << " inflight=" << m_adaptiveAdmissionInflight
                  << " queueDepth=" << m_adaptiveAdmissionQueue.size()
                  << " backpressure="
                  << m_adaptiveAdmissionIntervalBackpressure
                  << " successes=" << m_adaptiveAdmissionIntervalSuccesses
                  << " timeouts=" << m_adaptiveAdmissionIntervalTimeouts
                  << " avgLatencyMs=" << averageLatencyMs
                  << " p50LatencyMs=" << p50LatencyMs
                  << " p95LatencyMs=" << p95LatencyMs
                  << " maxLatencyMs=" << maxLatencyMs
                  << " targetLatencyMs=" << targetLatencyMs
                  << " baselineLatencyMs="
                  << m_adaptiveAdmissionBaselineLatencyMs
                  << " queueDelayMs=" << queueDelayMs
                  << " queueDelayGradientMs=" << queueDelayGradientMs
                  << " queueDelayTargetMs=" << queueDelayTargetMs
                  << " queueDelaySevereMs=" << queueDelaySevereMs
                  << " queueDelayOverTargetIntervals="
                  << m_adaptiveAdmissionQueueDelayOverTargetIntervals
                  << " aboveWindow=" << aboveWindow
                  << " queuePressure=" << queuePressure
                  << " queueSevere=" << queueSevere
                  << " latencyCongested=" << latencyCongested
                  << " latencySevere=" << latencySevere
                  << " congested=" << m_adaptiveAdmissionIntervalCongested
                  << " severe=" << m_adaptiveAdmissionIntervalSevere);

        m_adaptiveAdmissionIntervalSuccesses = 0;
        m_adaptiveAdmissionIntervalTimeouts = 0;
        m_adaptiveAdmissionIntervalBackpressure = 0;
        m_adaptiveAdmissionIntervalLatencySumMs = 0.0;
        m_adaptiveAdmissionIntervalLatencyCount = 0;
        m_adaptiveAdmissionIntervalLatenciesMs.clear();
        m_adaptiveAdmissionIntervalCongested = false;
        m_adaptiveAdmissionIntervalSevere = false;
        m_adaptiveAdmissionPreviousQueueDelayMs = queueDelayMs;

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

        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ADMISSION_RELEASED timestamp_us="
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
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_MATCH_ATTEMPT"
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
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_MATCH_FAILED_NO_PENDING_CALL"
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_MATCH_FAILED_EXPIRED_CALL"
                      << " timestamp_us=" << ackReceiveUs
                      << " requestId=" << requestId.toUri()
                      << " ackName=" << ackName.toUri()
                      << " providerName=" << providerName.toUri()
                      << " afterTimeoutCleanup=" << afterTimeoutCleanup
                      << " afterCompletionCleanup=" << afterCompletionCleanup);
        }
        if (!m_pendingCalls.empty()) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_MATCH_FAILED_REQUEST_ID_MISMATCH"
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

    void ServiceUser::fetchPermissionsFromController(const ndn::Name& controllerPrefix)
    {
        ndn::Name interestName(controllerPrefix);
        interestName.append(ndn::Name("/NDNSF/PERMISSIONS/USER"));
        interestName.append(identity);

        ndn::Interest interest(interestName);
        interest.setCanBePrefix(true);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::seconds(4));

        NDN_LOG_INFO("Fetch user permissions: " << interestName);
        m_face.expressInterest(
            interest,
            std::bind(&ServiceUser::onPermissionResponseData, this, _1, _2),
            [this](const ndn::Interest& interest, const ndn::lp::Nack&) {
                onPermissionResponseTimeout(interest);
            },
            std::bind(&ServiceUser::onPermissionResponseTimeout, this, _1));
    }

    void ServiceUser::applyPermissionResponse(const PermissionResponse& response)
    {
        if (response.getPermissionKind() != tlv::UserPermission) {
            NDN_LOG_ERROR("Ignoring non-user PermissionResponse for "
                          << response.getTargetIdentity());
            return;
        }

        for (const auto& entry : response.getEntries()) {
            ndn::Name providerServiceName(entry.getProviderName());
            providerServiceName.append(ndn::Name(entry.getServiceName()));
            UPT.insertPermission(providerServiceName.toUri(),
                                 entry.getServiceName(),
                                 entry.getToken());
            NDN_LOG_INFO("Installed user permission provider="
                         << entry.getProviderName()
                         << " service=" << entry.getServiceName());
        }
    }

    bool ServiceUser::handlePermissionResponseData(const ndn::Data& data,
                                                   const ndn::Name& identity,
                                                   ndn::KeyChain& keyChain,
                                                   UserPermissionTable& permissionTable)
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

        for (const auto& entry : response.getEntries()) {
            ndn::Name providerServiceName(entry.getProviderName());
            providerServiceName.append(ndn::Name(entry.getServiceName()));
            permissionTable.insertPermission(providerServiceName.toUri(),
                                             entry.getServiceName(),
                                             entry.getToken());
            NDN_LOG_INFO("Installed user permission provider="
                         << entry.getProviderName()
                         << " service=" << entry.getServiceName());
        }
        return true;
    }

    void ServiceUser::PublishRequest(const std::vector<ndn::Name> &serviceProviderNames, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &payload, const size_t& strategy)
    {
        // log the request;
        NDN_LOG_INFO("PublishRequest: " <<  ServiceName << FunctionName << RequestID);
        
        ndn_service_framework::BloomFilter bloomFilter;
        std::vector<std::pair<std::string, std::string>> pairs; // serviceFullName->token mapping
        // find serviceFullName->token mapping from UPT;
        pairs = UPT.searchByFunctionName(ndn::Name(ServiceName.toUri()).append(FunctionName).toUri());

        if (serviceProviderNames.size() > 0){
            // add providers to bloom filter;
            for (auto providerName : serviceProviderNames){
                bloomFilter.insert(providerName.toUri());
            }
        }else{
            for (auto pair : pairs){
                bloomFilter.insert(ndn::Name(pair.first).getPrefix(-2).toUri());
            }
        }

        ndn_service_framework::RequestMessage requestMessage;
        if (m_useTokens) {
            requestMessage.setUserToken(makeOneTimeToken());
        }
        requestMessage.setPayload(const_cast<ndn::Buffer&>(payload),payload.size());
        requestMessage.setStrategy(strategy);
        requestMessage.WireEncode().data();
        
        ndn::Name requestName = ndn_service_framework::makeRequestName(identity, ServiceName, FunctionName, ndn::Name(bloomFilter.toHexString()), RequestID);
        ndn::Name requestNameWithoutPrefix = ndn_service_framework::makeRequestNameWithoutPrefix(ServiceName, FunctionName, ndn::Name(bloomFilter.toHexString()),RequestID);

        auto pendingIt = m_pendingCalls.find(RequestID);
        if (pendingIt != m_pendingCalls.end()) {
            pendingIt->second.providers = serviceProviderNames;
            pendingIt->second.serviceName = makeUnifiedServiceName(ServiceName, FunctionName);
            pendingIt->second.requestName = requestName;
            pendingIt->second.requestNameWithoutPrefix = requestNameWithoutPrefix;
            pendingIt->second.requestMessage = requestMessage;
            pendingIt->second.strategy = strategy;
        }
    
        PublishMessage(requestName,requestNameWithoutPrefix,requestMessage);

        // register coordination strategy
        m_strategyMap.emplace(RequestID, strategy);

        // create a timer for LoadBalancing
        if (strategy == tlv::LoadBalancing){

            // insert RequestID->vector<AckInfo> into m_AckInfoMap
            m_AckInfoMap[RequestID] = std::vector<ndn_service_framework::AckInfo>();

            m_scheduler.schedule(100_ms,[this,RequestID](){
                // find vector<AckInfo> from m_AckInfoMap by RequestID 
                auto ackInfoVec = m_AckInfoMap.find(RequestID);
                if (ackInfoVec == m_AckInfoMap.end()){
                    NDN_LOG_ERROR("AckInfo vector not found for RequestID: " << RequestID.toUri());
                    return;
                }

                // if ackInfoVect.size() == 0, means no provider responded, change strategy to FirstResponding
                if (ackInfoVec->second.size() == 0){
                     NDN_LOG_ERROR("After waiting for 100 ms, No AckInfo found for RequestID: " << RequestID.toUri());
                    // log change strategy to FirstResponding
                    NDN_LOG_INFO("Change strategy of "<< RequestID<< " to FirstResponding");
                    m_strategyMap[RequestID] = tlv::FirstResponding;
                    m_AckInfoMap.erase(ackInfoVec);
                    return;
                }

                // random choose one AckInfo from ackInfoVec.second
                auto randomAckInfo = ackInfoVec->second[rand() % ackInfoVec->second.size()];
                // log AckInfo is choosen for LoadBalancing
                NDN_LOG_INFO("Choosen AckInfo for LoadBalancing: " << randomAckInfo.providerName.toUri() << " " << randomAckInfo.requestID.toUri());
                // publish service coordination message
                PublishServiceCoordinationMessage(randomAckInfo.providerName, randomAckInfo.serviceName, randomAckInfo.functionName, randomAckInfo.requestID);
                
            });
        }
    }

    void ServiceUser::PublishRequestV2(const std::vector<ndn::Name>& serviceProviderNames,
                                       const ndn::Name& serviceName,
                                       const ndn::Name& requestId,
                                       const ndn::Buffer& payload,
                                       const size_t& strategy)
    {
        NDN_LOG_INFO("PublishRequestV2: " << serviceName << requestId);

        ndn_service_framework::BloomFilter bloomFilter;
        std::vector<std::pair<std::string, std::string>> pairs =
            UPT.searchByFunctionName(serviceName.toUri());

        if (serviceProviderNames.size() > 0){
            for (auto providerName : serviceProviderNames){
                bloomFilter.insert(providerName.toUri());
            }
        }else{
            for (auto pair : pairs){
                ndn::Name serviceFullName(pair.first);
                bloomFilter.insert(serviceFullName.getPrefix(
                    -static_cast<int>(serviceName.size())).toUri());
            }
        }

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
        requestMessage.WireEncode().data();

        ndn::Name requestName =
            ndn_service_framework::makeRequestNameV2(identity,
                                                     serviceName,
                                                     ndn::Name(bloomFilter.toHexString()),
                                                     requestId);
        ndn::Name requestNameWithoutPrefix =
            ndn_service_framework::makeRequestNameWithoutPrefixV2(
                serviceName,
                ndn::Name(bloomFilter.toHexString()),
                requestId);

        NDN_LOG_INFO("[ServiceUser] selected providerName(s)=");
        if (serviceProviderNames.empty()) {
            NDN_LOG_INFO("<discovery/bloom-filter>");
        }
        else {
            for (size_t i = 0; i < serviceProviderNames.size(); ++i) {
                if (i != 0) {
                    NDN_LOG_INFO(",");
                }
                NDN_LOG_INFO(serviceProviderNames[i].toUri());
            }
        }
        NDN_LOG_INFO(" selected serviceName=" << serviceName.toUri()
                  << " final request name=" << requestName.toUri()
                  << " userToken=" << requestMessage.getUserToken());
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_PUBLISHED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " requestName=" << requestName.toUri());
        NDN_LOG_INFO("PublishRequestV2 selected serviceName=" << serviceName.toUri()
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
        }
        updateRequestLifecycleState(requestId, RequestLifecycleState::REQUEST_PUBLISHED);

        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_PUBLISH_BEGIN timestamp_us="
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
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_PUBLISH_RETURN timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " serviceName=" << serviceName.toUri());

        m_strategyMap.emplace(requestId, strategy);

        if (strategy == tlv::LoadBalancing){
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
                NDN_LOG_INFO("Choosen AckInfo for LoadBalancing: "
                             << randomAckInfo.providerName.toUri() << " "
                             << randomAckInfo.requestID.toUri());
                PublishServiceCoordinationMessage(randomAckInfo.providerName,
                                                  randomAckInfo.serviceName,
                                                  randomAckInfo.functionName,
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

        const ndn::Name dataNameWithoutPrefix =
            makeLargeDataNameWithoutPrefix(ctx.serviceName, ctx.requestId, result.objectId);
        const ndn::Name nominalDataName =
            makeLargeDataName(identity, ctx.serviceName, ctx.requestId, result.objectId);
        const std::vector<std::string> attributes = {
            "/SERVICE" + ctx.serviceName.toUri()
        };

        try {
            ndn::nacabe::SPtrVector<ndn::Data> contentData;
            ndn::nacabe::SPtrVector<ndn::Data> ckData;
            std::tie(contentData, ckData) =
                nacProducer.produce(dataNameWithoutPrefix,
                                    attributes,
                                    ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                                    m_signingInfo);
            if (contentData.empty()) {
                result.errorMessage = "NAC-ABE produced no encrypted content Data";
                return result;
            }

            {
                std::lock_guard<std::mutex> lock(_cache_mutex);
                for (const auto& data : contentData) {
                    m_IMS.insert(*data, freshness);
                }
                for (const auto& data : ckData) {
                    m_IMS.insert(*data, freshness);
                }
            }

            result.encryptedDataName = contentData.front()->getName().getPrefix(-1);
            if (result.encryptedDataName.empty()) {
                result.encryptedDataName = nominalDataName;
            }
            NDN_LOG_INFO("LARGE_DATA_PUBLISH_SEGMENTS"
                      << " name=" << result.encryptedDataName.toUri()
                      << " plaintextBytes=" << plaintext.size()
                      << " contentSegments=" << contentData.size()
                      << " ckSegments=" << ckData.size());
            result.success = true;
        }
        catch (const std::exception& e) {
            result.errorMessage = e.what();
        }
        return result;
    }

    ndn::Name ServiceUser::startAsyncCallWithRequestId(
        const ndn::Name& requestId,
        const std::vector<ndn::Name>& providers,
        const ndn::Name& serviceName,
        ndn_service_framework::RequestMessage requestMessage,
        int timeoutMs,
        TimeoutHandler onTimeout,
        ResponseHandler onResponseHandler,
        size_t strategy)
    {
        PendingCall pendingCall;
        pendingCall.providers = providers;
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
        pendingCall.strategy = strategy;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.createdAtUs = nowMicroseconds();
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        m_pendingCalls[requestId] = std::move(pendingCall);
        updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
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

    ndn::Name ServiceUser::async_call(const PreparedServiceRequest& ctx,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        return async_call(ctx,
                          {},
                          std::move(requestMessage),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponseHandler),
                          strategy);
    }

    ndn::Name ServiceUser::async_call(const PreparedServiceRequest& ctx,
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
        return startAsyncCallWithRequestId(ctx.requestId,
                                           providers,
                                           ctx.serviceName,
                                           std::move(requestMessage),
                                           timeoutMs,
                                           std::move(onTimeout),
                                           std::move(onResponseHandler),
                                           strategy);
    }

    ndn::Name ServiceUser::async_call(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        return startAsyncCallWithRequestId(makeRequestId(),
                                           providers,
                                           serviceName,
                                           std::move(requestMessage),
                                           timeoutMs,
                                           std::move(onTimeout),
                                           std::move(onResponseHandler),
                                           strategy);
    }

    ndn::Name ServiceUser::async_call(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      const ndn::Name& functionName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        return async_call(providers,
                          makeUnifiedServiceName(serviceName, functionName),
                          std::move(requestMessage),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponseHandler),
                          strategy);
    }

    ndn::Name ServiceUser::async_call(const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t strategy)
    {
        return async_call({},
                          serviceName,
                          std::move(requestMessage),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponseHandler),
                          strategy);
    }

    ndn::Name ServiceUser::async_call(const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AcksHandler onAcksHandler,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler)
    {
        const ndn::Name requestId = makeRequestId();

        PendingCall pendingCall;
        pendingCall.serviceName = serviceName;
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
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
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

    ndn::Name ServiceUser::async_call(const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AckCandidatesHandler onAcksHandler,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler)
    {
        const ndn::Name requestId = makeRequestId();

        PendingCall pendingCall;
        pendingCall.serviceName = serviceName;
        pendingCall.requestMessage = requestMessage;
        pendingCall.strategy = ndn_service_framework::tlv::FirstResponding;
        pendingCall.timeoutMs = timeoutMs;
        pendingCall.ackTimeoutMs = ackTimeoutMs;
        pendingCall.createdAtUs = nowMicroseconds();
        pendingCall.ackCandidatesHandler = std::move(onAcksHandler);
        pendingCall.timeoutHandler = std::move(onTimeout);
        pendingCall.responseHandler = std::move(onResponseHandler);
        m_pendingCalls[requestId] = std::move(pendingCall);
        updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
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

    ndn::Name ServiceUser::async_call(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AckCandidatesHandler onAcksHandler,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler,
                                      size_t requestStrategy)
    {
        const ndn::Name requestId = makeRequestId();

        PendingCall pendingCall;
        pendingCall.providers = providers;
        pendingCall.serviceName = serviceName;
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
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
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

    ndn::Name ServiceUser::async_call(const std::vector<ndn::Name>& providers,
                                      const ndn::Name& serviceName,
                                      ndn_service_framework::RequestMessage requestMessage,
                                      int ackTimeoutMs,
                                      AckSelectionStrategy selectionStrategy,
                                      int timeoutMs,
                                      TimeoutHandler onTimeout,
                                      ResponseHandler onResponseHandler)
    {
        if (selectionStrategy == AckSelectionStrategy::FirstRespondingSelection) {
            const ndn::Name requestId = makeRequestId();

            PendingCall pendingCall;
            pendingCall.providers = providers;
            pendingCall.serviceName = serviceName;
            pendingCall.requestMessage = requestMessage;
            pendingCall.strategy = ndn_service_framework::tlv::FirstResponding;
            pendingCall.timeoutMs = timeoutMs;
            pendingCall.ackTimeoutMs = ackTimeoutMs;
            pendingCall.createdAtUs = nowMicroseconds();
            pendingCall.timeoutHandler = std::move(onTimeout);
            pendingCall.responseHandler = std::move(onResponseHandler);
            m_pendingCalls[requestId] = std::move(pendingCall);
            updateRequestLifecycleState(requestId, RequestLifecycleState::QUEUED_LOCAL);
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=REQUEST_CREATED timestamp_us="
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

        auto handler = makeAckSelectionHandler(selectionStrategy);
        if (!handler) {
            return ndn::Name();
        }

        const size_t requestStrategy =
            selectionStrategy == AckSelectionStrategy::AllResponders ?
            ndn_service_framework::tlv::AllResponders :
            ndn_service_framework::tlv::FirstResponding;

        return async_call(providers,
                          serviceName,
                          std::move(requestMessage),
                          ackTimeoutMs,
                          std::move(handler),
                          timeoutMs,
                          std::move(onTimeout),
                          std::move(onResponseHandler),
                          requestStrategy);
    }

    void ServiceUser::handleResponse(const ndn::Name& requestId,
                                     const ndn::Name& providerName,
                                     const ndn_service_framework::ResponseMessage& responseMessage)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            ++m_runtimeDiagnostics.callbackSkippedNoPending;
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=CALLBACK_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " userToken=" << responseMessage.getUserToken());
            return;
        }

        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=CALLBACK_ATTEMPT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " pendingCall=present"
                  << " timedOut=" << pendingCall->second.timedOut);
        if (pendingCall->second.timedOut) {
            ++m_runtimeDiagnostics.callbackSkippedTimeout;
            ++m_runtimeDiagnostics.responseAfterPendingTimeout;
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=CALLBACK_SKIPPED_TIMEOUT timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
            return;
        }
        const bool expectMultipleResponses =
            pendingCall->second.expectedResponseProviders.size() > 1;
        if (pendingCall->second.hasResponse && !expectMultipleResponses) {
            return;
        }
        if (expectMultipleResponses &&
            !containsName(pendingCall->second.expectedResponseProviders, providerName)) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_REJECTED_UNSELECTED_PROVIDER timestamp_us="
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

        NDN_LOG_INFO("[ServiceUser] RESPONSE accepted requestId="
                  << requestId.toUri()
                  << " userToken=" << responseMessage.getUserToken());
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=CALLBACK_FIRED timestamp_us="
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=CALLBACK_QUEUE_FULL timestamp_us="
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPTED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=CALLBACK_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " reason=response_after_pending_erased");
            return false;
        }

        auto pendingCall = m_pendingCalls.find(requestId);
        const auto& expectedUserToken = pendingCall->second.requestMessage.getUserToken();
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_START timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " pendingCall=present");
        if (m_useTokens &&
            (expectedUserToken.empty() ||
             responseMessage.getUserToken() != expectedUserToken)) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_FAILED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " reason=user_token_mismatch"
                      << " expectedTokenPresent=" << !expectedUserToken.empty());
            NDN_LOG_ERROR("Reject response with mismatched UserToken for requestId="
                          << requestId.toUri());
            return false;
        }

        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_DONE timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri());
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_VALIDATION_FAILED timestamp_us="
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

        auto parsed = ndn_service_framework::parseResponseName(responseName);
        if (!parsed) {
            NDN_LOG_ERROR("handleDecryptedResponseByName: parseResponseName failed: " << responseName);
            return false;
        }

        ndn::Name requesterIdentity;
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name functionName;
        ndn::Name requestId;
        std::tie(requesterIdentity, providerName, serviceName, functionName, requestId) =
            parsed.value();

        return handleDecryptedResponse(requestId, providerName, responseMessage);
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

    bool ServiceUser::handleRequestAckByName(
        const ndn::Name& ackName,
        const ndn_service_framework::RequestAckMessage& ackMessage)
    {
        auto parsedV2 = ndn_service_framework::parseRequestAckNameV2(ackName);
        if (parsedV2) {
            const auto ackReceiveUs = nowMicroseconds();
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
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_IGNORED_NO_PENDING timestamp_us="
                          << ackReceiveUs
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " status=" << ackMessage.getStatus());
                return false;
            }

            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_RECEIVED timestamp_us="
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
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=FIRST_ACK_OBSERVED timestamp_us="
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
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_REJECTED_USER_TOKEN timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " expectedTokenPresent=" << !expectedUserToken.empty());
                NDN_LOG_ERROR("Reject ACK with mismatched UserToken for requestId="
                              << parsedV2->requestId.toUri()
                              << " provider=" << parsedV2->providerName.toUri());
                return false;
            }

            if (m_useTokens &&
                ackMessage.getStatus() && ackMessage.getProviderToken().empty()) {
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_REJECTED_PROVIDER_TOKEN timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri());
                NDN_LOG_ERROR("Reject ACK missing ProviderToken for requestId="
                              << parsedV2->requestId.toUri()
                              << " provider=" << parsedV2->providerName.toUri());
                return false;
            }

            if (pendingCall->second.providerSelected ||
                !pendingCall->second.selectedProvider.empty()) {
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_IGNORED_PROVIDER_SELECTED timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " selectedProvider="
                          << (pendingCall->second.selectedProvider.empty() ?
                              "-" : pendingCall->second.selectedProvider.toUri())
                          << " status=" << ackMessage.getStatus());
                return false;
            }

            pendingCall->second.providerTokens[parsedV2->providerName.toUri()] =
                ackMessage.getProviderToken();
            pendingCall->second.requestAcks.push_back(
                StoredAck{parsedV2->providerName,
                          parsedV2->serviceName,
                          parsedV2->requestId,
                          ackMessage});
            updateRequestLifecycleState(parsedV2->requestId, RequestLifecycleState::ACK_MATCHED);
            auto& traceRecord = m_pendingCallTraceHistory[parsedV2->requestId];
            if (traceRecord.createdAtUs == 0) {
                traceRecord.createdAtUs = pendingCall->second.createdAtUs;
            }
            traceRecord.matchedAck = true;
            traceRecord.requestName = pendingCall->second.requestName;
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_MATCHED_PENDING_CALL timestamp_us="
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
            const auto& storedAck = pendingCall->second.requestAcks.back();
            if (pendingCall->second.acksHandler ||
                pendingCall->second.ackCandidatesHandler) {
                if (pendingCall->second.ackWindowExpired) {
                    selectLateAckAfterAckTimeout(pendingCall->second, storedAck);
                }
                return true;
            }
            if (pendingCall->second.ackWindowExpired) {
                selectLateAckAfterAckTimeout(pendingCall->second, storedAck);
                return true;
            }
            const bool shouldCoordinateFirstAck =
                pendingCall->second.strategy == ndn_service_framework::tlv::FirstResponding &&
                pendingCall->second.selectedProvider.empty() &&
                ackMessage.getStatus();
            evaluateAckSelection(parsedV2->requestId);
            pendingCall = m_pendingCalls.find(parsedV2->requestId);
            if (shouldCoordinateFirstAck &&
                pendingCall != m_pendingCalls.end() &&
                pendingCall->second.selectedProvider.equals(parsedV2->providerName)) {
                const auto scheduleAtUs = nowMicroseconds();
                pendingCall->second.coordinationScheduledAtUs = scheduleAtUs;
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_ELIGIBILITY_CHECK timestamp_us="
                          << scheduleAtUs
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " serviceName=" << parsedV2->serviceName.toUri()
                          << " eligible=1"
                          << " reason=first_ack_selected"
                          << " providerTokenPresent="
                          << (pendingCall->second.providerTokens.find(parsedV2->providerName.toUri()) !=
                              pendingCall->second.providerTokens.end()));
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_SCHEDULE_ATTEMPT timestamp_us="
                          << scheduleAtUs
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " serviceName=" << parsedV2->serviceName.toUri());
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_FAST_PATH timestamp_us="
                          << nowMicroseconds()
                          << " requestId=" << parsedV2->requestId.toUri()
                          << " providerName=" << parsedV2->providerName.toUri()
                          << " serviceName=" << parsedV2->serviceName.toUri());
                PublishServiceCoordinationMessageV2(parsedV2->providerName,
                                                    parsedV2->serviceName,
                                                    parsedV2->requestId);
            }
            else if (shouldCoordinateFirstAck) {
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_SKIPPED timestamp_us="
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

        auto parsed = ndn_service_framework::parseRequestAckName(ackName);
        if (!parsed) {
            NDN_LOG_ERROR("handleRequestAckByName: parseRequestAckName failed: " << ackName);
            return false;
        }

        ndn::Name providerName;
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name functionName;
        ndn::Name requestId;
        std::tie(providerName, requesterName, serviceName, functionName, requestId) =
            parsed.value();

        const auto ackReceiveUs = nowMicroseconds();
        logAckMatchAttempt(requestId, ackName, providerName, ackReceiveUs,
                           "decoded_ack");
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            logAckNoPending(requestId, ackName, providerName, ackReceiveUs);
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_IGNORED_NO_PENDING timestamp_us="
                      << ackReceiveUs
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " status=" << ackMessage.getStatus());
            return false;
        }

        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_RECEIVED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=FIRST_ACK_OBSERVED timestamp_us="
                      << pendingCall->second.firstAckAtUs
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " status=" << ackMessage.getStatus());
        }

        const auto& expectedUserToken = pendingCall->second.requestMessage.getUserToken();
        if (m_useTokens &&
            (expectedUserToken.empty() ||
             ackMessage.getUserToken() != expectedUserToken)) {
            NDN_LOG_ERROR("Reject ACK with mismatched UserToken for requestId="
                          << requestId.toUri()
                          << " provider=" << providerName.toUri());
            return false;
        }

        if (m_useTokens &&
            ackMessage.getStatus() && ackMessage.getProviderToken().empty()) {
            NDN_LOG_ERROR("Reject ACK missing ProviderToken for requestId="
                          << requestId.toUri()
                          << " provider=" << providerName.toUri());
            return false;
        }

        if (pendingCall->second.providerSelected ||
            !pendingCall->second.selectedProvider.empty()) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_IGNORED_PROVIDER_SELECTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " selectedProvider="
                      << (pendingCall->second.selectedProvider.empty() ?
                          "-" : pendingCall->second.selectedProvider.toUri())
                      << " status=" << ackMessage.getStatus());
            return false;
        }

        pendingCall->second.providerTokens[providerName.toUri()] =
            ackMessage.getProviderToken();
        pendingCall->second.requestAcks.push_back(
            StoredAck{providerName,
                      makeUnifiedServiceName(serviceName, functionName),
                      requestId,
                      ackMessage});
        updateRequestLifecycleState(requestId, RequestLifecycleState::ACK_MATCHED);
        auto& traceRecord = m_pendingCallTraceHistory[requestId];
        if (traceRecord.createdAtUs == 0) {
            traceRecord.createdAtUs = pendingCall->second.createdAtUs;
        }
        traceRecord.matchedAck = true;
        traceRecord.requestName = pendingCall->second.requestName;
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_MATCHED_PENDING_CALL timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " callId=" << requestId.toUri()
                  << " ackName=" << ackName.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << makeUnifiedServiceName(serviceName, functionName).toUri()
                  << " ackCount=" << pendingCall->second.requestAcks.size()
                  << " status=" << ackMessage.getStatus()
                  << " providerTokenPresent="
                  << !ackMessage.getProviderToken().empty()
                  << " userTokenMatched=1");
        const auto& storedAck = pendingCall->second.requestAcks.back();
        if (pendingCall->second.acksHandler ||
            pendingCall->second.ackCandidatesHandler) {
            if (pendingCall->second.ackWindowExpired) {
                selectLateAckAfterAckTimeout(pendingCall->second, storedAck);
            }
            return true;
        }
        if (pendingCall->second.ackWindowExpired) {
            selectLateAckAfterAckTimeout(pendingCall->second, storedAck);
            return true;
        }
        const bool shouldCoordinateFirstAck =
            pendingCall->second.strategy == ndn_service_framework::tlv::FirstResponding &&
            pendingCall->second.selectedProvider.empty() &&
            ackMessage.getStatus();
        evaluateAckSelection(requestId);
        pendingCall = m_pendingCalls.find(requestId);
        if (shouldCoordinateFirstAck &&
            pendingCall != m_pendingCalls.end() &&
            pendingCall->second.selectedProvider.equals(providerName)) {
            const auto scheduleAtUs = nowMicroseconds();
            pendingCall->second.coordinationScheduledAtUs = scheduleAtUs;
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_ELIGIBILITY_CHECK timestamp_us="
                      << scheduleAtUs
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << makeUnifiedServiceName(serviceName, functionName).toUri()
                      << " eligible=1"
                      << " reason=first_ack_selected"
                      << " providerTokenPresent="
                      << (pendingCall->second.providerTokens.find(providerName.toUri()) !=
                          pendingCall->second.providerTokens.end()));
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_SCHEDULE_ATTEMPT timestamp_us="
                      << scheduleAtUs
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << makeUnifiedServiceName(serviceName, functionName).toUri());
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_FAST_PATH timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << makeUnifiedServiceName(serviceName, functionName).toUri());
            PublishServiceCoordinationMessage(providerName,
                                              serviceName,
                                              functionName,
                                              requestId);
        }
        else if (shouldCoordinateFirstAck) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_SKIPPED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << makeUnifiedServiceName(serviceName, functionName).toUri()
                      << " reason="
                      << (pendingCall == m_pendingCalls.end() ?
                          "pending_missing_after_selection" : "selected_provider_mismatch"));
        }

        return true;
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
        return ndn::Name(ndn::time::toIsoString(ndn::time::system_clock::now()));
    }

    ndn::Name ServiceUser::makeUnifiedServiceName(const ndn::Name& serviceName,
                                                  const ndn::Name& functionName)
    {
        if (functionName.empty()) {
            return serviceName;
        }

        ndn::Name unified(serviceName);
        for (const auto& component : functionName) {
            unified.append(component);
        }
        return unified;
    }

    bool ServiceUser::evaluateAckSelection(const ndn::Name& requestId)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_SELECTION_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
            return false;
        }

        if (pendingCall->second.providerSelected ||
            !pendingCall->second.selectedProvider.empty()) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_SELECTION_SKIPPED_ALREADY_SELECTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " selectedProvider="
                      << (pendingCall->second.selectedProvider.empty() ?
                          "-" : pendingCall->second.selectedProvider.toUri()));
            return true;
        }

        pendingCall->second.ackSelectionAtUs = nowMicroseconds();
        size_t successfulAckCount = 0;
        for (const auto& storedAck : pendingCall->second.requestAcks) {
            if (storedAck.message.getStatus()) {
                ++successfulAckCount;
            }
        }
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_SELECTION_BEGIN timestamp_us="
                  << pendingCall->second.ackSelectionAtUs
                  << " requestId=" << requestId.toUri()
                  << " ackCount=" << pendingCall->second.requestAcks.size()
                  << " successfulAckCount=" << successfulAckCount
                  << " ackTimeoutMs=" << pendingCall->second.ackTimeoutMs
                  << " timeoutMs=" << pendingCall->second.timeoutMs
                  << " customHandler="
                  << static_cast<bool>(pendingCall->second.acksHandler ||
                                       pendingCall->second.ackCandidatesHandler));

        bool selected = false;
        if (pendingCall->second.acksHandler ||
            pendingCall->second.ackCandidatesHandler) {
            selected = evaluateCustomAckSelection(pendingCall->second);
        }
        else {
            selected = evaluateBuiltInAckSelection(pendingCall->second);
        }

        const bool hasSelectedCandidate =
            !pendingCall->second.selectedProvider.empty() ||
            !pendingCall->second.customSelectedAcks.empty() ||
            (pendingCall->second.strategy == ndn_service_framework::tlv::AllResponders &&
             !pendingCall->second.successfulAckProviders.empty());
        selected = selected && hasSelectedCandidate;

        pendingCall->second.ackSelectionCompletedAtUs = nowMicroseconds();
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_SELECTION_END timestamp_us="
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
        if (selected && hasSelectedCandidate) {
            pendingCall->second.providerSelected = true;
            updateRequestLifecycleState(requestId, RequestLifecycleState::PROVIDER_SELECTED);
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=PROVIDER_SELECTED timestamp_us="
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
            releaseAdaptiveAdmissionSlot(requestId,
                                         pendingCall->second,
                                         "no_provider_selected",
                                         pendingCall->second.ackSelectionCompletedAtUs);
        }
        return selected;
    }

    bool ServiceUser::handleAckCollectionTimeout(const ndn::Name& requestId)
    {
        auto pendingCall = m_pendingCalls.find(requestId);
        if (pendingCall == m_pendingCalls.end()) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_SELECTION_SKIPPED_NO_PENDING timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri());
            return false;
        }

        if (pendingCall->second.strategy == ndn_service_framework::tlv::FirstResponding &&
            !pendingCall->second.acksHandler &&
            !pendingCall->second.ackCandidatesHandler) {
            return false;
        }

        pendingCall->second.ackWindowExpired = true;
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

        pendingCall.selectedProvider = storedAck.providerName;
        pendingCall.providerSelected = true;
        pendingCall.customSelectedAcks.clear();
        pendingCall.customSelectedAcks.push_back(storedAck);
        pendingCall.successfulAckProviders.clear();
        addUniqueName(pendingCall.successfulAckProviders, storedAck.providerName);
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=LATE_ACK_SELECTED_AFTER_ACK_TIMEOUT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << storedAck.requestId.toUri()
                  << " providerName=" << storedAck.providerName.toUri()
                  << " serviceName=" << storedAck.serviceName.toUri()
                  << " providerTokenPresent="
                  << !storedAck.message.getProviderToken().empty());
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=CUSTOM_ACK_SELECTED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << storedAck.requestId.toUri()
                  << " providerName=" << storedAck.providerName.toUri()
                  << " serviceName=" << storedAck.serviceName.toUri()
                  << " providerTokenPresent="
                  << !storedAck.message.getProviderToken().empty());
        PublishServiceCoordinationMessageV2(storedAck.providerName,
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

        if (pendingCall.ackCandidatesHandler) {
            std::vector<ndn_service_framework::AckSelectionCandidate> candidates;
            for (const auto& storedAck : pendingCall.requestAcks) {
                candidates.push_back({storedAck.providerName,
                                      storedAck.serviceName,
                                      storedAck.requestId,
                                      storedAck.message});
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

        for (const auto& selectedAck : pendingCall.customSelectedAcks) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=CUSTOM_ACK_SELECTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << selectedAck.requestId.toUri()
                      << " providerName=" << selectedAck.providerName.toUri()
                      << " serviceName=" << selectedAck.serviceName.toUri()
                      << " providerTokenPresent="
                      << !selectedAck.message.getProviderToken().empty());
            PublishServiceCoordinationMessageV2(selectedAck.providerName,
                                                selectedAck.serviceName,
                                                selectedAck.requestId);
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

        if (pendingCall.strategy == ndn_service_framework::tlv::LoadBalancing) {
            pendingCall.selectedProvider = selectLoadBalancingProvider(pendingCall.successfulAckProviders);
            pendingCall.expectedResponseProviders.clear();
            addUniqueName(pendingCall.expectedResponseProviders, pendingCall.selectedProvider);
            return !pendingCall.selectedProvider.empty();
        }

        if (pendingCall.strategy == ndn_service_framework::tlv::AllResponders) {
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
                PublishServiceCoordinationMessageV2(storedAck.providerName,
                                                    storedAck.serviceName,
                                                    storedAck.requestId);
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

    ndn::Name ServiceUser::selectLoadBalancingProvider(
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
        NDN_LOG_INFO("processNDNSDServiceInfoCallback: Service publish callback received");
        // find key name = "tokenNames"
        if (details.serviceMetaInfo.find("tokenName") != details.serviceMetaInfo.end()) {
            std::string tokenName = details.serviceMetaInfo.at("tokenName");
            // split tokens uing ";"

            // consume tokenName using NAC-ABE
            ndn::Name providerName, ServiceName, FunctionName, seqNum;
            
            auto result = ndn_service_framework::parsePermissionTokenName(ndn::Name(tokenName));
            if(result){
                std::tie(providerName, ServiceName, FunctionName, seqNum) = result.value();
            nacConsumer.consume(
                ndn::Name(tokenName),
                std::bind(&ServiceUser::OnPermissionTokenDecryptionSuccessCallback, this, providerName, ServiceName, FunctionName, seqNum, _1),
                std::bind(&ServiceUser::OnPermissionTokenDecryptionErrorCallback, this, providerName, ServiceName, FunctionName, seqNum, _1));
            }else{
                NDN_LOG_ERROR("Invalid token name: " << tokenName); 
            }
            
        } else {
            NDN_LOG_ERROR("No tokenNames found in service publish callback");
        }


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
                handlePermissionResponseData(validatedData, identity, m_keyChain, UPT);
            },
            [](const ndn::Data& badData, const ndn::security::ValidationError& error) {
                NDN_LOG_ERROR("PermissionResponse Data validation failed: "
                              << badData.getName() << " reason=" << error);
            });
    }

    void ServiceUser::onPermissionResponseTimeout(const ndn::Interest& interest)
    {
        NDN_LOG_ERROR("PermissionResponse timeout: " << interest.getName());
    }

    void ServiceUser::OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) {
            return;
        }
        // log message
        NDN_LOG_INFO("OnRequestAck: " << subscription.name);

        auto ackV2 = parseRequestAckNameV2(subscription.name);
        if (ackV2) {
            const auto ackReceiveUs = nowMicroseconds();
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
            if (pendingCall == m_pendingCalls.end() ||
                pendingCall->second.hasResponse ||
                pendingCall->second.timedOut ||
                pendingCall->second.providerSelected ||
                !pendingCall->second.selectedProvider.empty()) {
                if (pendingCall == m_pendingCalls.end()) {
                    logAckNoPending(ackV2->requestId,
                                    subscription.name,
                                    ackV2->providerName,
                                    ackReceiveUs);
                }
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_MATCH_SKIPPED_PRE_DECRYPT timestamp_us="
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
                                                          ndn::Name(),
                                                          ackV2->requestId,
                                                          plaintext);
                    return;
                }
                if (m_useHybridMessageCrypto &&
                    decryptHybridMessage(
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
                                                                  ndn::Name(),
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
                                                                ndn::Name(),
                                                                requestId,
                                                                error);
                        })) {
                    return;
                }
                nacConsumer.consume(
                            ndn::Name(subscription.name),
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
                                logCryptoDiag("user", "ack", "decrypt", "normal",
                                              "success", decryptStartUs, decryptEndUs,
                                              subscriptionName, buffer.size());
                                OnRequestAckDecryptionSuccessCallback(providerName,
                                                                      serviceName,
                                                                      ndn::Name(),
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
                                                                    ndn::Name(),
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
                                                                      ndn::Name(),
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
                                                                    ndn::Name(),
                                                                    requestId,
                                                                    error);
                            });
            }
            return;
        }

        // parse permission ack name
        ndn::Name providerName, userName, ServiceName, FunctionName, seqNum;
        auto results = parseRequestAckName(subscription.name);
        if (!results)
        {   
            NDN_LOG_ERROR("parseRequestAckName failed: " << subscription.name);
            return;
        }
        std::tie(providerName, userName, ServiceName, FunctionName, seqNum) = results.value();

        const auto ackReceiveUs = nowMicroseconds();
        logAckMatchAttempt(seqNum,
                           subscription.name,
                           providerName,
                           ackReceiveUs,
                           "pre_decrypt");
        auto pendingCall = m_pendingCalls.find(seqNum);
        if (pendingCall == m_pendingCalls.end() ||
            pendingCall->second.hasResponse ||
            pendingCall->second.timedOut ||
            pendingCall->second.providerSelected ||
            !pendingCall->second.selectedProvider.empty()) {
            if (pendingCall == m_pendingCalls.end()) {
                logAckNoPending(seqNum,
                                subscription.name,
                                providerName,
                                ackReceiveUs);
            }
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=ACK_MATCH_SKIPPED_PRE_DECRYPT timestamp_us="
                      << ackReceiveUs
                      << " requestId=" << seqNum.toUri()
                      << " ackName=" << subscription.name.toUri()
                      << " providerName=" << providerName.toUri()
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
            NDN_LOG_TRACE("Skip decrypting irrelevant ACK: " << subscription.name);
            return;
        }
        
        if(subscription.data.size() > 0){
            const auto decryptStartUs = nowMicroseconds();
            if (plaintextAckDiagEnabled()) {
                ndn::Buffer plaintext(subscription.data.begin(), subscription.data.end());
                const auto decryptEndUs = nowMicroseconds();
                logCryptoDiag("user", "ack", "decrypt", "plaintext",
                              "success", decryptStartUs, decryptEndUs,
                              subscription.name, plaintext.size());
                OnRequestAckDecryptionSuccessCallback(providerName,
                                                      ServiceName,
                                                      FunctionName,
                                                      seqNum,
                                                      plaintext);
                return;
            }
            nacConsumer.consume(
                        ndn::Name(subscription.name),
                        ndn::Block(subscription.data),
                        [this, providerName, ServiceName, FunctionName, seqNum,
                         subscriptionName = ndn::Name(subscription.name),
                         decryptStartUs](const ndn::Buffer& buffer) {
                            const auto decryptEndUs = nowMicroseconds();
                            logCryptoDiag("user", "ack", "decrypt", "normal",
                                          "success", decryptStartUs, decryptEndUs,
                                          subscriptionName, buffer.size());
                            OnRequestAckDecryptionSuccessCallback(providerName, ServiceName,
                                                                  FunctionName, seqNum, buffer);
                        },
                        [this, providerName, ServiceName, FunctionName, seqNum,
                         subscriptionName = ndn::Name(subscription.name),
                         decryptStartUs](const std::string& error) {
                            const auto decryptEndUs = nowMicroseconds();
                            logCryptoDiag("user", "ack", "decrypt", "normal",
                                          "failure", decryptStartUs, decryptEndUs,
                                          subscriptionName, 0, error);
                            OnRequestAckDecryptionErrorCallback(providerName, ServiceName,
                                                                FunctionName, seqNum, error);
                        });
        }else{
            const auto decryptStartUs = nowMicroseconds();
            nacConsumer.consume(
                        ndn::Name(subscription.name),
                        [this, providerName, ServiceName, FunctionName, seqNum,
                         subscriptionName = ndn::Name(subscription.name),
                         decryptStartUs](const ndn::Buffer& buffer) {
                            const auto decryptEndUs = nowMicroseconds();
                            logCryptoDiag("user", "ack", "decrypt", "normal",
                                          "success", decryptStartUs, decryptEndUs,
                                          subscriptionName, buffer.size());
                            OnRequestAckDecryptionSuccessCallback(providerName, ServiceName,
                                                                  FunctionName, seqNum, buffer);
                        },
                        [this, providerName, ServiceName, FunctionName, seqNum,
                         subscriptionName = ndn::Name(subscription.name),
                         decryptStartUs](const std::string& error) {
                            const auto decryptEndUs = nowMicroseconds();
                            logCryptoDiag("user", "ack", "decrypt", "normal",
                                          "failure", decryptStartUs, decryptEndUs,
                                          subscriptionName, 0, error);
                            OnRequestAckDecryptionErrorCallback(providerName, ServiceName,
                                                                FunctionName, seqNum, error);
                        });
        }

    }

    void ServiceUser::OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) {
            return;
        }

        NDN_LOG_INFO("OnResponse: " << subscription.name);

        ndn::Name requesterName, providerName, ServiceName, FunctionName, RequestId;
        auto resultsV2 = ndn_service_framework::parseResponseNameV2(subscription.name);
        if (resultsV2) {
            requesterName = resultsV2->requesterName;
            providerName = resultsV2->providerName;
            ServiceName = resultsV2->serviceName;
            FunctionName = ndn::Name();
            RequestId = resultsV2->requestId;
        }
        else {
            auto results = ndn_service_framework::parseResponseName(subscription.name);
            if (!results) {
            NDN_LOG_ERROR("parseResponseName failed: " << subscription.name);
            return;
            }
            std::tie(requesterName, providerName, ServiceName, FunctionName, RequestId) =
                results.value();
        }

        const ndn::Name responseName(subscription.name);
        auto responsePending = m_pendingCalls.find(RequestId);
        if (responsePending != m_pendingCalls.end() &&
            responsePending->second.responseObservedAtUs == 0) {
            responsePending->second.responseObservedAtUs = nowMicroseconds();
        }
        updateRequestLifecycleState(RequestId, RequestLifecycleState::RESPONSE_OBSERVED);
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_OBSERVED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << RequestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << ServiceName.toUri()
                  << " responseName=" << responseName.toUri()
                  << " contentBytes=" << subscription.data.size());
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
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPT_START timestamp_us="
                  << decryptStartUs
                  << " requestId=" << RequestId.toUri()
                  << " responseName=" << responseName.toUri());
        auto onSuccess = [this, responseName, RequestId, decryptStartUs](const ndn::Buffer& buffer) {
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPT_DONE timestamp_us="
                      << decryptEndUs
                      << " requestId=" << RequestId.toUri()
                      << " responseName=" << responseName.toUri()
                      << " payloadBytes=" << buffer.size()
                      << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                            decryptEndUs - decryptStartUs : 0));
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_RECEIVED timestamp_us="
                      << decryptEndUs
                      << " requestId=" << RequestId.toUri()
                      << " responseName=" << responseName.toUri());
            ndn::Block responseBlock(buffer);
            if (!handleDecryptedResponseByName(responseName, responseBlock)) {
                NDN_LOG_INFO("OnResponse: no pending async callback for " << responseName);
            }
        };
        auto onError = [this, providerName, ServiceName, FunctionName, RequestId,
                        responseName, decryptStartUs](const std::string& error) {
            const auto decryptEndUs = nowMicroseconds();
            logCryptoDiag("user", "response", "decrypt", "normal",
                          "failure", decryptStartUs, decryptEndUs,
                          responseName, 0, error);
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPT_FAILED timestamp_us="
                      << decryptEndUs
                      << " requestId=" << RequestId.toUri()
                      << " responseName=" << responseName.toUri()
                      << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                            decryptEndUs - decryptStartUs : 0)
                      << " error=" << error);
            OnResponseDecryptionErrorCallback(providerName, ServiceName,
                                              FunctionName, RequestId, error);
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
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_DECRYPT_DONE timestamp_us="
                          << decryptEndUs
                          << " requestId=" << RequestId.toUri()
                          << " responseName=" << responseName.toUri()
                          << " payloadBytes=" << plaintext.size()
                          << " durationUs=" << (decryptEndUs >= decryptStartUs ?
                                                decryptEndUs - decryptStartUs : 0)
                          << " mode=plaintext");
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=RESPONSE_RECEIVED timestamp_us="
                          << decryptEndUs
                          << " requestId=" << RequestId.toUri()
                          << " responseName=" << responseName.toUri());
                ndn::Block responseBlock(plaintext);
                if (!handleDecryptedResponseByName(responseName, responseBlock)) {
                    NDN_LOG_INFO("OnResponse: no pending async callback for " << responseName);
                }
                return;
            }
            if (m_useHybridMessageCrypto &&
                decryptHybridMessage(responseName,
                                     ndn::Block(subscription.data),
                                     onSuccess,
                                     onError)) {
                return;
            }
            nacConsumer.consume(
                        ndn::Name(subscription.name),
                        ndn::Block(subscription.data),
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
    const ndn::Name& ServiceName,
    const ndn::Name& FunctionName,
    const ndn::Name& requestID,
    const ndn::Buffer& buffer)
{
    // Copy raw bytes across thread boundary (DO NOT pass Block/Buffer)
    auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());

    // ndnsf::post([this,
    //              providerName,
    //              ServiceName,
    //              FunctionName,
    //              requestID,
    //              raw]() mutable
    // {
        // Reconstruct TLV block safely
        ndn::Block block(ndn::span<const uint8_t>(raw->data(), raw->size()));

        try {
            block.parse();
        }
        catch (const std::exception& e) {
            NDN_LOG_ERROR("OnRequestAckDecryptionSuccessCallback: Failed to parse Block: " << e.what());
            return;
        }

        // Logging
        NDN_LOG_INFO("OnRequestAckDecryptionSuccessCallback: "
                     << providerName.toUri() << " "
                     << ServiceName.toUri() << " "
                     << FunctionName.toUri() << " "
                     << requestID.toUri());

        // Decode Permission Ack message
        RequestAckMessage AckMessage;
        try {
            AckMessage.WireDecode(block);
        }
        catch (const std::exception& e) {
            NDN_LOG_ERROR("RequestAckMessage decode failed: " << e.what());
            return;
        }

        const auto ackPayload = AckMessage.getPayload();
        const std::string ackPayloadText(
            reinterpret_cast<const char*>(ackPayload.data()),
            ackPayload.size());
        NDN_LOG_INFO("[ServiceUser] ACK received timestampMs="
                  << nowMilliseconds()
                  << " requestId=" << requestID.toUri()
                  << " providerName=" << providerName.toUri()
                  << " status=" << AckMessage.getStatus()
                  << " message=" << AckMessage.getMessage()
                  << " userToken=" << AckMessage.getUserToken()
                  << " providerToken=" << AckMessage.getProviderToken()
                  << " payload=" << ackPayloadText);
        const ndn::Name ackName = FunctionName.empty() ?
            ndn_service_framework::makeRequestAckNameV2(providerName,
                                                        identity,
                                                        ServiceName,
                                                        requestID) :
            ndn_service_framework::makeRequestAckName(providerName,
                                                      identity,
                                                      ServiceName,
                                                      FunctionName,
                                                      requestID);
        const bool handledByPendingCall = handleRequestAckByName(ackName, AckMessage);
        if (FunctionName.empty()) {
            if (!handledByPendingCall) {
                NDN_LOG_INFO("V2 ACK did not update pending call state for requestID: "
                             << requestID.toUri());
            }
            return;
        }
        auto pendingCall = m_pendingCalls.find(requestID);
        if (handledByPendingCall && pendingCall != m_pendingCalls.end()) {
            return;
        }
        if (pendingCall == m_pendingCalls.end() ||
            pendingCall->second.providerSelected ||
            !pendingCall->second.selectedProvider.empty()) {
            return;
        }

        // Check permission
        if (!AckMessage.getStatus()) {
            NDN_LOG_ERROR("Permission Denied by Provider: "
                          << providerName.toUri() << " "
                          << ServiceName.toUri() << " "
                          << FunctionName.toUri() << " "
                          << requestID.toUri() << " "
                          << "message: " << AckMessage.getMessage());
            return;
        }

        // Lookup strategy
        auto strategyIt = m_strategyMap.find(requestID);
        if (strategyIt == m_strategyMap.end()) {
            NDN_LOG_ERROR("Strategy not found for requestID: " << requestID.toUri());
            return;
        }

        auto strategy = strategyIt->second;
        NDN_LOG_INFO("Strategy: " << strategy);

        // Decision based on strategy
        if (strategy == tlv::FirstResponding) {
            PublishServiceCoordinationMessage(providerName,
                                              ServiceName,
                                              FunctionName,
                                              requestID);
            m_strategyMap.erase(strategyIt);
        }
        else if (strategy == tlv::LoadBalancing) {
            // Ensure vector exists
            auto itAck = m_AckInfoMap.find(requestID);
            if (itAck == m_AckInfoMap.end()) {
                NDN_LOG_ERROR("AckInfo vector missing for RequestID: " << requestID.toUri());
                return;
            }

            // Add provider ACK info
            itAck->second.push_back({providerName, ServiceName, FunctionName, requestID});

            NDN_LOG_INFO("AckInfo added for RequestID: "
                         << providerName.toUri() << " "
                         << requestID.toUri());
        }
        else if (strategy == tlv::AllResponders) {
            PublishServiceCoordinationMessage(providerName, ServiceName, FunctionName, requestID);
        }
        else {
            NDN_LOG_ERROR("Invalid strategy: " << strategy);
            return;
        }
    // });
}



    void ServiceUser::OnRequestAckDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const std::string &error)
    {
        // log error
        NDN_LOG_ERROR("OnRequestAckDecryptionErrorCallback: " << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << requestID.toUri() << " with error: " << error);
    }

    void ServiceUser::PublishServiceCoordinationMessage(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID)
    {
        NDN_LOG_INFO("[ServiceUser] PublishServiceCoordinationMessage called timestampMs="
                  << nowMilliseconds()
                  << " requestId=" << requestID.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << ServiceName.toUri());
        if (FunctionName.empty()) {
            PublishServiceCoordinationMessageV2(providerName, ServiceName, requestID);
            return;
        }

        // log message
        NDN_LOG_INFO("PublishServiceCoordinationMessage: " << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << requestID.toUri());
        // create service coordination message
        ServiceCoordinationMessage coordinationMessage;
        coordinationMessage.setRequestIDs({requestID.toUri()});
        auto pendingIt = m_pendingCalls.find(requestID);
        bool providerTokenPresent = false;
        if (pendingIt != m_pendingCalls.end()) {
            auto tokenIt =
                pendingIt->second.providerTokens.find(providerName.toUri());
            if (m_useTokens && tokenIt != pendingIt->second.providerTokens.end()) {
                coordinationMessage.setProviderToken(tokenIt->second);
                providerTokenPresent = true;
            }
        }
        if (!m_useTokens) {
            providerTokenPresent = true;
        }
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_ELIGIBILITY_CHECK timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestID.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << makeUnifiedServiceName(ServiceName, FunctionName).toUri()
                  << " eligible=" << (pendingIt != m_pendingCalls.end() && providerTokenPresent)
                  << " reason=publish_entry"
                  << " pendingCallPresent=" << (pendingIt != m_pendingCalls.end())
                  << " providerTokenPresent=" << providerTokenPresent);
        if (pendingIt == m_pendingCalls.end() || !providerTokenPresent) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_REJECTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestID.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << makeUnifiedServiceName(ServiceName, FunctionName).toUri()
                      << " reason="
                      << (pendingIt == m_pendingCalls.end() ?
                          "pending_missing" : "provider_token_missing"));
        }

        // make service coordination message name
        ndn::Name serviceCoordinationName = makeServiceCoordinationName(identity, providerName, ServiceName, FunctionName, requestID);
        ndn::Name serviceCoordinationNameWithoutPrefix = makeServiceCoordinationNameWithoutPrefix(providerName, ServiceName, FunctionName, requestID);

        // publish service coordination message
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_PUBLISH_ATTEMPT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestID.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << makeUnifiedServiceName(ServiceName, FunctionName).toUri()
                  << " messageName=" << serviceCoordinationName.toUri());
        try {
            PublishMessage(serviceCoordinationName, serviceCoordinationNameWithoutPrefix, coordinationMessage);
        }
        catch (const std::exception& e) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_PUBLISH_FAILED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestID.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << makeUnifiedServiceName(ServiceName, FunctionName).toUri()
                      << " reason=exception"
                      << " error=" << e.what());
            throw;
        }
        if (pendingIt != m_pendingCalls.end()) {
            pendingIt->second.coordinationPublishedAtUs = nowMicroseconds();
            addUniqueName(pendingIt->second.coordinatedProviders, providerName);
        }
        updateRequestLifecycleState(requestID, RequestLifecycleState::COORDINATION_PUBLISHED);
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_PUBLISHED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestID.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << makeUnifiedServiceName(ServiceName, FunctionName).toUri()
                  << " messageName=" << serviceCoordinationName.toUri());
        NDN_LOG_INFO("[ServiceUser] coordination PublishMessage returned timestampMs="
                  << nowMilliseconds()
                  << " requestId=" << requestID.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << ServiceName.toUri());
    }

    void ServiceUser::PublishServiceCoordinationMessageV2(const ndn::Name& providerName,
                                                          const ndn::Name& serviceName,
                                                          const ndn::Name& requestId)
    {
        NDN_LOG_INFO("PublishServiceCoordinationMessageV2: "
                     << providerName.toUri()
                     << serviceName.toUri()
                     << requestId.toUri());
        NDN_LOG_INFO("[ServiceUser] PublishServiceCoordinationMessage called timestampMs="
                  << nowMilliseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri());

        ServiceCoordinationMessage coordinationMessage;
        coordinationMessage.setRequestIDs({requestId.toUri()});
        auto pendingIt = m_pendingCalls.find(requestId);
        bool providerTokenPresent = false;
        if (pendingIt != m_pendingCalls.end()) {
            auto tokenIt =
                pendingIt->second.providerTokens.find(providerName.toUri());
            if (m_useTokens && tokenIt != pendingIt->second.providerTokens.end()) {
                coordinationMessage.setProviderToken(tokenIt->second);
                providerTokenPresent = true;
            }
        }
        if (!m_useTokens) {
            providerTokenPresent = true;
        }
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_TOKEN_STATE timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " pendingCallPresent=" << (pendingIt != m_pendingCalls.end())
                  << " providerTokenPresent=" << providerTokenPresent);
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_ELIGIBILITY_CHECK timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " eligible=" << (pendingIt != m_pendingCalls.end() && providerTokenPresent)
                  << " reason=publish_entry"
                  << " pendingCallPresent=" << (pendingIt != m_pendingCalls.end())
                  << " providerTokenPresent=" << providerTokenPresent);
        if (pendingIt == m_pendingCalls.end() || !providerTokenPresent) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_REJECTED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " reason="
                      << (pendingIt == m_pendingCalls.end() ?
                          "pending_missing" : "provider_token_missing"));
        }

        ndn::Name serviceCoordinationName =
            makeServiceCoordinationNameV2(identity, providerName, serviceName, requestId);
        ndn::Name serviceCoordinationNameWithoutPrefix =
            makeServiceCoordinationNameWithoutPrefixV2(providerName, serviceName, requestId);

        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_PUBLISH_ATTEMPT timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " messageName=" << serviceCoordinationName.toUri());
        try {
            PublishMessage(serviceCoordinationName,
                           serviceCoordinationNameWithoutPrefix,
                           coordinationMessage);
        }
        catch (const std::exception& e) {
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_PUBLISH_FAILED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestId.toUri()
                      << " providerName=" << providerName.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " reason=exception"
                      << " error=" << e.what());
            throw;
        }
        if (pendingIt != m_pendingCalls.end()) {
            pendingIt->second.coordinationPublishedAtUs = nowMicroseconds();
            addUniqueName(pendingIt->second.coordinatedProviders, providerName);
        }
        updateRequestLifecycleState(requestId, RequestLifecycleState::COORDINATION_PUBLISHED);
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=COORDINATION_PUBLISHED timestamp_us="
                  << nowMicroseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri()
                  << " messageName=" << serviceCoordinationName.toUri());
        NDN_LOG_INFO("[ServiceUser] coordination PublishMessage returned timestampMs="
                  << nowMilliseconds()
                  << " requestId=" << requestId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " serviceName=" << serviceName.toUri());
    }

    void ServiceUser::onMissingData(const std::vector<ndn::svs::MissingDataInfo>& infoVector)
    {
        // for (const auto& info : infoVector) {
        //     NDN_LOG_INFO("onMissingData from node " << info.nodeId
        //                 << " seq range [" << info.low << ", " << info.high << "]");
        // }
    }


    void ServiceUser::OnPermissionTokenDecryptionSuccessCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum, const ndn::Buffer &buffer)
    {
        // Buffer to const char*
        const char* dataPtr = reinterpret_cast<const char*>(buffer.data());
        // log names and tokens
        NDN_LOG_INFO("OnPermissionTokenDecryptionSuccessCallback: " << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << seqNum.toUri() << "token:" << std::string(dataPtr, buffer.size()));
        // insert token into UPT
        UPT.insertPermission(ndn::Name(providerName.toUri()).append(ServiceName).append(FunctionName).toUri(), 
            ndn::Name(ServiceName.toUri()).append(FunctionName).toUri(), 
            std::string(dataPtr, buffer.size()));
    }

    void ServiceUser::OnPermissionTokenDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum, const std::string &error)
    {
        NDN_LOG_ERROR("OnPermissionTokenDecryptionErrorCallback: No access to service " << providerName << ServiceName << FunctionName << " with error: " << error);
    }

    bool ServiceUser::replyFromIMS(const ndn::Interest &interest)
    {
        auto data = m_IMS.find(interest.getName());
        if (data != nullptr)
        {
            NDN_LOG_TRACE("Reply from IMS: " << interest.getName().toUri());
            m_face.put(*data);
        }else{
            m_IMS.size();
            NDN_LOG_TRACE("Not Found In IMS: " << interest.getName().toUri()<<" SIZE: "<< m_IMS.size());
        }
        return false;
    }

    void ServiceUser::onPrefixRegisterFailure(const ndn::Name &prefix, const std::string &reason)
    {
        // log error
        NDN_LOG_ERROR("Prefix registration failed for prefix " << prefix.toUri() << " reason: " << reason);
    }
    void ServiceUser::onInterest(const ndn::InterestFilter &, const ndn::Interest &interest)
    {
        NDN_LOG_INFO("Received Interest: " << interest.getName().toUri());
        replyFromIMS(interest);
        
    }
    void ServiceUser::serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data> &contentData, ndn::nacabe::SPtrVector<ndn::Data> &ckData)
    {
        //log data
        NDN_LOG_INFO("serveDataWithIMS: " << contentData.size() << " " << ckData.size());
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
        ndn::Name serviceName;
        ndn::Name requestId;
        ndn::Name senderPrefix = identity;
        if (auto request = parseRequestNameV2(messageName)) {
            serviceName = request->serviceName;
            requestId = request->requestId;
        }
        else if (auto coordination = parseServiceCoordinationNameV2(messageName)) {
            serviceName = coordination->serviceName;
            requestId = coordination->requestId;
        }
        else {
            NDN_LOG_ERROR("Hybrid publish unsupported message name: " << messageName);
            return;
        }

        const auto messageType = hybridMessageTypeForName(messageName);
        const auto accessAttribute = hybridAccessAttributeForName(messageName, serviceName);
        auto key = m_hybridMessageCrypto.getOrCreateSendKey(
            serviceName, senderPrefix, accessAttribute, messageType, m_hybridCryptoCounters);

        const auto plaintextBlock = message.WireEncode();
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
        const auto aesStartUs = timelineSteadyMicroseconds();
        auto encrypted = hybridAesGcmEncrypt(
            key.key,
            ndn::span<const uint8_t>(&*plaintextBlock.begin(), plaintextBlock.size()),
            ndn::span<const uint8_t>(ad.data(), ad.size()));
        const auto aesEndUs = timelineSteadyMicroseconds();
        if (m_timelineTrace) {
            logTimelineTrace("user", "aes_gcm_encrypt_done", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"messageType", messageType},
                              {"duration_us", std::to_string(aesEndUs >= aesStartUs ?
                                                             aesEndUs - aesStartUs : 0)}});
        }
        envelope.setNonce(encrypted.nonce);
        envelope.setCipherText(encrypted.ciphertext);
        envelope.setAuthTag(encrypted.tag);
        ++m_hybridCryptoCounters.symmetric_encrypt_count;
        if (m_useTokens) {
            if (messageType == "REQUEST" || messageType == "RESPONSE") {
                ++m_hybridCryptoCounters.user_token_symmetric_encrypt_count;
            }
            if (messageType == "ACK" || messageType == "COORDINATION") {
                ++m_hybridCryptoCounters.provider_token_symmetric_encrypt_count;
            }
        }

        if (m_hybridMessageCrypto.shouldAttachWrappedKey(key.keyId)) {
            if (m_timelineTrace) {
                logTimelineTrace("user", "wrapped_key_attached", requestId,
                                 {{"value", "true"},
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
            envelope.setWrappedMessageKey(ndn::Buffer(wrapped.data(), wrapped.size()));
            serveDataWithIMS(contentData, ckData);
            m_hybridMessageCrypto.markSendKeyWrapped(key.keyId);
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
            logTimelineTrace("user", "wrapped_key_attached", requestId,
                             {{"value", "false"},
                              {"serviceName", serviceName.toUri()},
                              {"messageType", messageType}});
        }
        if (m_timelineTrace) {
            logTimelineTrace("user", cryptoStageForName(messageName) + "_crypto_done", requestId,
                             {{"serviceName", serviceName.toUri()},
                              {"messageName", messageName.toUri()},
                              {"mode", "hybrid"}});
        }

        auto envelopeBlock = envelope.WireEncode();
        auto buffer = ndn::Buffer(envelopeBlock.begin(), envelopeBlock.end());
        const auto queuedAtUs = nowMicroseconds();
        NDN_LOG_DEBUG("[NDNSF_HYBRID] role=user event=HYBRID_PUBLISH"
                  << " messageName=" << messageName.toUri()
                  << " messageType=" << messageType
                  << " keyId=" << key.keyId
                  << " epochId=" << key.epochId
                  << " wrappedKeyAttached=" << envelope.hasWrappedMessageKey()
                  << " ciphertextBytes=" << encrypted.ciphertext.size());
        m_face.getIoContext().post(
            [this, messageName, queuedAtUs, buffer = std::move(buffer)]() mutable {
                ndn::Block contentBlock(buffer);
                const auto beginUs = nowMicroseconds();
                NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=SVS_PUBLISH_BEGIN timestamp_us="
                          << beginUs
                          << " messageName=" << messageName.toUri()
                          << " contentBytes=" << contentBlock.value_size()
                          << " eventLoopLagUs=" << (beginUs >= queuedAtUs ? beginUs - queuedAtUs : 0)
                          << " mode=hybrid-message-crypto");
                if (m_timelineTrace) {
                    ndn::Name rid;
                    ndn::Name svc;
                    if (auto request = parseRequestNameV2(messageName)) {
                        rid = request->requestId;
                        svc = request->serviceName;
                    }
                    else if (auto coordination = parseServiceCoordinationNameV2(messageName)) {
                        rid = coordination->requestId;
                        svc = coordination->serviceName;
                    }
                    if (!rid.empty()) {
                        logTimelineTrace("user", cryptoStageForName(messageName) + "_publish_start",
                                         rid,
                                         {{"serviceName", svc.toUri()},
                                          {"messageName", messageName.toUri()},
                                          {"mode", "hybrid"}});
                    }
                }
                m_svsps->publish(messageName, contentBlock);
                if (m_timelineTrace) {
                    ndn::Name rid;
                    ndn::Name svc;
                    if (auto request = parseRequestNameV2(messageName)) {
                        rid = request->requestId;
                        svc = request->serviceName;
                    }
                    else if (auto coordination = parseServiceCoordinationNameV2(messageName)) {
                        rid = coordination->requestId;
                        svc = coordination->serviceName;
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
    }

    bool ServiceUser::decryptHybridMessage(const ndn::Name& messageName,
                                           const ndn::Block& envelopeBlock,
                                           std::function<void(const ndn::Buffer&)> onSuccess,
                                           std::function<void(const std::string&)> onError)
    {
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

        auto finish = [this, envelope, messageName, serviceName, requestId,
                       senderPrefix, onSuccess = std::move(onSuccess),
                       onError = std::move(onError)](const ndn::Buffer& key) mutable {
            const auto ad = hybridAssociatedData(messageName, envelope.getMessageType(),
                                                requestId, serviceName, senderPrefix,
                                                envelope.getKeyId(), envelope.getEpochId());
            ndn::Buffer plaintext;
            if (!hybridAesGcmDecrypt(key, envelope,
                                     ndn::span<const uint8_t>(ad.data(), ad.size()),
                                     plaintext)) {
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
        };

        ndn::Buffer key;
        if (m_hybridMessageCrypto.findReceiveKey(envelope.getKeyId(), key,
                                                 m_hybridCryptoCounters)) {
            finish(key);
            return true;
        }
        if (!envelope.hasWrappedMessageKey()) {
            auto retry = std::make_shared<std::function<void(size_t)>>();
            *retry = [this,
                      keyId = envelope.getKeyId(),
                      finish,
                      onError,
                      retry](size_t remaining) mutable {
                ndn::Buffer retryKey;
                if (m_hybridMessageCrypto.findReceiveKey(keyId, retryKey,
                                                         m_hybridCryptoCounters)) {
                    finish(retryKey);
                    return;
                }
                if (remaining == 0) {
                    if (onError) {
                        onError("hybrid key cache miss and no wrapped MessageKey attached");
                    }
                    return;
                }
                m_scheduler.schedule(ndn::time::milliseconds(25),
                                     [retry, remaining] {
                                         (*retry)(remaining - 1);
                                     });
            };
            (*retry)(40);
            return true;
        }

        ++m_hybridCryptoCounters.nac_abe_key_unwrap_count;
        nacConsumer.consume(ndn::Name(envelope.getKeyId()),
                            ndn::Block(envelope.getWrappedMessageKey()),
                            [this, envelope, finish = std::move(finish)](const ndn::Buffer& unwrappedKey) mutable {
                                m_hybridMessageCrypto.cacheReceiveKey(envelope.getKeyId(),
                                                                      envelope.getEpochId(),
                                                                      unwrappedKey);
                                finish(unwrappedKey);
                            },
                            [onError = std::move(onError)](const std::string& error) {
                                if (onError) {
                                    onError("hybrid MessageKey unwrap failed: " + error);
                                }
                            });
        return true;
    }

    void ServiceUser::PublishMessage(const ndn::Name &messageName, const ndn::Name &messageNameWithoutPrefix,AbstractMessage &message)
    {
        // log message
        NDN_LOG_INFO("PublishMessage: " << messageName.toUri());
        if (m_svsps == nullptr) {
            NDN_LOG_INFO("PublishMessage skipped because SVS publisher is not initialized");
            return;
        }

        auto results = ndn_service_framework::GetAttributesByName(messageName);
        if (!results)
        {
            NDN_LOG_ERROR("GetAttributesByName failed: " << messageName);
            return;
        }
        NDN_LOG_INFO("GetAttributesByName: messageName=" << messageName.toUri()
                     << " attributes=" << formatAttributesForLog(*results));
        if (m_useHybridMessageCrypto) {
            publishHybridMessage(messageName, messageNameWithoutPrefix, message);
            return;
        }
        const auto stage = cryptoStageForName(messageName);
        ndn::Name timelineRequestId;
        ndn::Name timelineServiceName;
        if (auto request = parseRequestNameV2(messageName)) {
            timelineRequestId = request->requestId;
            timelineServiceName = request->serviceName;
        }
        else if (auto coordination = parseServiceCoordinationNameV2(messageName)) {
            timelineRequestId = coordination->requestId;
            timelineServiceName = coordination->serviceName;
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=SVS_PUBLISH_QUEUED timestamp_us="
                      << nowMicroseconds()
                      << " messageName=" << messageName.toUri()
                      << " contentBytes=" << buffer.size()
                      << " contentSegments=0"
                      << " ckSegments=0");
            const auto queuedAtUs = nowMicroseconds();
            m_face.getIoContext().post(
                [this, messageName, queuedAtUs, buffer = std::move(buffer)]() mutable {
                    ndn::Block contentBlock(buffer);
                    const auto beginUs = nowMicroseconds();
                    NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=SVS_PUBLISH_BEGIN timestamp_us="
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
                        else if (auto coordination = parseServiceCoordinationNameV2(messageName)) {
                            requestId = coordination->requestId;
                            serviceName = coordination->serviceName;
                        }
                        if (!requestId.empty()) {
                            logTimelineTrace("user", cryptoStageForName(messageName) + "_publish_start",
                                             requestId,
                                             {{"serviceName", serviceName.toUri()},
                                              {"messageName", messageName.toUri()}});
                        }
                    }
                    m_svsps->publish(messageName, contentBlock);
                    if (m_timelineTrace) {
                        ndn::Name requestId;
                        ndn::Name serviceName;
                        if (auto request = parseRequestNameV2(messageName)) {
                            requestId = request->requestId;
                            serviceName = request->serviceName;
                        }
                        else if (auto coordination = parseServiceCoordinationNameV2(messageName)) {
                            requestId = coordination->requestId;
                            serviceName = coordination->serviceName;
                        }
                        if (!requestId.empty()) {
                            logTimelineTrace("user", cryptoStageForName(messageName) + "_publish_done",
                                             requestId,
                                             {{"serviceName", serviceName.toUri()},
                                              {"messageName", messageName.toUri()}});
                        }
                    }
                    NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=SVS_PUBLISH_DONE timestamp_us="
                              << nowMicroseconds()
                              << " messageName=" << messageName.toUri());
                    NDN_LOG_INFO("Message Published: " << messageName.toUri()
                                 << " " << contentBlock.value_size());
                });
            return;
        }

        std::vector<uint8_t> plaintext(plaintextBlock.begin(), plaintextBlock.end());
        ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=PRODUCE_STARTED timestamp_us="
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=PRODUCE_COMPLETED timestamp_us="
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
            NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=PRODUCE_EMPTY_CONTENT timestamp_us="
                      << nowMicroseconds()
                      << " messageName=" << messageName.toUri()
                      << " stage=" << stage
                      << " mode=synchronous-user-publish"
                      << " contentSegments=" << contentData.size()
                      << " ckSegments=" << ckData.size());
            return;
        }
        const auto queuedAtUs = nowMicroseconds();
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=SVS_PUBLISH_QUEUED timestamp_us="
                  << queuedAtUs
                  << " messageName=" << messageName.toUri()
                  << " contentBytes=" << buffer.size()
                  << " contentSegments=" << contentData.size()
                  << " ckSegments=" << ckData.size()
                  << " mode=synchronous-user-publish");

        serveDataWithIMS(contentData, ckData);
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=IMS_INSERT_DONE timestamp_us="
                  << nowMicroseconds()
                  << " messageName=" << messageName.toUri()
                  << " contentSegments=" << contentData.size()
                  << " ckSegments=" << ckData.size()
                  << " mode=synchronous-user-publish");
        ndn::Block contentBlock(buffer);
        const auto beginUs = nowMicroseconds();
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=SVS_PUBLISH_BEGIN timestamp_us="
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
        m_svsps->publish(messageName, contentBlock);
        if (m_timelineTrace && !timelineRequestId.empty()) {
            logTimelineTrace("user", stage + "_publish_done", timelineRequestId,
                             {{"serviceName", timelineServiceName.toUri()},
                              {"messageName", messageName.toUri()}});
        }
        NDN_LOG_DEBUG("[NDNSF_TRACE] role=user event=SVS_PUBLISH_DONE timestamp_us="
                  << nowMicroseconds()
                  << " messageName=" << messageName.toUri()
                  << " mode=synchronous-user-publish");
        NDN_LOG_INFO("Message Published: " << messageName.toUri()
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


    void ServiceUser::OnResponseDecryptionErrorCallback(const ndn::Name& serviceProviderName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const std::string & msg)
    {
        NDN_LOG_INFO("[ServiceUser] OnResponseDecryptionErrorCallback provider="
                  << serviceProviderName.toUri()
                  << " service=" << ServiceName.toUri()
                  << " function=" << FunctionName.toUri()
                  << " requestID=" << RequestID.toUri()
                  << " error=" << msg);
        NDN_LOG_ERROR("OnResponseDecryptionErrorCallback: " << serviceProviderName << ServiceName << FunctionName << RequestID << " with error: " << msg);
    }
}
