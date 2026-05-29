#include <ServiceProvider.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>

#include <ndn-cxx/security/validation-error.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace ndn_service_framework
{
    NDN_LOG_INIT(ndn_service_framework.ServiceProvider);

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
            return std::getenv("NDNSF_SVS_ASYNC_PUBLISH") == nullptr ||
                   isTruthyEnv("NDNSF_SVS_ASYNC_PUBLISH");
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
        collaborationAssociatedData(const ndn::Name& dataName,
                                    const ndn::Name& requestId,
                                    const CollaborationDataMessage& message,
                                    const std::string& keyId,
                                    const std::string& epochId)
        {
            const std::string text =
                dataName.toUri() + "|COLLAB|" + requestId.toUri() + "|" +
                message.getKeyScope() + "|" + message.getTopic().toUri() + "|" +
                message.getProducerRole() + "|" +
                std::to_string(message.getSequence()) + "|" + keyId + "|" + epochId;
            return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        }

        void
        publishSvs(const std::shared_ptr<ndn::svs::SVSPubSub>& svs,
                   const ndn::Name& name,
                   const ndn::Block& content)
        {
            if (svs == nullptr) {
                return;
            }
            if (useAsyncSvsPublish()) {
                svs->publishAsync(name, content);
            }
            else {
                svs->publish(name, content);
            }
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
                              const UserPermissionTable& permissionTable)
        {
            return permissionTable.queryPermission(
                ndn::Name(providerIdentity.toUri()).append(serviceName).toUri(),
                serviceName.toUri()).has_value();
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
            const UserPermissionTable& permissionTable)
        {
            const auto rolePermission =
                makeCollaborationRolePermissionName(serviceName, role);
            return permissionTable.queryPermission(
                ndn::Name(providerIdentity.toUri()).append(rolePermission).toUri(),
                rolePermission.toUri()).has_value();
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

    ServiceProvider::ServiceProvider(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate, std::string trustSchemaPath) 
        : m_face(face),
        m_scheduler(m_face.getIoContext()),
        identity(identityCert.getIdentity()),
        validator(std::make_shared<MessageValidator>(trustSchemaPath)),
        identityCert(identityCert),
        // nac_validator(std::move(ndn::security::ValidatorNull())),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        random(ndn::random::getRandomNumberEngine()),
        m_IMS(50000)
    {
        m_handlerPool.setThreadCount(defaultNdnsfWorkerThreads());
        m_ackPool.setThreadCount(defaultNdnsfAckThreads());
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

        NDN_LOG_INFO("[ServiceProvider] NAC_ABE_BOOTSTRAP provider="
                  << identity.toUri()
                  << " authority=" << attrAuthorityCertificate.getIdentity().toUri()
                  << " dkPrefix="
                  << ndn::Name(attrAuthorityCertificate.getIdentity()).append("DKEY").toUri());

        nacConsumer.obtainDecryptionKey();

        // Serve NDNSF and ck messages using IMS
        const ndn::Name ndnsfFilter = ndn::Name(identity.toUri()).append("NDNSF");
        const ndn::Name ckFilter = ndn::Name(identity.toUri()).append("CK");
        NDN_LOG_INFO("[ServiceProvider] registered service content prefix="
                  << ndnsfFilter.toUri());
        m_face.setInterestFilter(ndnsfFilter,
            std::bind(&ServiceProvider::onInterest, this, _1, _2),
            std::bind(&ServiceProvider::onPrefixRegisterFailure, this, _1, _2));
        m_face.setInterestFilter(ckFilter,
            std::bind(&ServiceProvider::onInterest, this, _1, _2),
            std::bind(&ServiceProvider::onPrefixRegisterFailure, this, _1, _2));

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
            const int suppressionMs =
                std::max(0, intEnvOrDefault("NDNSF_SVS_MAX_SUPPRESSION_MS", 1));
            m_svsps->getSVSync().getCore().setMaxSuppressionTime(
                ndn::time::milliseconds(suppressionMs));
            NDN_LOG_INFO("NDNSF_SVS_MAX_SUPPRESSION_MS role=provider value="
                         << suppressionMs);
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
            const bool enableParallelSync =
                std::getenv("NDNSF_SVS_PARALLEL_SYNC") == nullptr ||
                isTruthyEnv("NDNSF_SVS_PARALLEL_SYNC");
            if (enableParallelSync) {
                const int workers = std::max(1, intEnvOrDefault("NDNSF_SVS_PARALLEL_WORKERS", 4));
                const int queue = std::max(1, intEnvOrDefault("NDNSF_SVS_PARALLEL_QUEUE", 256));
                m_svsps->getSVSync().getCore().setParallelSyncProcessing(
                    true, static_cast<size_t>(workers), static_cast<size_t>(queue));
                NDN_LOG_INFO("NDNSF_SVS_PARALLEL_SYNC enabled role=provider workers="
                             << workers << " queue=" << queue);
            }
            const bool enableParallelProduction =
                std::getenv("NDNSF_SVS_PARALLEL_PRODUCTION") == nullptr ||
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
            if (isTruthyEnv("NDNSF_SVS_SYNC_BATCHING")) {
                const int windowMs = std::max(0, intEnvOrDefault("NDNSF_SVS_SYNC_BATCH_MS", 5));
                m_svsps->getSVSync().getCore().setSyncInterestBatching(
                    true, ndn::time::milliseconds(windowMs));
                NDN_LOG_INFO("NDNSF_SVS_SYNC_BATCHING enabled role=provider windowMs="
                             << windowMs);
            }
        }

        while(!nacConsumer.readyForDecryption()){
            // log waiting for decryption key
            NDN_LOG_INFO("DK_INTEREST_EXPRESSED prefix="
                      << ndn::Name(attrAuthorityCertificate.getIdentity()).append("DKEY").toUri()
                      << " provider=" << identity.toUri());
            nacConsumer.obtainDecryptionKey();
            NDN_LOG_INFO("Waiting for decryption key");
            face.processEvents(ndn::time::milliseconds(1000));
        }
        NDN_LOG_INFO("DK_DECRYPT_SUCCESS provider=" << identity.toUri());


    }

    ServiceProvider::ServiceProvider(LocalMockTag,
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
        attrAuthorityCertificate(attrAuthorityCertificate),
        nacConsumer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        nacProducer(m_face, m_keyChain, nac_validator, identityCert, attrAuthorityCertificate),
        random(ndn::random::getRandomNumberEngine()),
        m_IMS(50000),
        m_configManager("/tmp/ndnsf-service-provider-local-mock.conf")
    {
        m_signingInfo = ndn::security::signingByCertificate(identityCert);
    }

    void ServiceProvider::init()
    {
        registerServiceInfo();
        registerNDNSFMessages();
    }

    ServiceProvider::~ServiceProvider()
    {
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
                         << " mainBlockingMs=" << stats.syncInterestMainThreadBlockingMs);
        }
        m_cryptoProduceQueue.shutdown();
        m_ackPool.shutdown();
        m_handlerPool.shutdown();
    }

    void ServiceProvider::ConsumeRequest(const ndn::Name& RequesterName,
                                         const ndn::Name& providerName,
                                         const ndn::Name& ServiceName,
                                         const ndn::Name& FunctionName,
                                         const ndn::Name& RequestID,
                                         RequestMessage&)
    {
        const auto unifiedServiceName = makeUnifiedServiceName(ServiceName, FunctionName);
        NDN_LOG_ERROR("No legacy ConsumeRequest handler registered for requester="
                      << RequesterName.toUri()
                      << " provider=" << providerName.toUri()
                      << " service=" << unifiedServiceName.toUri()
                      << " requestId=" << RequestID.toUri());
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
        m_services[serviceName] = {std::move(ackHandler), std::move(requestHandler)};
        const auto serviceUri = serviceName.toUri();
        if (std::find(m_serviceNames.begin(), m_serviceNames.end(), serviceUri) ==
            m_serviceNames.end()) {
            m_serviceNames.push_back(serviceUri);
        }
        NDN_LOG_WARN("[ServiceProvider] registered service prefix="
                  << serviceUri);
        NDN_LOG_WARN("Registered service handler for " << serviceUri);
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
                                     const ndn::Name& functionName,
                                     AckStrategyHandler ackHandler,
                                     RequestHandler requestHandler)
    {
        addService(makeUnifiedServiceName(serviceName, functionName),
                   std::move(ackHandler),
                   std::move(requestHandler));
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     const ndn::Name& functionName,
                                     LegacyAckStrategyHandler ackHandler,
                                     RequestHandler requestHandler)
    {
        addService(makeUnifiedServiceName(serviceName, functionName),
                   wrapLegacyAckStrategyHandler(std::move(ackHandler)),
                   std::move(requestHandler));
    }

    void ServiceProvider::addService(const ndn::Name& serviceName,
                                     const ndn::Name& functionName,
                                     RequestHandler requestHandler)
    {
        addService(makeUnifiedServiceName(serviceName, functionName),
                   std::move(requestHandler));
    }

    void ServiceProvider::RegisterService(const ServiceName& serviceName,
                                          AckStrategyHandler ackHandler,
                                          RequestHandler requestHandler)
    {
        addService(serviceName, std::move(ackHandler), std::move(requestHandler));
    }

    void ServiceProvider::RegisterService(const ServiceName& serviceName,
                                          RequestHandler requestHandler)
    {
        addService(serviceName, std::move(requestHandler));
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
        m_collaborationServices[serviceName] =
            {std::move(ackHandler), std::move(handler), std::move(allowedRoles)};
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
    }

    SessionId ServiceProvider::CollaborationContext::sessionId() const
    {
        return m_requestId.toUri();
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

    std::optional<ndn::Buffer>
    ServiceProvider::CollaborationContext::fetchLarge(const ndn::Name& dataName,
                                                      KeyScope keyScope,
                                                      int timeoutMs)
    {
        return m_provider.fetchCollaborationLargeData(m_requestId,
                                                      std::move(keyScope),
                                                      dataName,
                                                      timeoutMs);
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

    void ServiceProvider::CollaborationContext::publishFinalResponse(
        const ndn::Buffer& payload)
    {
        m_provider.publishCollaborationFinalResponse(m_requesterName,
                                                     m_assignment.service,
                                                     m_requestId,
                                                     m_requestMessage,
                                                     payload);
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

    void ServiceProvider::setAckStrategyHandler(const ndn::Name& serviceName,
                                                const ndn::Name& functionName,
                                                AckStrategyHandler ackHandler)
    {
        setAckStrategyHandler(makeUnifiedServiceName(serviceName, functionName),
                              std::move(ackHandler));
    }

    void ServiceProvider::setLegacyAckStrategyHandler(
        const ndn::Name& serviceName,
        LegacyAckStrategyHandler ackHandler)
    {
        setAckStrategyHandler(serviceName,
                              wrapLegacyAckStrategyHandler(std::move(ackHandler)));
    }

    void ServiceProvider::setLegacyAckStrategyHandler(
        const ndn::Name& serviceName,
        const ndn::Name& functionName,
        LegacyAckStrategyHandler ackHandler)
    {
        setLegacyAckStrategyHandler(makeUnifiedServiceName(serviceName, functionName),
                                    std::move(ackHandler));
    }

    bool ServiceProvider::hasService(const ndn::Name& serviceName) const
    {
        return m_services.find(serviceName) != m_services.end();
    }

    size_t ServiceProvider::getPendingRequestCountForTesting() const
    {
        return pendingRequests.size();
    }

    size_t ServiceProvider::getSelectedOutstandingRequestCountForTesting() const
    {
        return m_selectedOutstandingRequests.load(std::memory_order_relaxed);
    }

    size_t ServiceProvider::getPendingProviderTokenCountForTesting() const
    {
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
        AckStrategyHandler ackHandler)
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
             ackHandler = std::move(ackHandler)]() mutable {
                AckDecision decision;
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

                m_face.getIoContext().post(
                    [this,
                     requesterIdentity,
                     serviceName,
                     requestId,
                     requestMessage,
                     decision = std::move(decision)]() mutable {
                        finishAckDecisionOnEventLoop(requesterIdentity,
                                                     serviceName,
                                                     requestId,
                                                     std::move(requestMessage),
                                                     std::move(decision));
                    });
            });

        if (!queued) {
            AckDecision decision;
            decision.status = false;
            decision.message = "ACK handler queue full";
            finishAckDecisionOnEventLoop(requesterIdentity,
                                         serviceName,
                                         requestId,
                                         std::move(requestMessage),
                                         std::move(decision));
        }
        return true;
    }

    void ServiceProvider::finishAckDecisionOnEventLoop(
        const ndn::Name& requesterIdentity,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        RequestMessage requestMessage,
        AckDecision decision)
    {
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
        if (decision.status) {
            pendingRequests[pendingKey] =
                std::make_shared<RequestMessage>(requestMessage);
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PENDING_REQUEST_STORED timestamp_us="
                      << nowMicroseconds()
                      << " providerName=" << identity.toUri()
                      << " requestId=" << requestId.toUri()
                      << " serviceName=" << serviceName.toUri()
                      << " pendingKey=" << pendingKey.toUri());
            schedulePendingRequestCleanup(pendingKey);
        }

        const std::string providerToken =
            (m_useTokens && decision.status) ? makeOneTimeToken() : "";
        if (m_useTokens && decision.status) {
            pendingProviderTokens[pendingKey] = providerToken;
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
                                   providerToken);
    }

    bool ServiceProvider::dispatchRequestExecutionAsync(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        RequestMessage requestMessage)
    {
        if (m_handlerPool.getThreadCount() == 0) {
            return false;
        }

        auto service = m_services.find(serviceName);
        if (service == m_services.end() || !service->second.requestHandler) {
            return false;
        }
        auto requestHandler = service->second.requestHandler;

        const bool queued = m_handlerPool.post(
            [this,
             requesterName,
             providerName,
             serviceName,
             requestId,
             requestMessage,
             requestHandler = std::move(requestHandler)]() mutable {
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

                m_face.getIoContext().post(
                    [this,
                     requesterName,
                     providerName,
                     serviceName,
                     requestId,
                     requestMessage,
                     response = std::move(response)]() mutable {
                        finishRequestExecutionOnEventLoop(requesterName,
                                                          providerName,
                                                          serviceName,
                                                          requestId,
                                                          requestMessage,
                                                          std::move(response));
                    });
            });

        if (!queued) {
            publishExecutionFailureOnEventLoop(requesterName,
                                               providerName,
                                               serviceName,
                                               requestId,
                                               requestMessage,
                                               "Request handler queue full");
        }
        return true;
    }

    bool ServiceProvider::dispatchCollaborationExecutionAsync(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        RequestMessage requestMessage,
        CollaborationAssignment assignment)
    {
        auto service = m_collaborationServices.find(serviceName);
        if (service == m_collaborationServices.end() || !service->second.handler) {
            return false;
        }

        const auto handler = service->second.handler;
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
                "Provider is not authorized for collaboration role " + assignment.role);
            return true;
        }
        if (!service->second.allowedRoles.empty() &&
            !hasProviderCollaborationRolePermission(identity, serviceName,
                                                    assignment.role, UPT)) {
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
                    assignment.role);
            return true;
        }
        auto assignmentForPreparation = assignment;
        auto assignmentForHandler = std::move(assignment);
        prepareCollaborationAssignmentAsync(
            requestId,
            std::move(assignmentForPreparation),
            [this,
             requesterName,
             providerName,
             serviceName,
             requestId,
             requestMessage,
             assignment = std::move(assignmentForHandler),
             handler](bool ready, std::string error) mutable {
                if (!ready) {
                    publishExecutionFailureOnEventLoop(
                        requesterName,
                        providerName,
                        serviceName,
                        requestId,
                        requestMessage,
                        "Collaboration assignment preparation failed: " + error);
                    return;
                }

                auto runHandler =
                    [this,
                     requesterName,
                     serviceName,
                     requestId,
                     requestMessage,
                     assignment = std::move(assignment),
                     handler]() mutable {
                        try {
                            CollaborationContext ctx(*this,
                                                     requesterName,
                                                     requestId,
                                                     requestMessage,
                                                     std::move(assignment));
                            handler(ctx, requestMessage);
                        }
                        catch (const std::exception& e) {
                            NDN_LOG_ERROR("Collaboration handler failed for "
                                          << serviceName.toUri() << ": " << e.what());
                        }
                        catch (...) {
                            NDN_LOG_ERROR("Collaboration handler failed for "
                                          << serviceName.toUri());
                        }
                        m_face.getIoContext().post(
                            [this, requestId, serviceName] {
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
                if (!queued) {
                    publishExecutionFailureOnEventLoop(
                        requesterName,
                        providerName,
                        serviceName,
                        requestId,
                        requestMessage,
                        "Collaboration handler queue full");
                }
            });
        return true;
    }

    void ServiceProvider::finishRequestExecutionOnEventLoop(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage,
        ResponseMessage response)
    {
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
        if (m_useTokens) {
            response.setUserToken(requestMessage.getUserToken());
        }
        response.setPolicyEpoch(m_currentPolicyEpoch);
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
            size_t selectedOutstanding =
                m_selectedOutstandingRequests.load(std::memory_order_relaxed);
            while (selectedOutstanding > 0 &&
                   !m_selectedOutstandingRequests.compare_exchange_weak(
                       selectedOutstanding,
                       selectedOutstanding - 1,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
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
            throw;
        }
    }

    void ServiceProvider::publishExecutionFailureOnEventLoop(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage,
        const std::string& error)
    {
        ResponseMessage response = makeErrorResponse(error);
        finishRequestExecutionOnEventLoop(requesterName,
                                          providerName,
                                          serviceName,
                                          requestId,
                                          requestMessage,
                                          std::move(response));
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

        auto encryptAndPublish = [this,
                                  name,
                                  requestId,
                                  scopeKey = std::move(scopeKey),
                                  plaintext = payload,
                                  message = std::move(message)]() mutable {
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
            }
            catch (const std::exception& e) {
                error = e.what();
            }

            m_face.getIoContext().post(
                [this, name, encoded = std::move(encoded),
                 error = std::move(error)]() mutable {
                    if (!error.empty()) {
                        NDN_LOG_ERROR("Collaboration data encryption failed for "
                                      << name.toUri() << ": " << error);
                        return;
                    }
                    ndn::Block block(encoded);
                    publishSvs(m_svsps, name, block);
                });
        };
        if (m_handlerPool.getThreadCount() == 0 ||
            !m_handlerPool.post(encryptAndPublish)) {
            encryptAndPublish();
        }
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

        ndn::Segmenter segmenter(m_keyChain, m_signingInfo);
        auto segments = segmenter.segment(
            ndn::span<const uint8_t>(encoded.data(), encoded.size()),
            name,
            maxSegmentSize == 0 ? 7000 : maxSegmentSize,
            ndn::time::milliseconds(freshnessMs <= 0 ? 60000 : freshnessMs));

        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            for (const auto& data : segments) {
                m_IMS.insert(*data, ndn::time::milliseconds(freshnessMs <= 0 ? 60000 : freshnessMs));
            }
        }
        NDN_LOG_DEBUG("COLLAB_LARGE_PUBLISHED name=" << name.toUri()
                      << " plaintextBytes=" << payload.size()
                      << " segments=" << segments.size());
        return name;
    }

    std::optional<ndn::Buffer>
    ServiceProvider::fetchCollaborationLargeData(
        const ndn::Name& requestId,
        const std::string& keyScope,
        const ndn::Name& dataName,
        int timeoutMs)
    {
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

        m_face.getIoContext().post([this, dataName, completed, mutex, cv, error, encoded] {
            ndn::Interest interest(dataName);
            interest.setCanBePrefix(true);
            interest.setMustBeFresh(true);
            interest.setInterestLifetime(ndn::time::seconds(4));
            ndn::SegmentFetcher::Options options;
            options.probeLatestVersion = false;
            options.useConstantCwnd = true;
            options.initCwnd = 4.0;
            options.maxTimeout = ndn::time::seconds(10);
            auto fetcher = ndn::SegmentFetcher::start(m_face, interest, nac_validator, options);
            fetcher->onComplete.connect(
                [completed, mutex, cv, encoded](ndn::ConstBufferPtr buffer) {
                    {
                        std::lock_guard<std::mutex> lock(*mutex);
                        encoded->assign(buffer->begin(), buffer->end());
                        completed->store(true);
                    }
                    cv->notify_one();
                });
            fetcher->onError.connect(
                [completed, mutex, cv, error](uint32_t code, const std::string& msg) {
                    {
                        std::lock_guard<std::mutex> lock(*mutex);
                        *error = "SegmentFetcher error " + std::to_string(code) + ": " + msg;
                        completed->store(true);
                    }
                    cv->notify_one();
                });
        });

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs <= 0 ? 5000 : timeoutMs);
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

        try {
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
        const ndn::Buffer& payload)
    {
        ResponseMessage response;
        response.setStatus(true);
        ndn::Buffer responsePayload(payload);
        response.setPayload(responsePayload, responsePayload.size());
        if (m_useTokens) {
            response.setUserToken(requestMessage.getUserToken());
        }
        response.setPolicyEpoch(m_currentPolicyEpoch);
        m_face.getIoContext().post(
            [this,
             requesterName,
             serviceName,
             requestId,
             requestMessage,
             response = std::move(response)]() mutable {
                finishRequestExecutionOnEventLoop(requesterName,
                                                  identity,
                                                  serviceName,
                                                  requestId,
                                                  requestMessage,
                                                  std::move(response));
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

    void ServiceProvider::prepareCollaborationAssignmentAsync(
        const ndn::Name& requestId,
        CollaborationAssignment assignment,
        std::function<void(bool, std::string)> onReady)
    {
        struct FetchState
        {
            ndn::Name requestId;
            CollaborationAssignment assignment;
            std::function<void(bool, std::string)> onReady;
            size_t pending = 0;
            bool failed = false;
            std::string error;
            std::map<KeyScope, ndn::Buffer> fetchedKeys;
            ndn::Buffer fetchedArtifact;
        };

        auto state = std::make_shared<FetchState>();
        state->requestId = requestId;
        state->assignment = std::move(assignment);
        state->onReady = std::move(onReady);

        {
            std::lock_guard<std::mutex> lock(m_collaborationMutex);
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

        state->pending = keysToFetch.size() + (needsArtifactFetch ? 1 : 0);

        auto finishIfReady = [this, state]() mutable {
            if (state->pending != 0) {
                return;
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

            state->onReady(!state->failed, state->error);
        };

        const auto provisioningTimeoutMs =
            state->assignment.provisioningTimeoutMs > 0 ?
                state->assignment.provisioningTimeoutMs : 30000;
        const auto provisioningLifetime =
            ndn::time::milliseconds(provisioningTimeoutMs);

        auto startFetch = [this, state, finishIfReady, provisioningLifetime](
                              const ndn::Name& dataName,
                              std::function<void(const ndn::Buffer&)> onPlaintext) mutable {
            m_face.getIoContext().post(
                [this,
                 state,
                 finishIfReady,
                 provisioningLifetime,
                 dataName,
                 onPlaintext = std::move(onPlaintext)]() mutable {
                    ndn::Interest interest(dataName);
                    interest.setCanBePrefix(true);
                    interest.setMustBeFresh(true);
                    interest.setInterestLifetime(provisioningLifetime);

                    try {
                        nacConsumer.consume(
                            interest,
                            [state,
                             onPlaintext = std::move(onPlaintext),
                             finishIfReady](const ndn::Buffer& buffer) mutable {
                                onPlaintext(buffer);
                                if (state->pending > 0) {
                                    --state->pending;
                                }
                                finishIfReady();
                            },
                            [state, dataName, finishIfReady](
                                const std::string& reason) mutable {
                                state->failed = true;
                                if (!state->error.empty()) {
                                    state->error += "; ";
                                }
                                state->error += dataName.toUri() + ": " + reason;
                                if (state->pending > 0) {
                                    --state->pending;
                                }
                                finishIfReady();
                            });
                    }
                    catch (const std::exception& e) {
                        state->failed = true;
                        if (!state->error.empty()) {
                            state->error += "; ";
                        }
                        state->error += dataName.toUri() + ": " + e.what();
                        if (state->pending > 0) {
                            --state->pending;
                        }
                        finishIfReady();
                    }
                });
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

            m_face.getIoContext().post(
                [this, ok, data = std::move(data), dataName]() mutable {
                    if (!ok) {
                        NDN_LOG_ERROR("Collaboration data authentication failed for "
                                      << dataName.toUri());
                        return;
                    }
                    deliverCollaborationData(data);
                });
        };
        if (m_handlerPool.getThreadCount() == 0 ||
            !m_handlerPool.post(decryptAndDeliver)) {
            decryptAndDeliver();
        }
    }

    bool ServiceProvider::maybeFetchCollaborationScopeKey(
        const ndn::Name& requestId,
        const KeyScope& keyScope)
    {
        ndn::Name keyDataName;
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
            if (!m_collaborationScopeKeyFetchesInFlight.insert(fetchKey).second) {
                return false;
            }
            keyDataName = nameIt->second;
        }

        ndn::Interest interest(keyDataName);
        interest.setCanBePrefix(true);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::seconds(4));

        m_face.getIoContext().post(
            [this, interest, requestId, keyScope, fetchKey]() mutable {
                try {
                    nacConsumer.consume(
                        interest,
                        [this, requestId, keyScope, fetchKey](
                            const ndn::Buffer& buffer) {
                            std::vector<PendingEncryptedCollaborationData> pending;
                            {
                                std::lock_guard<std::mutex> lock(m_collaborationMutex);
                                m_collaborationScopeKeyFetchesInFlight.erase(fetchKey);
                                if (buffer.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                                    NDN_LOG_ERROR("Fetched invalid collaboration scope key for "
                                                  << requestId.toUri()
                                                  << " scope=" << keyScope);
                                    return;
                                }
                                m_collaborationScopeKeysByRequest[requestId][keyScope] =
                                    buffer;
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
                        },
                        [this, requestId, keyScope, fetchKey](
                            const std::string& reason) {
                            std::lock_guard<std::mutex> lock(m_collaborationMutex);
                            m_collaborationScopeKeyFetchesInFlight.erase(fetchKey);
                            NDN_LOG_ERROR("Failed to fetch collaboration scope key for "
                                          << requestId.toUri()
                                          << " scope=" << keyScope
                                          << ": " << reason);
                        });
                }
                catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(m_collaborationMutex);
                    m_collaborationScopeKeyFetchesInFlight.erase(fetchKey);
                    NDN_LOG_ERROR("Failed to start collaboration scope key fetch for "
                                  << requestId.toUri()
                                  << " scope=" << keyScope
                                  << ": " << e.what());
                }
            });
        return true;
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

        const auto fields = parseSemicolonFields(payload);
        auto readField = [&fields](const std::string& key) {
            auto it = fields.find(key);
            return it == fields.end() ? std::string() : it->second;
        };

        assignment.role = readField("role");
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
        assignment.requiresProvisioning =
            readField("requiresProvisioning") == "1";
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
        }
        const auto artifactData = readField("artifactData");
        if (!artifactData.empty()) {
            assignment.artifactPayload = hexDecode(artifactData);
        }
        return assignment;
    }

    bool ServiceProvider::hasService(const ndn::Name& serviceName,
                                     const ndn::Name& functionName) const
    {
        return hasService(makeUnifiedServiceName(serviceName, functionName));
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

        if (!service->second.requestHandler) {
            return makeErrorResponse("Registered service has no request handler for " +
                                     serviceName.toUri());
        }

        return service->second.requestHandler(requesterIdentity,
                                              providerName,
                                              serviceName,
                                              requestId,
                                              requestMessage);
    }

    ResponseMessage ServiceProvider::dispatchRequest(
        const ndn::Name& requesterIdentity,
        const ndn::Name& providerName,
        const ndn::Name& serviceName,
        const ndn::Name& functionName,
        const ndn::Name& requestId,
        const RequestMessage& requestMessage) const
    {
        return dispatchRequest(requesterIdentity,
                               providerName,
                               makeUnifiedServiceName(serviceName, functionName),
                               requestId,
                               requestMessage);
    }

    ResponseMessage ServiceProvider::handleDecryptedRequestByName(
        const ndn::Name& requestName,
        const RequestMessage& requestMessage) const
    {
        auto parsedV2 = ndn_service_framework::parseRequestNameV2(requestName);
        if (parsedV2) {
            if (!isAcceptablePolicyEpoch(requestMessage.getPolicyEpoch())) {
                return makeErrorResponse("Stale policy epoch for " +
                                         parsedV2->serviceName.toUri());
            }
            if (!hasProviderPermission(identity, parsedV2->serviceName, UPT)) {
                return makeErrorResponse("Permission denied for " +
                                         parsedV2->serviceName.toUri());
            }
            if (m_useTokens && requestMessage.getUserToken().empty()) {
                return makeErrorResponse("Missing UserToken for " +
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
            response.setPolicyEpoch(m_currentPolicyEpoch);
            return response;
        }

        auto parsed = parseRequestNameForUnifiedService(requestName);
        if (!parsed) {
            return makeErrorResponse("Failed to parse request name " + requestName.toUri());
        }

        if (!isAcceptablePolicyEpoch(requestMessage.getPolicyEpoch())) {
            return makeErrorResponse("Stale policy epoch for " +
                                     parsed->serviceName.toUri());
        }

        if (!hasProviderPermission(identity, parsed->serviceName, UPT)) {
            return makeErrorResponse("Permission denied for " +
                                     parsed->serviceName.toUri());
        }
        if (m_useTokens && requestMessage.getUserToken().empty()) {
            return makeErrorResponse("Missing UserToken for " +
                                     parsed->serviceName.toUri());
        }
        if (requestMessage.getStrategy() == tlv::AllSelected) {
            return makeErrorResponse("AllSelected requires selection before execution for " +
                                     parsed->serviceName.toUri());
        }

        auto response = dispatchRequest(parsed->requesterIdentity,
                                        identity,
                                        parsed->serviceName,
                                        parsed->requestId,
                                        requestMessage);
        if (m_useTokens) {
            response.setUserToken(requestMessage.getUserToken());
        }
        response.setPolicyEpoch(m_currentPolicyEpoch);
        return response;
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

    ndn::Name ServiceProvider::makeUnifiedServiceName(const ndn::Name& serviceName,
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

    std::optional<ServiceProvider::ParsedRequestName>
    ServiceProvider::parseRequestNameForUnifiedService(const ndn::Name& requestName)
    {
        auto parsed = ndn_service_framework::parseRequestName(requestName);
        if (!parsed) {
            return std::nullopt;
        }

        ndn::Name requesterIdentity;
        ndn::Name serviceName;
        ndn::Name functionName;
        ndn::Name bloomFilterName;
        ndn::Name requestId;
        std::tie(requesterIdentity, serviceName, functionName, bloomFilterName, requestId) =
            parsed.value();

        return ParsedRequestName{
            requesterIdentity,
            makeUnifiedServiceName(serviceName, functionName),
            requestId};
    }

    void ServiceProvider::cleanupPendingRequestState(const ndn::Name& pendingKey)
    {
        ++m_cleanupInvocationCount;
        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PENDING_CLEANUP timestamp_us="
                  << nowMicroseconds()
                  << " providerName=" << identity.toUri()
                  << " pendingKey=" << pendingKey.toUri()
                  << " hadRequest=" << (pendingRequests.find(pendingKey) != pendingRequests.end())
                  << " hadProviderToken="
                  << (pendingProviderTokens.find(pendingKey) != pendingProviderTokens.end()));
        pendingRequests.erase(pendingKey);
        pendingProviderTokens.erase(pendingKey);
    }

    bool ServiceProvider::expirePendingRequestState(const ndn::Name& pendingKey)
    {
        const bool hadRequest = pendingRequests.find(pendingKey) != pendingRequests.end();
        const bool hadToken = pendingProviderTokens.find(pendingKey) != pendingProviderTokens.end();
        if (!hadRequest && !hadToken) {
            return false;
        }

        NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PENDING_EXPIRED timestamp_us="
                  << nowMicroseconds()
                  << " providerName=" << identity.toUri()
                  << " pendingKey=" << pendingKey.toUri()
                  << " hadRequest=" << hadRequest
                  << " hadProviderToken=" << hadToken);
        if (!pendingKey.empty()) {
            updateProviderRequestLifecycleState(
                ndn::Name(pendingKey[-1].toUri()),
                ndn::Name(),
                ProviderRequestLifecycleState::PROVIDER_REQUEST_EXPIRED);
        }
        cleanupPendingRequestState(pendingKey);
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
        m_face.getIoContext().post(
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

        if (m_hybridMessageCrypto.shouldAttachWrappedKey(key.keyId)) {
            if (m_timelineTrace) {
                logTimelineTrace("provider", "wrapped_key_attached", requestId,
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
                logTimelineTrace("provider", "hybrid_key_wrap_done", requestId,
                                 {{"serviceName", serviceName.toUri()},
                                  {"messageType", messageType},
                                  {"duration_us", std::to_string(wrapEndUs >= wrapStartUs ?
                                                                 wrapEndUs - wrapStartUs : 0)}});
            }
        }
        else if (m_timelineTrace) {
            logTimelineTrace("provider", "wrapped_key_attached", requestId,
                             {{"value", "false"},
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
            m_face.getIoContext().post(
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
                                          {"messageName", messageName.toUri()},
                                          {"mode", "hybrid"}});
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

    bool ServiceProvider::decryptHybridMessage(const ndn::Name& messageName,
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
        if (auto request = parseRequestNameV2(messageName)) {
            serviceName = request->serviceName;
            requestId = request->requestId;
            senderPrefix = request->requesterName;
        }
        else if (auto selection = parseServiceSelectionNameV2(messageName)) {
            serviceName = selection->serviceName;
            requestId = selection->requestId;
            senderPrefix = selection->requesterName;
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
            auto decryptAndPost = [this, key, envelope, ad,
                                   onSuccess = std::move(onSuccess),
                                   onError = std::move(onError)]() mutable {
                ndn::Buffer plaintext;
                const bool ok = hybridAesGcmDecrypt(
                    key, envelope, ndn::span<const uint8_t>(ad.data(), ad.size()), plaintext);
                m_face.getIoContext().post(
                    [this, ok, envelope, plaintext = std::move(plaintext),
                     onSuccess = std::move(onSuccess),
                     onError = std::move(onError)]() mutable {
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
            finish(key);
            return true;
        }
        if (!envelope.hasWrappedMessageKey()) {
            ndn::Name keyDataName = senderPrefix;
            keyDataName.append(ndn::Name(envelope.getKeyId()));
            ++m_hybridCryptoCounters.nac_abe_key_unwrap_count;
            nacConsumer.consume(
                keyDataName,
                [this, envelope, finish = std::move(finish)](const ndn::Buffer& unwrappedKey) mutable {
                    m_hybridMessageCrypto.cacheReceiveKey(envelope.getKeyId(),
                                                          envelope.getEpochId(),
                                                          unwrappedKey);
                    finish(unwrappedKey);
                },
                [onError = std::move(onError), keyDataName](const std::string& error) {
                    if (onError) {
                        onError("hybrid MessageKey fetch failed " +
                                keyDataName.toUri() + ": " + error);
                    }
                });
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

    void ServiceProvider::schedulePendingRequestCleanup(const ndn::Name& pendingKey,
                                                        ndn::time::milliseconds ttl)
    {
        m_scheduler.schedule(ttl, [this, pendingKey] {
            const bool hadRequest = pendingRequests.find(pendingKey) != pendingRequests.end();
            const bool hadToken = pendingProviderTokens.find(pendingKey) != pendingProviderTokens.end();
            if (!hadRequest && !hadToken) {
                return;
            }
            if (m_pendingRequestTimeoutGrace.count() <= 0) {
                expirePendingRequestState(pendingKey);
                return;
            }
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=PENDING_GRACE_STARTED timestamp_us="
                      << nowMicroseconds()
                      << " providerName=" << identity.toUri()
                      << " pendingKey=" << pendingKey.toUri()
                      << " graceMs=" << m_pendingRequestTimeoutGrace.count()
                      << " hadRequest=" << hadRequest
                      << " hadProviderToken=" << hadToken);
            m_scheduler.schedule(m_pendingRequestTimeoutGrace, [this, pendingKey] {
                expirePendingRequestState(pendingKey);
            });
        });
    }

    void ServiceProvider::PublishMessage(const ndn::Name &messageName, const ndn::Name &messageNameWithoutPrefix,AbstractMessage &message)
    {
        // log message
        NDN_LOG_DEBUG("PublishMessage: " << messageName.toUri());

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
            m_face.getIoContext().post(
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
                    nacProducer.produce(
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
                            nacProducer.produce(
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
                    m_face.getIoContext().post(
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
        // for (const auto& info : infoVector) {
        //     NDN_LOG_INFO("onMissingData from node " << info.nodeId
        //                 << " seq range [" << info.low << ", " << info.high << "]");
        // }
    }

    void ServiceProvider::UpdateUPTWithServiceMetaInfo(ndnsd::discovery::Details serviceDetails)
    {
        if (serviceDetails.serviceMetaInfo.find("tokenName") != serviceDetails.serviceMetaInfo.end()) {
            std::string tokenName = serviceDetails.serviceMetaInfo.at("tokenName");

            // parse token and get return values
            ndn::Name providerName, ServiceName, FunctionName, seqNum;
                
            auto result = ndn_service_framework::parsePermissionTokenName(ndn::Name(tokenName));
            if (!result){
                NDN_LOG_ERROR("Invalid Permission Token Name: " << tokenName);    
                return;
            }
            std::tie(providerName, ServiceName, FunctionName, seqNum) = result.value();
            // update UPT
            std::string token = ndn_service_framework::RandomString(16);
            // log tokenName and token
            NDN_LOG_INFO("TokenName: " << tokenName << " Token: " << token);

            UPT.insertPermission(ndn::Name(identity.toUri()).append(ServiceName).append(FunctionName).toUri(), 
                                ndn::Name(ServiceName.toUri()).append(FunctionName).toUri(),
                                token);
            // encrypt the token with nac-abe; and then serve it using IMS
            ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
            const std::vector<std::string> attributes = {"/PERMISSION"+ndn::Name(identity.toUri()).append(ServiceName).append(FunctionName).toUri()};
            std::tie(contentData, ckData) =
                nacProducer.produce(ndn_service_framework::makePermissionTokenNameWithoutPrefix(ServiceName, FunctionName, seqNum),
                    attributes, ndn::make_span(reinterpret_cast<const uint8_t *>(token.data()), token.size()), m_signingInfo);
            // std::tie(contentData, ckData) =
            //     nacProducer.produce(ndn_service_framework::makePermissionTokenNameWithoutPrefix(ServiceName, FunctionName, seqNum),
            //         ndn_service_framework::ConcatenateString(attributes), ndn::make_span(reinterpret_cast<const uint8_t *>(token.data()), token.size()), m_signingInfo);
            
            
            // serve data
            for (auto data : contentData)
                m_IMS.insert(*data);
            for (auto data : ckData)
                m_IMS.insert(*data);

        }
    }


    void ServiceProvider::OnRequest(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) return;
        NDN_LOG_DEBUG("[ServiceProvider] OnRequest name="
                  << subscription.name.toUri()
                  << " producer=" << subscription.producerPrefix.toUri()
                  << " bytes=" << subscription.data.size());
        // log the request
        NDN_LOG_DEBUG("OnRequest: " << subscription.name << " " << subscription.data.size());

        auto requestV2 = ndn_service_framework::parseRequestNameV2(subscription.name);
        if (requestV2) {
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=REQUEST_RECEIVED timestamp_us="
                      << nowMicroseconds()
                      << " requestId=" << requestV2->requestId.toUri()
                      << " serviceName=" << requestV2->serviceName.toUri()
                      << " requestName=" << subscription.name.toUri());
            if (m_timelineTrace) {
                logTimelineTrace("provider", "request_observed", requestV2->requestId,
                                 {{"serviceName", requestV2->serviceName.toUri()},
                                  {"requesterName", requestV2->requesterName.toUri()},
                                  {"requestName", subscription.name.toUri()}});
            }
            auto token = UPT.queryPermission(
                ndn::Name(identity.toUri()).append(requestV2->serviceName).toUri(),
                requestV2->serviceName.toUri());
            if(!token)
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
                                                   ndn::Name(),
                                                   requestV2->requestId,
                                                   _1),
                                         std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback,
                                                   this,
                                                   requestV2->requesterName,
                                                   requestV2->serviceName,
                                                   ndn::Name(),
                                                   requestV2->requestId,
                                                   _1))) {
                    return;
                }
                OnRequestDecryptionErrorCallback(requestV2->requesterName,
                                                 requestV2->serviceName,
                                                 ndn::Name(),
                                                 requestV2->requestId,
                                                 "invalid hybrid request envelope");
                return;
            }
            else{
                nacConsumer.consume(subscription.name,
                                    std::bind(&ServiceProvider::OnRequestDecryptionSuccessCallbackV2,
                                              this,
                                              requestV2->requesterName,
                                              requestV2->serviceName,
                                              ndn::Name(),
                                              requestV2->requestId,
                                              _1),
                                    std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback,
                                              this,
                                              requestV2->requesterName,
                                              requestV2->serviceName,
                                              ndn::Name(),
                                              requestV2->requestId,
                                              _1));

            }
            return;
        }
        
        ndn::Name RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId;
        
        auto resutls = ndn_service_framework::parseRequestName(subscription.name);
        if(!resutls)
        {
            NDN_LOG_ERROR("OnRequest: parseRequestName failed: " << subscription.name);
            return; // parseRequestName failed
        }
        std::tie(RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId) = resutls.value();

        // check whether its identity in the bloom filter
        std::string bfStr = bloomFilterName.toUri().substr(1);
        
        ndn_service_framework::BloomFilter bloomFilter;
        if(!bloomFilter.fromHexString(bfStr))
        {
            NDN_LOG_ERROR("OnRequest: BloomFilter parse failed: " << bfStr);
            return;
        }
        bool isTarget = bloomFilter.contains(this->identity.toUri());
        // log bfstr and isTarget
        NDN_LOG_INFO("BloomFilter: " << bfStr << isTarget);

        if(isTarget)    // if the identity is in the bloom filter, consume the request
        {
            auto token = UPT.queryPermission(ndn::Name(identity.toUri()).append(ServiceName).append(FunctionName).toUri(), ndn::Name(ServiceName).append(FunctionName).toUri());
            // for (auto item : UPT.userPermissions){
            //     NDN_LOG_INFO("UPT: " << item.left << " " << item.right.first << " " << item.right.second);
            // }
            if(!token)
            {
                NDN_LOG_ERROR("Not serving: " << ServiceName << " function " << FunctionName);
                return;
            }
            // fetch and decrypt the request, and then PreProcess it to check permisison and publish ACK;
            if(subscription.data.size() > 0){
                nacConsumer.consume(subscription.name,
                                    ndn::Block(subscription.data),
                                    std::bind(&ServiceProvider::OnRequestDecryptionSuccessCallback, this, RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId,  _1),
                                    std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback, this, RequesterName, ServiceName, FunctionName, RequestId, _1));
       
            }else{
                nacConsumer.consume(subscription.name,
                                    std::bind(&ServiceProvider::OnRequestDecryptionSuccessCallback, this, RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId,  _1),
                                    std::bind(&ServiceProvider::OnRequestDecryptionErrorCallback, this, RequesterName, ServiceName, FunctionName, RequestId, _1));
       
            }
           
            //preprocessRequest(RequesterName, ServiceName, FunctionName, bloomFilterName, RequestId);
        }
        else    // if the identity is not in the bloom filter, return error message
        {
            NDN_LOG_ERROR("OnRequest: Requester is not in the bloom filter: " << RequesterName);
        }

    }

void ServiceProvider::OnRequestDecryptionSuccessCallbackV2(
    const ndn::Name& requesterIdentity,
    const ndn::Name& serviceName,
    const ndn::Name& bloomFilterName,
    const ndn::Name& requestId,
    const ndn::Buffer& buffer)
{
    auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());
    auto decodeAndFinish = [this, requesterIdentity, serviceName, bloomFilterName,
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

        m_face.getIoContext().post(
            [this, requesterIdentity, serviceName, bloomFilterName, requestId,
             raw,
             requestMessage = std::move(requestMessage)]() mutable {
                finishDecodedRequestOnEventLoop(requesterIdentity,
                                                serviceName,
                                                bloomFilterName,
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
                                        bloomFilterName,
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
    const ndn::Name& bloomFilterName,
    const ndn::Name& requestId,
    ndn_service_framework::RequestMessage requestMessage)
{
    NDN_LOG_DEBUG("OnRequestDecryptionSuccessCallbackV2: "
        << requesterIdentity.toUri()
        << serviceName.toUri()
        << bloomFilterName.toUri()
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

    if (!isAcceptablePolicyEpoch(requestMessage.getPolicyEpoch())) {
        NDN_LOG_ERROR("Reject request with stale policy epoch requestId="
                      << requestId.toUri()
                      << " receivedEpoch=" << requestMessage.getPolicyEpoch()
                      << " currentEpoch=" << m_currentPolicyEpoch);
        return;
    }

    if (!hasProviderPermission(identity, serviceName, UPT)) {
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
                                         std::move(asyncAckHandler))) {
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
                                     std::move(decision));
        return;
    }

    NDN_LOG_INFO("No V2 dynamic handler for " << serviceName.toUri());

    ndn::Name pendingKey = ndn::Name(requesterIdentity.toUri())
                               .append(serviceName)
                               .append(requestId);
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
    pendingRequests[pendingKey] =
        std::make_shared<RequestMessage>(requestMessage);
    schedulePendingRequestCleanup(pendingKey);

    std::string msg = "Permission Granted";
    const std::string providerToken = m_useTokens ? makeOneTimeToken() : "";
    if (m_useTokens) {
        pendingProviderTokens[pendingKey] = providerToken;
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

void ServiceProvider::OnRequestDecryptionSuccessCallback(
    const ndn::Name& requesterIdentity,
    const ndn::Name& ServiceName,
    const ndn::Name& FunctionName,
    const ndn::Name& bloomFilterName,
    const ndn::Name& RequestID,
    const ndn::Buffer& buffer)
{
    // Deep copy buffer so it is safe to pass across threads
    auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());

    // ndnsf::post([this,
    //              requesterIdentity,
    //              ServiceName,
    //              FunctionName,
    //              bloomFilterName,
    //              RequestID,
    //              raw]() mutable
    // {
        // Reconstruct a safe TLV block
        auto spanBuf = ndn::span<const uint8_t>(raw->data(), raw->size());
        auto [ok, block] = ndn::Block::fromBuffer(spanBuf);

        // Logging
        NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: "
            << requesterIdentity.toUri()
            << ServiceName.toUri()
            << FunctionName.toUri()
            << bloomFilterName.toUri()
            << RequestID.toUri());

        const ndn::Name unifiedServiceName = makeUnifiedServiceName(ServiceName, FunctionName);
        if (!hasProviderPermission(identity, unifiedServiceName, UPT)) {
            NDN_LOG_ERROR("Not Serving: " << ServiceName << " function " << FunctionName);
            return;
        }

        // Decode RequestMessage safely
        ndn_service_framework::RequestMessage requestMessage;
        requestMessage.WireDecode(block);

        if (m_useTokens && requestMessage.getUserToken().empty()) {
            NDN_LOG_ERROR("OnRequestDecryptionSuccessCallback: Missing UserToken");
            return;
        }
        NDN_LOG_INFO("OnRequestDecryptionSuccessCallback: Permission Granted to "
            << requesterIdentity.toUri()
            << " for " << ServiceName.toUri()
            << " function " << FunctionName.toUri());

        if (hasService(unifiedServiceName)) {
            NDN_LOG_INFO("Dispatch request using dynamic handler for "
                         << unifiedServiceName.toUri());

            ndn::Name pendingKey = ndn::Name(requesterIdentity.toUri())
                                       .append(ServiceName)
                                       .append(FunctionName)
                                       .append(RequestID);
            pendingRequests[pendingKey] =
                std::make_shared<RequestMessage>(requestMessage);
            schedulePendingRequestCleanup(pendingKey);

            auto service = m_services.find(unifiedServiceName);
            AckDecision decision = makeDefaultAckDecision();
            if (service != m_services.end() && service->second.ackHandler) {
                decision = service->second.ackHandler(requestMessage);
                if (decision.message.empty()) {
                    decision.message =
                        decision.status ? "Permission Granted" : "Permission Denied";
                }
            }

            const std::string providerToken =
                (m_useTokens && decision.status) ? makeOneTimeToken() : "";
            if (m_useTokens && decision.status) {
                pendingProviderTokens[pendingKey] = providerToken;
            }

            PublishRequestAckMessage(requesterIdentity,
                                     ServiceName,
                                     FunctionName,
                                     RequestID,
                                     decision.status,
                                     decision.message,
                                     decision.payload,
                                     m_useTokens ? requestMessage.getUserToken() : "",
                                     providerToken);
            return;
        }

        NDN_LOG_INFO("No dynamic handler for "
                     << unifiedServiceName.toUri()
                     << "; preserving ACK/selection path");

        // Save request into pendingRequests
        ndn::Name pendingKey = ndn::Name(requesterIdentity.toUri())
                                   .append(ServiceName)
                                   .append(FunctionName)
                                   .append(RequestID);

        pendingRequests[pendingKey] =
            std::make_shared<RequestMessage>(requestMessage);
        schedulePendingRequestCleanup(pendingKey);

        // Send Permission ACK
        std::string msg = "Permission Granted";
        const std::string providerToken = m_useTokens ? makeOneTimeToken() : "";
        if (m_useTokens) {
            pendingProviderTokens[pendingKey] = providerToken;
        }
        PublishRequestAckMessage(requesterIdentity,
                                 ServiceName,
                                 FunctionName,
                                 RequestID,
                                 true,
                                 msg,
                                 ndn::Buffer(),
                                 m_useTokens ? requestMessage.getUserToken() : "",
                                 providerToken);
    //});
}




    void ServiceProvider::OnRequestDecryptionErrorCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const std::string &)
    {
        // log error
        NDN_LOG_ERROR("OnRequestDecryptionErrorCallback: " << requesterIdentity.toUri() << ServiceName.toUri() << FunctionName.toUri() << RequestID.toUri());
    }

void ServiceProvider::processNDNSDServiceInfoCallback(const ndnsd::discovery::Details & callback)
{
        NDN_LOG_INFO("Service publish callback received");
}

    void ServiceProvider::onPermissionResponseData(const ndn::Interest& interest,
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

    void ServiceProvider::onPermissionResponseTimeout(const ndn::Interest& interest)
    {
        NDN_LOG_ERROR("PermissionResponse timeout: " << interest.getName());
    }

    void ServiceProvider::fetchPolicyManifestFromController(const ndn::Name& controllerPrefix)
    {
        ndn::Name interestName(controllerPrefix);
        interestName.append(ndn::Name("/NDNSF/POLICY-MANIFEST"));

        ndn::Interest interest(interestName);
        interest.setCanBePrefix(false);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::seconds(4));

        NDN_LOG_INFO("Fetch policy manifest: " << interestName);
        m_face.expressInterest(
            interest,
            std::bind(&ServiceProvider::onPolicyManifestData, this, _1, _2),
            [this](const ndn::Interest& interest, const ndn::lp::Nack&) {
                onPolicyManifestTimeout(interest);
            },
            std::bind(&ServiceProvider::onPolicyManifestTimeout, this, _1));
    }

    void ServiceProvider::onPolicyManifestData(const ndn::Interest& interest,
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

    void ServiceProvider::onPolicyManifestTimeout(const ndn::Interest& interest)
    {
        NDN_LOG_ERROR("PolicyManifest timeout: " << interest.getName());
    }

// void ServiceProvider::PublishResponse(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer &buffer)
//     {
//         //  /<identity>/NDNSF/RESPONSE/<requesterIdentity>/<ServiceName>/<FunctionName>/<request-id>
//         // identity will be appended by NAC-ABE
        
//         ndn::Name responseName = ndn_service_framework::makeResponseName(identity, requesterIdentity, ServiceName, FunctionName, RequestID);
//         ndn::Name responseNameWithoutPrefix = ndn_service_framework::makeResponseNameWithoutPrefix(requesterIdentity, ServiceName, FunctionName, RequestID);
//         NDN_LOG_INFO("PublishResponse:"<<responseName);
//         // Encrypt the response with nac-abe
//         // publish the encrypted response with ndn-svs, and insert ck into the repo
//         //  contentData segments, and ckData segments
//         ndn::nacabe::SPtrVector<ndn::Data> contentData, ckData;
//         const std::vector<std::string> attributes = {"/ID"+requesterIdentity.toUri()};
//         std::tie(contentData, ckData) =
//             nacProducer.produce(responseNameWithoutPrefix, attributes, ndn::make_span(reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size()), m_signingInfo);
//         // serve data
//         for (auto data : contentData){
//             //NDN_LOG_INFO(data->getName());
//             m_IMS.insert(*data);
//         }
//         for (auto data : ckData)
//             m_IMS.insert(*data);
//         // content
//         // ndn::BufferPtr contentBuffer = ndn_service_framework::CombineSegmentsIntoBuffer(contentData);
//         // ndn::BufferPtr ckBuffer = ndn_service_framework::CombineSegmentsIntoBuffer(ckData);

//         m_svsps->publishAsync(responseName);
//         //m_svsps->publishAsync(responseName, ndn::make_span(reinterpret_cast<const uint8_t *>(contentBuffer->data()), contentBuffer->size()));
//         NDN_LOG_INFO("Publish Encrypted response" << contentData.at(0)->getName().getPrefix(-1));
//         //m_svsps->publishAsync(ckData.at(0)->getName().getPrefix(-1));
//         //m_svsps->publishAsync(ckData.at(0)->getName().getPrefix(-1), ndn::make_span(reinterpret_cast<const uint8_t *>(ckBuffer->data()), ckBuffer->size()));
//         //NDN_LOG_INFO("ServiceProvider_Drone::PublishResponse CK" << ckData.at(0)->getName().getPrefix(-1));
//     }

    bool ServiceProvider::replyFromIMS(const ndn::Interest &interest)
    {
        std::shared_ptr<const ndn::Data> data;
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            data = m_IMS.find(interest);
        }
        if (data != nullptr)
        {
            NDN_LOG_TRACE("Reply from IMS: " << interest.getName().toUri());
            m_face.put(*data);
        }else{
            NDN_LOG_TRACE("Not Found In IMS: " << interest.getName().toUri());
            // for(auto d:m_IMS)
            // {
            //     NDN_LOG_TRACE("In IMS: " << d.getName().toUri());
            // }
        }
        return false;
    }

    void ServiceProvider::onPrefixRegisterFailure(const ndn::Name &prefix, const std::string &reason)
    {
        // log error
        NDN_LOG_ERROR("Prefix registration failed for prefix " << prefix.toUri() << " reason: " << reason);
    }
    void ServiceProvider::onInterest(const ndn::InterestFilter &, const ndn::Interest &interest)
    {
        // log interest
        NDN_LOG_DEBUG("Received Interest: " << interest.getName().toUri());
        replyFromIMS(interest);
        
    }

    void ServiceProvider::serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data> &contentData, ndn::nacabe::SPtrVector<ndn::Data> &ckData)
    {
        //log data
        NDN_LOG_DEBUG("serveDataWithIMS: " << contentData.size() << " " << ckData.size());
        std::lock_guard<std::mutex> lock(_cache_mutex);
        for (auto data : contentData)
        {
            m_IMS.insert(*data);
        }
        for (auto data : ckData)
        {
            m_IMS.insert(*data); 
        }
    }

    LargeDataFetchResult ServiceProvider::fetchAndDecryptLargeData(
        const ndn::Name& encryptedDataName,
        const std::string& serviceName)
    {
        LargeDataFetchResult result;
        if (encryptedDataName.empty()) {
            result.errorMessage = "encryptedDataName is empty";
            return result;
        }
        if (serviceName.empty()) {
            result.errorMessage = "serviceName is empty";
            return result;
        }

        auto completed = std::make_shared<std::atomic<bool>>(false);
        auto mutex = std::make_shared<std::mutex>();
        auto cv = std::make_shared<std::condition_variable>();
        auto error = std::make_shared<std::string>();
        auto plaintext = std::make_shared<ndn::Buffer>();

        m_face.getIoContext().post([this, encryptedDataName, completed, mutex, cv, error, plaintext] {
            ndn::Interest interest(encryptedDataName);
            interest.setCanBePrefix(true);
            interest.setMustBeFresh(true);
            interest.setInterestLifetime(ndn::time::seconds(4));

            try {
                nacConsumer.consume(
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

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        std::unique_lock<std::mutex> lock(*mutex);
        cv->wait_until(lock, deadline, [&completed] { return completed->load(); });

        if (!completed->load()) {
            result.errorMessage = "large-data fetch timed out or data not found";
            return result;
        }
        if (!error->empty()) {
            result.errorMessage = "large-data authorization/decryption failure for " +
                                  serviceName + ": " + *error;
            return result;
        }

        result.plaintext.assign(plaintext->begin(), plaintext->end());
        result.success = true;
        return result;
    }



    void ServiceProvider::PublishRequestAckMessage(const ndn::Name & requesterIdentity, const ndn::Name & ServiceName, const ndn::Name & FunctionName, const ndn::Name & RequestID, bool status, const std::string& msg, const ndn::Buffer& payload, const std::string& userToken, const std::string& providerToken)
    {
        // log message
        NDN_LOG_DEBUG("PublishRequestAckMessage: " << requesterIdentity.toUri() << ServiceName.toUri() << FunctionName.toUri() << RequestID.toUri());
        NDN_LOG_DEBUG("[ServiceProvider] ACK publish requestId="
                  << RequestID.toUri()
                  << " userToken=" << userToken
                  << " providerToken=" << providerToken);
        // create Permission Ack Message
        RequestAckMessage RequestAckMessage;
        RequestAckMessage.setStatus(status);
        RequestAckMessage.setMessage(msg);
        RequestAckMessage.setUserToken(userToken);
        RequestAckMessage.setProviderToken(providerToken);
        if (!payload.empty()) {
            ndn::Buffer ackPayload(payload);
            RequestAckMessage.setPayload(ackPayload, ackPayload.size());
        }

        ndn::Name name = makeRequestAckName(identity, requesterIdentity, ServiceName, FunctionName, RequestID);
        ndn::Name nameWithouPrefix = makeRequestAckNameWithoutPrefix(requesterIdentity, ServiceName, FunctionName, RequestID);
        PublishMessage(name,nameWithouPrefix,RequestAckMessage);
    }

    void ServiceProvider::PublishRequestAckMessageV2(const ndn::Name& requesterIdentity,
                                                     const ndn::Name& serviceName,
                                                     const ndn::Name& requestId,
                                                     bool status,
                                                     const std::string& msg,
                                                     const ndn::Buffer& payload,
                                                     const std::string& userToken,
                                                     const std::string& providerToken)
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
        requestAckMessage.setPolicyEpoch(m_currentPolicyEpoch);
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
    }

    void ServiceProvider::onServiceSelectionMessage(const ndn::svs::SVSPubSub::SubscriptionData &subscription)
    {
        if(!isFresh(subscription)) return;

        auto selectionV2 =
            ndn_service_framework::parseServiceSelectionNameV2(subscription.name);
        if (selectionV2) {
            if (!selectionV2->providerName.equals(identity)) {
                return;
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
                                ndn::Name(), requestId, error);
                        })) {
                    return;
                }
                OnServiceSelectionMessageDecryptionErrorCallback(
                    selectionV2->requesterName,
                    selectionV2->providerName,
                    selectionV2->serviceName,
                    ndn::Name(),
                    selectionV2->requestId,
                    "invalid hybrid selection envelope");
                return;
                nacConsumer.consume(subscription.name,
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
                                            ndn::Name(), requestId, error);
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
                nacConsumer.consume(subscription.name,
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
                                            ndn::Name(), requestId, error);
                                    });
            }
            return;
        }

        // parse ServiceSelectionMessage
        ndn::Name requesterName, providerName, ServiceName, FunctionName, msgId;
        auto results = ndn_service_framework::parseServiceSelectionName(subscription.name);
        if (!results)
        {
            NDN_LOG_ERROR("parseServiceSelectionMessageName failed: " << subscription.name.toUri());
            return;
        }
        std::tie(requesterName, providerName, ServiceName, FunctionName, msgId) = results.value();
        if (!providerName.equals(identity)) {
            return;
        }
        NDN_LOG_DEBUG("Received Service Selection Message: "
                      << subscription.name.toUri());
        NDN_LOG_DEBUG("[ServiceProvider] selection received timestampMs="
                  << nowMilliseconds()
                  << " requestId=" << msgId.toUri()
                  << " providerName=" << providerName.toUri()
                  << " requesterName=" << requesterName.toUri()
                  << " serviceName=" << ServiceName.toUri());
        // fetch and decrypt the request, and then PreProcess it to check permisison and publish ACK;
        if(subscription.data.size() > 0){
            nacConsumer.consume(subscription.name,
                                ndn::Block(subscription.data),
                                std::bind(&ServiceProvider::OnServiceSelectionMessageDecryptionSuccessCallback, this, requesterName, providerName, ServiceName, FunctionName, msgId, _1),
                                std::bind(&ServiceProvider::OnServiceSelectionMessageDecryptionErrorCallback, this, requesterName, providerName, ServiceName, FunctionName, msgId, _1));
    
        }else{
            nacConsumer.consume(subscription.name,
                                std::bind(&ServiceProvider::OnServiceSelectionMessageDecryptionSuccessCallback, this, requesterName, providerName, ServiceName, FunctionName, msgId, _1),
                                std::bind(&ServiceProvider::OnServiceSelectionMessageDecryptionErrorCallback, this, requesterName, providerName, ServiceName, FunctionName, msgId, _1));
        }
        
 

    }

    ndn::Name ServiceProvider::getName()
    {
        return identity;
    }

    void ServiceProvider::fetchPermissionsFromController(const ndn::Name& controllerPrefix)
    {
        fetchPolicyManifestFromController(controllerPrefix);

        ndn::Name interestName(controllerPrefix);
        interestName.append(ndn::Name("/NDNSF/PERMISSIONS/PROVIDER"));
        interestName.append(identity);

        ndn::Interest interest(interestName);
        interest.setCanBePrefix(true);
        interest.setMustBeFresh(true);
        interest.setInterestLifetime(ndn::time::seconds(4));

        NDN_LOG_INFO("Fetch provider permissions: " << interestName);
        m_face.expressInterest(
            interest,
            std::bind(&ServiceProvider::onPermissionResponseData, this, _1, _2),
            [this](const ndn::Interest& interest, const ndn::lp::Nack&) {
                onPermissionResponseTimeout(interest);
            },
            std::bind(&ServiceProvider::onPermissionResponseTimeout, this, _1));
    }

    void ServiceProvider::applyPermissionResponse(const PermissionResponse& response)
    {
        if (response.getPermissionKind() != tlv::ProviderPermission) {
            NDN_LOG_ERROR("Ignoring non-provider PermissionResponse for "
                          << response.getTargetIdentity());
            return;
        }

        m_currentPolicyEpoch = response.getPolicyEpoch();

        for (const auto& entry : response.getEntries()) {
            if (entry.getVersion() != 0 && entry.getVersion() != m_currentPolicyEpoch) {
                NDN_LOG_WARN("Permission entry epoch differs from response epoch provider="
                             << entry.getProviderName()
                             << " service=" << entry.getServiceName()
                             << " entryEpoch=" << entry.getVersion()
                             << " responseEpoch=" << m_currentPolicyEpoch);
            }
            ndn::Name providerServiceName(entry.getProviderName());
            providerServiceName.append(ndn::Name(entry.getServiceName()));
            UPT.insertPermission(providerServiceName.toUri(),
                                 entry.getServiceName(),
                                 entry.getToken());
            NDN_LOG_WARN("Installed provider permission provider="
                         << entry.getProviderName()
                         << " service=" << entry.getServiceName()
                         << " policyEpoch=" << entry.getVersion());
        }
    }

    size_t ServiceProvider::getCurrentPolicyEpoch() const
    {
        return m_currentPolicyEpoch;
    }

    bool ServiceProvider::isAcceptablePolicyEpoch(size_t messageEpoch) const
    {
        return m_currentPolicyEpoch == 0 || messageEpoch == 0 ||
               messageEpoch == m_currentPolicyEpoch;
    }

    bool ServiceProvider::handlePermissionResponseData(const ndn::Data& data,
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

        if (response.getPermissionKind() != tlv::ProviderPermission) {
            NDN_LOG_ERROR("Ignoring non-provider PermissionResponse for "
                          << response.getTargetIdentity());
            return false;
        }

        for (const auto& entry : response.getEntries()) {
            ndn::Name providerServiceName(entry.getProviderName());
            providerServiceName.append(ndn::Name(entry.getServiceName()));
            permissionTable.insertPermission(providerServiceName.toUri(),
                                             entry.getServiceName(),
                                             entry.getToken());
            NDN_LOG_WARN("Installed provider permission provider="
                         << entry.getProviderName()
                         << " service=" << entry.getServiceName());
        }
        return true;
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

        ServiceSelectionMessage message;
        message.WireDecode(block);
        if (!isAcceptablePolicyEpoch(message.getPolicyEpoch())) {
            NDN_LOG_ERROR("Reject V2 selection with stale policy epoch for "
                          << msgId.toUri()
                          << " receivedEpoch=" << message.getPolicyEpoch()
                          << " currentEpoch=" << m_currentPolicyEpoch);
            return;
        }

        auto key = ndn::Name(requesterName.toUri())
                    .append(serviceName)
                    .append(msgId);

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
            return;
        }

        auto providerTokenIt = pendingProviderTokens.find(key);
        if (m_timelineTrace) {
            logTimelineTrace("provider", "provider_token_validate_start", msgId,
                             {{"serviceName", serviceName.toUri()}});
        }
        if (m_useTokens &&
            (providerTokenIt == pendingProviderTokens.end() ||
             message.getProviderToken() != providerTokenIt->second)) {
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
                      << !message.getProviderToken().empty());
            NDN_LOG_ERROR("Reject V2 selection with mismatched ProviderToken for "
                          << key.toUri());
            return;
        }
        if (m_timelineTrace) {
            logTimelineTrace("provider", "provider_token_validate_done", msgId,
                             {{"serviceName", serviceName.toUri()},
                              {"valid", "true"}});
        }
        if (m_useTokens) {
            ++m_tokenConsumeCount;
        }

        for (const auto& requestID : message.getRequestIDs()) {
            const ndn::Name requestId(requestID);
            auto collabService = m_collaborationServices.find(serviceName);
            if (!hasService(serviceName) &&
                collabService == m_collaborationServices.end()) {
                NDN_LOG_INFO("No V2 dynamic handler for " << serviceName.toUri());
                continue;
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
            m_selectedOutstandingRequests.fetch_add(1, std::memory_order_relaxed);
            RequestMessage requestCopy = *(it->second);
            if (collabService != m_collaborationServices.end()) {
                auto assignment =
                    parseCollaborationAssignment(serviceName,
                                                 message.getAssignmentPayload());
                if (dispatchCollaborationExecutionAsync(requesterName,
                                                        providerName,
                                                        serviceName,
                                                        requestId,
                                                        requestCopy,
                                                        std::move(assignment))) {
                    continue;
                }
            }
            if (dispatchRequestExecutionAsync(requesterName,
                                              providerName,
                                              serviceName,
                                              requestId,
                                              requestCopy)) {
                continue;
            }

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
                                              std::move(response));
        }

        cleanupPendingRequestState(key);
    }


    void ServiceProvider::OnServiceSelectionMessageDecryptionSuccessCallback(
        const ndn::Name& requesterName,
        const ndn::Name& providerName,
        const ndn::Name& ServiceName,
        const ndn::Name& FunctionName,
        const ndn::Name& msgID,
        const ndn::Buffer& buffer)
    {
        // Deep copy the buffer (never pass ndn::Buffer or ndn::Block across threads)
        auto raw = std::make_shared<std::vector<uint8_t>>(buffer.begin(), buffer.end());

        // ndnsf::post([this,
        //             requesterName,
        //             providerName,
        //             ServiceName,
        //             FunctionName,
        //             msgID,
        //             raw]() mutable
        // {
            // Reconstruct Block on NDNSF thread (safe)
            auto spanBuf = ndn::span<const uint8_t>(raw->data(), raw->size());
            auto [ok, block] = ndn::Block::fromBuffer(spanBuf);

            NDN_LOG_DEBUG("OnServiceSelectionMessageDecryptionSuccessCallback: "
                << requesterName.toUri()
                << providerName.toUri()
                << ServiceName.toUri()
                << FunctionName.toUri()
                << msgID.toUri());

            // Decode ServiceSelectionMessage
            ServiceSelectionMessage message;
            message.WireDecode(block);

            // Build lookup key
            auto key = ndn::Name(requesterName.toUri())
                        .append(ServiceName)
                        .append(FunctionName)
                        .append(msgID);

            auto it = pendingRequests.find(key);
            if (it != pendingRequests.end()) {
                auto providerTokenIt = pendingProviderTokens.find(key);
                if (m_useTokens &&
                    (providerTokenIt == pendingProviderTokens.end() ||
                     message.getProviderToken() != providerTokenIt->second)) {
                    NDN_LOG_ERROR("Reject selection with mismatched ProviderToken for "
                                  << key.toUri());
                    return;
                }
                if (m_useTokens) {
                    ++m_tokenConsumeCount;
                }

                for (const auto& requestID : message.getRequestIDs()) {
                    const auto unifiedServiceName = makeUnifiedServiceName(ServiceName, FunctionName);
                    if (hasService(unifiedServiceName)) {
                        auto response = dispatchRequest(requesterName,
                                                        providerName,
                                                        unifiedServiceName,
                                                        ndn::Name(requestID),
                                                        *(it->second));
                        if (m_useTokens) {
                            response.setUserToken(it->second->getUserToken());
                        }
                        response.setPolicyEpoch(m_currentPolicyEpoch);
                        ndn::Name responseName = makeResponseName(providerName,
                                                                  requesterName,
                                                                  ServiceName,
                                                                  FunctionName,
                                                                  ndn::Name(requestID));
                        ndn::Name responseNameWithoutPrefix =
                            makeResponseNameWithoutPrefix(requesterName,
                                                          ServiceName,
                                                          FunctionName,
                                                          ndn::Name(requestID));
                        PublishMessage(responseName, responseNameWithoutPrefix, response);
                        continue;
                    }

                    // Consume corresponding request
                    ConsumeRequest(
                        requesterName,
                        providerName,
                        ServiceName,
                        FunctionName,
                        ndn::Name(requestID),
                        *(it->second));
                }

                // Remove pending record
                cleanupPendingRequestState(key);
            }
        // });
    }



    void ServiceProvider::OnServiceSelectionMessageDecryptionErrorCallback(const ndn::Name &requesterName, const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID, const std::string &reason)
    {
        // log error
        NDN_LOG_ERROR("OnServiceSelectionMessageDecryptionErrorCallback: " << requesterName.toUri() << providerName.toUri() << ServiceName.toUri() << FunctionName.toUri() << msgID.toUri() << " reason: " << reason);

    }

    void ServiceProvider::registerNDNSFMessages()
    {
        // log register
        NDN_LOG_WARN("Register NDNSF Messages in ndn-svs");
        for(auto serviceName:m_serviceNames){ 
            // register Request Message
            ndn::Name sname(serviceName);
            std::string regex_str =
                "^(<>*)<NDNSF><REQUEST>" +
                ndn_service_framework::NameToRegexString(sname) +
                "(<>)$";
            // V2 requests are published as:
            //   /<requester>/NDNSF/REQUEST/<serviceName...>/<requestId>
            // The service-specific regex keeps /HELLO subscribed as:
            //   ^(<>*)<NDNSF><REQUEST><HELLO>(<>)$
            NDN_LOG_WARN("[ServiceProvider] SVS request subscription regex="
                      << regex_str);
            NDN_LOG_DEBUG(regex_str);
            m_svsps->subscribeWithRegex(ndn::Regex(regex_str),
                                        std::bind(&ServiceProvider::OnRequest, this, _1),
                                        true, false);
            // register Service Selection Message
            std::string regex_str2 = "^(<>*)<NDNSF><SELECTION>(<>*)$";
            NDN_LOG_DEBUG(regex_str2);
            m_svsps->subscribeWithRegex(ndn::Regex(regex_str2),
                                        std::bind(&ServiceProvider::onServiceSelectionMessage, this, _1),
                                        true, false);
        }
        std::string collabRegex = "^(<>*)<NDNSF><COLLAB>(<>*)$";
        NDN_LOG_DEBUG(collabRegex);
        m_svsps->subscribeWithRegex(ndn::Regex(collabRegex),
                                    std::bind(&ServiceProvider::onCollaborationDataMessage, this, _1),
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

}
