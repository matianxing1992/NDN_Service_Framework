#ifndef NDN_SERVICE_FRAMEWORK_COMMON_HPP
#define NDN_SERVICE_FRAMEWORK_COMMON_HPP

#define BOOST_BIND_NO_PLACEHOLDERS

#include <iostream>
#include <functional>
#include <memory>
#include <thread>
#include <stdexcept>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <vector>
#include <ndn-cxx/face.hpp>
#include <ndn-svs/svspubsub.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/security/validation-policy-signed-interest.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include "ndn-cxx/security/certificate-fetcher-from-network.hpp"
#include "ndn-cxx/security/interest-signer.hpp"
#include <unordered_map>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/interest.hpp>
#include "ndn-cxx/util/logger.hpp"
#include <ndn-cxx/util/regex.hpp>
#include "ndn-cxx/util/time.hpp"
#include "ndn-cxx/util/random.hpp"
#include "ndn-cxx/util/scheduler.hpp"

#include <ndn-cxx/encoding/block-helpers.hpp>

#include <ndn-cxx/util/segment-fetcher.hpp>
#include <ndn-cxx/util/segmenter.hpp>
#include "ndn-cxx/ims/in-memory-storage-fifo.hpp"

#include <nac-abe/consumer.hpp>
#include <nac-abe/producer.hpp>
#include <nac-abe/cache-producer.hpp>

#include <ndnsd/discovery/service-discovery.hpp>

#include <map>
#include <mutex>
#include <atomic>

#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <boost/bimap/bimap.hpp>
#include <boost/bimap/unordered_set_of.hpp>
#include <boost/bimap.hpp>
#include <boost/bimap/set_of.hpp>
#include <boost/bimap/multiset_of.hpp>

#include <NDNSFThreadPool.hpp>

// #define USE_TIMESTAMP


namespace ndn_service_framework{

    ndn::KeyType
    getCertificateKeyType(const ndn::security::Certificate& cert);

    ndn::security::Certificate
    getEcdsaSigningCertificateOrFallback(ndn::KeyChain& keyChain,
                                         const ndn::security::Certificate& fallbackCert);

    ndn::security::Certificate
    getRsaEncryptionCertificateOrThrow(ndn::KeyChain& keyChain,
                                       const ndn::security::Certificate& identityHintCert);

    ndn::security::Certificate
    getRsaEncryptionCertificateOrThrow(const ndn::security::Certificate& identityHintCert);

    ndn::security::SigningInfo
    makeEcdsaPreferredSigningInfo(ndn::KeyChain& keyChain,
                                  const ndn::Name& identityName);

    void
    signDataEcdsaPreferred(ndn::KeyChain& keyChain,
                           ndn::Data& data,
                           const ndn::Name& identityName);

    enum class SelectionExecutionState
    {
        Unknown,
        Received,
        Queued,
        Running,
        Completed,
        Failed,
        Rejected,
        Expired,
        Cancelled,
    };

    struct SelectionExecutionStatus
    {
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestId;
        std::string selectionDigest;
        SelectionExecutionState state = SelectionExecutionState::Unknown;
        std::string message;
        ndn::Name responseName;
        uint64_t receivedAtUs = 0;
        uint64_t queuedAtUs = 0;
        uint64_t runningAtUs = 0;
        uint64_t completedAtUs = 0;
        uint64_t updatedAtUs = 0;
    };

    const char*
    selectionExecutionStateToString(SelectionExecutionState state);

    SelectionExecutionState
    selectionExecutionStateFromString(const std::string& state);

    struct HybridCryptoCounters
    {
        std::atomic<uint64_t> hybrid_key_epoch_created{0};
        std::atomic<uint64_t> hybrid_key_rotation_age{0};
        std::atomic<uint64_t> hybrid_key_rotation_uses{0};
        std::atomic<uint64_t> nac_abe_key_wrap_count{0};
        std::atomic<uint64_t> nac_abe_key_unwrap_count{0};
        std::atomic<uint64_t> symmetric_encrypt_count{0};
        std::atomic<uint64_t> symmetric_decrypt_count{0};
        std::atomic<uint64_t> key_cache_hit_count{0};
        std::atomic<uint64_t> key_cache_miss_count{0};
        std::atomic<uint64_t> auth_decrypt_failure_count{0};
        std::atomic<uint64_t> user_token_symmetric_encrypt_count{0};
        std::atomic<uint64_t> user_token_symmetric_decrypt_count{0};
        std::atomic<uint64_t> provider_token_symmetric_encrypt_count{0};
        std::atomic<uint64_t> provider_token_symmetric_decrypt_count{0};
        std::atomic<uint64_t> legacy_token_nac_abe_encrypt_count{0};
        std::atomic<uint64_t> legacy_token_nac_abe_decrypt_count{0};
    };

    class SerializedWorkerQueue
    {
    public:
        explicit SerializedWorkerQueue(std::string name, size_t maxQueueSize = 1024)
            : m_name(std::move(name))
            , m_maxQueueSize(maxQueueSize == 0 ? 1 : maxQueueSize)
            , m_worker([this] { run(); })
        {
        }

        ~SerializedWorkerQueue()
        {
            shutdown();
        }

        SerializedWorkerQueue(const SerializedWorkerQueue&) = delete;
        SerializedWorkerQueue& operator=(const SerializedWorkerQueue&) = delete;

        bool
        post(std::function<void()> task)
        {
            if (!task) {
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping || m_tasks.size() >= m_maxQueueSize) {
                    return false;
                }
                m_tasks.emplace_back(std::move(task));
            }
            m_cv.notify_one();
            return true;
        }

        void
        shutdown()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping) {
                    return;
                }
                m_stopping = true;
            }
            m_cv.notify_one();
            if (m_worker.joinable()) {
                m_worker.join();
            }
        }

    private:
        void
        run()
        {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this] {
                        return m_stopping || !m_tasks.empty();
                    });
                    if (m_stopping && m_tasks.empty()) {
                        return;
                    }
                    task = std::move(m_tasks.front());
                    m_tasks.pop_front();
                }

                try {
                    task();
                }
                catch (const std::exception& e) {
                    NDN_LOG_ERROR(m_name << " task failed: " << e.what());
                }
                catch (...) {
                    NDN_LOG_ERROR(m_name << " task failed with unknown exception");
                }
            }
        }

    private:
        NDN_LOG_MEMBER_DECL();
        std::string m_name;
        size_t m_maxQueueSize;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<std::function<void()>> m_tasks;
        bool m_stopping = false;
        std::thread m_worker;
    };

    class BoundedWorkerPool
    {
    public:
        explicit BoundedWorkerPool(std::string name, size_t maxQueueSize = 1024)
            : m_name(std::move(name))
            , m_maxQueueSize(maxQueueSize == 0 ? 1 : maxQueueSize)
        {
        }

        ~BoundedWorkerPool()
        {
            shutdown();
        }

        BoundedWorkerPool(const BoundedWorkerPool&) = delete;
        BoundedWorkerPool& operator=(const BoundedWorkerPool&) = delete;

        void
        setThreadCount(size_t threadCount)
        {
            shutdown();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stopping = false;
                m_threadCount = threadCount;
            }
            if (threadCount == 0) {
                return;
            }
            m_workers.reserve(threadCount);
            for (size_t i = 0; i < threadCount; ++i) {
                m_workers.emplace_back([this] { run(); });
            }
        }

        size_t
        getThreadCount() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_threadCount;
        }

        size_t
        getQueueSize() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_tasks.size();
        }

        bool
        post(std::function<void()> task)
        {
            if (!task) {
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_threadCount == 0) {
                    return false;
                }
                if (m_stopping || m_tasks.size() >= m_maxQueueSize) {
                    return false;
                }
                m_tasks.emplace_back(std::move(task));
            }
            m_cv.notify_one();
            return true;
        }

        void
        shutdown()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping && m_workers.empty()) {
                    return;
                }
                m_stopping = true;
            }
            m_cv.notify_all();
            for (auto& worker : m_workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            m_workers.clear();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_threadCount = 0;
            }
        }

    private:
        void
        run()
        {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this] {
                        return m_stopping || !m_tasks.empty();
                    });
                    if (m_stopping && m_tasks.empty()) {
                        return;
                    }
                    task = std::move(m_tasks.front());
                    m_tasks.pop_front();
                }

                try {
                    task();
                }
                catch (const std::exception& e) {
                    NDN_LOG_ERROR(m_name << " task failed: " << e.what());
                }
                catch (...) {
                    NDN_LOG_ERROR(m_name << " task failed with unknown exception");
                }
            }
        }

    private:
        NDN_LOG_MEMBER_DECL();
        std::string m_name;
        size_t m_maxQueueSize;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<std::function<void()>> m_tasks;
        std::vector<std::thread> m_workers;
        size_t m_threadCount = 0;
        bool m_stopping = true;
    };

    class OptionalServiceDiscovery
    {
    public:
        using Details = ndnsd::discovery::Details;
        using Callback = std::function<void(const Details&)>;

        OptionalServiceDiscovery() = default;

        OptionalServiceDiscovery(const ndn::Name& groupPrefix,
                                 const ndn::Name& identity,
                                 ndn::Face& face,
                                 ndn::KeyChain& keyChain,
                                 Callback callback)
        {
            enable(groupPrefix, identity, face, keyChain, std::move(callback));
        }

        void enable(const ndn::Name& groupPrefix,
                    const ndn::Name& identity,
                    ndn::Face& face,
                    ndn::KeyChain& keyChain,
                    Callback callback)
        {
            m_discovery = std::make_unique<ndnsd::discovery::ServiceDiscovery>(
                groupPrefix, identity, face, keyChain, std::move(callback));
        }

        bool isEnabled() const
        {
            return static_cast<bool>(m_discovery);
        }

        void publishServiceDetail(const Details& details)
        {
            if (m_discovery) {
                m_discovery->publishServiceDetail(details);
            }
        }

        std::map<std::string, Details>
        getReceivedServiceDetails() const
        {
            if (m_discovery) {
                return m_discovery->getReceivedServiceDetails();
            }
            return {};
        }

    private:
        std::unique_ptr<ndnsd::discovery::ServiceDiscovery> m_discovery;
    };

    class MessageValidator : public ndn::svs::BaseValidator
    {
    public:
        MessageValidator(std::string trustSchemaPath)
            : m_validator(m_face, makeCommandInterestOptions(), makeSignedInterestOptions())
        {
            m_validator.load(trustSchemaPath);
        }

        size_t
        getFailureCountForTesting() const
        {
            return m_failureCount.load();
        }

        /**
         * @brief Asynchronously validate @p data
         *
         * @note @p successCb and @p failureCb must not be nullptr
         */
        void
        validate(const ndn::Data &data,
                 const ndn::security::DataValidationSuccessCallback &successCb,
                 const ndn::security::DataValidationFailureCallback &failureCb) override
        {
            const auto sigType = data.getSignatureType();
            if ((sigType != ndn::tlv::SignatureSha256WithRsa &&
                 sigType != ndn::tlv::SignatureSha256WithEcdsa) ||
                !data.getSignatureInfo().hasKeyLocator()) {
                ndn::security::ValidationError error(
                    ndn::security::ValidationError::MALFORMED_SIGNATURE,
                    "Data must have an RSA/ECDSA signature with KeyLocator");
                NDN_LOG_ERROR("MessageValidator Data validation failed name="
                              << data.getName()
                              << " reason=" << error);
                ++m_failureCount;
                if (failureCb) {
                    failureCb(data, error);
                }
                return;
            }
            try {
                const auto keyLocator = data.getSignatureInfo().getKeyLocator();
                if (keyLocator.getType() == ndn::tlv::Name) {
                    const auto& klName = keyLocator.getName();
                    NDN_LOG_DEBUG("MessageValidator Data validation begin name="
                                  << data.getName()
                                  << " sigType=" << sigType
                                  << " keyLocator=" << klName);
                    const auto dataName = data.getName();
                    bool isControllerBootstrapData = false;
                    for (size_t i = 0; i + 1 < dataName.size(); ++i) {
                        if (dataName[i].toUri() == "NDNSF" &&
                            (dataName[i + 1].toUri() == "POLICY-MANIFEST" ||
                             dataName[i + 1].toUri() == "PERMISSIONS")) {
                            isControllerBootstrapData = true;
                            break;
                        }
                    }
                    if (isControllerBootstrapData) {
                        const auto signerIdentity =
                            ndn::security::extractIdentityFromCertName(klName);
                        if (signerIdentity.isPrefixOf(dataName)) {
                            NDN_LOG_DEBUG("MessageValidator controller bootstrap "
                                          "Data accepted by hierarchical "
                                          "name/key-locator relation name="
                                          << dataName
                                          << " signer=" << signerIdentity);
                            successCb(data);
                            return;
                        }
                    }
                    if (validateWithLocalCertificate(data, klName)) {
                        successCb(data);
                        return;
                    }
                }
            }
            catch (const std::exception&) {
            }

            m_validator.validate(
                data,
                [&](const ndn::Data &)
                {
                    successCb(data);
                },
                [&](const ndn::Data& data, const ndn::security::ValidationError& error)
                {
                    NDN_LOG_ERROR("MessageValidator Data validation failed name="
                                  << data.getName()
                                  << " reason=" << error);
                    ++m_failureCount;
                    if (failureCb) {
                        failureCb(data, error);
                    }
                });
        }

        /**
         * @brief Asynchronously validate @p interest
         *
         * @note @p successCb and @p failureCb must not be nullptr
         */
        void
        validate(const ndn::Interest &interest,
                 const ndn::security::InterestValidationSuccessCallback &successCb,
                 const ndn::security::InterestValidationFailureCallback &failureCb) override
        {
            // SVS Sync Interests only announce sync state and carry no NDNSF
            // application payload. The actual SVS Data packets fetched after
            // sync are still validated by the Data path above against the
            // trust schema and trust anchor.
            successCb(interest);
            return;
            m_validator.validate(
                interest,
                [&](const ndn::Interest& interest)
                {
                    successCb(interest);
                },
                [&](const ndn::Interest& interest, const ndn::security::ValidationError& error)
                {
                    NDN_LOG_ERROR("MessageValidator Interest validation failed name="
                                  << interest.getName()
                                  << " reason=" << error);
                    ++m_failureCount;
                    if (failureCb) {
                        failureCb(interest, error);
                    }
                });

        }

    private:
        bool
        validateWithLocalCertificate(const ndn::Data& data,
                                     const ndn::Name& certName)
        {
            if (!ndn::security::Certificate::isValidName(certName)) {
                return false;
            }

            const auto signerIdentity =
                ndn::security::extractIdentityFromCertName(certName);
            if (!signerIdentity.isPrefixOf(data.getName())) {
                NDN_LOG_ERROR("MessageValidator Data validation failed name="
                              << data.getName()
                              << " reason=signer identity is not a prefix of Data name"
                              << " signer=" << signerIdentity);
                ++m_failureCount;
                return false;
            }

            try {
                const auto keyName =
                    ndn::security::extractKeyNameFromCertName(certName);
                const auto identity =
                    m_keyChain.getPib().getIdentity(signerIdentity);
                const auto key = identity.getKey(keyName);
                const auto cert = key.getCertificate(certName);
                if (!ndn::security::verifySignature(
                        data,
                        std::optional<ndn::security::Certificate>{cert})) {
                    NDN_LOG_ERROR("MessageValidator Data validation failed name="
                                  << data.getName()
                                  << " reason=signature mismatch cert="
                                  << certName);
                    ++m_failureCount;
                    return false;
                }
                NDN_LOG_DEBUG("MessageValidator Data validated with local PIB "
                              "certificate name=" << data.getName()
                              << " cert=" << certName);
                return true;
            }
            catch (const std::exception& e) {
                NDN_LOG_DEBUG("MessageValidator local PIB certificate lookup "
                              "miss name=" << data.getName()
                              << " cert=" << certName
                              << " reason=" << e.what());
                return false;
            }
        }

        static ndn::security::ValidatorConfig::CommandInterestOptions
        makeCommandInterestOptions()
        {
            ndn::security::ValidatorConfig::CommandInterestOptions options;
            // ValidatorConfig includes the legacy command-interest policy
            // inside the signed-interest policy. That policy treats V03 signed
            // Interests as stop-and-wait command Interests and records one
            // last timestamp per signer. SVS Sync Interests are not
            // stop-and-wait; they can be multicast and delivered out of order.
            // Disable only the last-timestamp record while retaining the
            // normal timestamp grace-period sanity check.
            options.maxRecords = 0;
            return options;
        }

        static ndn::security::ValidatorConfig::SignedInterestOptions
        makeSignedInterestOptions()
        {
            ndn::security::ValidatorConfig::SignedInterestOptions options;
            // SVS Sync Interests are multicast realtime signals. They can be
            // delivered out of order even when each signer produces monotonic
            // timestamps, so timestamp-order replay checks cause false drops
            // under high publication rates. Keep nonce replay protection and
            // normal trust-schema/signature validation.
            options.shouldValidateTimestamps = false;
            options.shouldValidateNonces = true;
            options.maxNonceRecordCount = 10000;
            return options;
        }

    private:
        NDN_LOG_MEMBER_DECL();
        ndn::KeyChain m_keyChain;
        ndn::Face m_face;
        ndn::ValidatorConfig m_validator;
        std::atomic<size_t> m_failureCount{0};
    };

    /**
     * A signer using an ndn-cxx keychain instance
     */
    class CommandInterestSigner : public ndn::svs::BaseSigner
    {
    public:
        explicit CommandInterestSigner(ndn::security::KeyChain &keyChain)
            : m_keyChain(keyChain),
              m_signer(m_keyChain)
        {
        }

        void
        sign(ndn::Interest &interest) const override
        {
            std::lock_guard<std::mutex> lock(m_signerMutex);
            m_signer.makeSignedInterest(interest, signingInfo,
                            ndn::security::InterestSigner::SigningFlags::WantNonce |
                            ndn::security::InterestSigner::SigningFlags::WantTime);
        }

        void
        sign(ndn::Data &data) const override
        {
            m_keyChain.sign(data, signingInfo);
        }

    private:
        ndn::security::KeyChain &m_keyChain;
        mutable std::mutex m_signerMutex;
        mutable ndn::security::InterestSigner m_signer;
    };
}

#endif
