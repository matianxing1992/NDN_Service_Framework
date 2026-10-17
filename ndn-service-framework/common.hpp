#ifndef NDN_SERVICE_FRAMEWORK_COMMON_HPP
#define NDN_SERVICE_FRAMEWORK_COMMON_HPP

#define BOOST_BIND_NO_PLACEHOLDERS

#include "ProducerSignerPolicy.hpp"

#include <iostream>
#include <functional>
#include <memory>
#include <thread>
#include <stdexcept>
#include <atomic>
#include <condition_variable>
#include <chrono>
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
    namespace test { struct InvocationNamesOracle; }

  /** Configured ndn-cxx validation with pinned, currently valid trust anchors. */
  class TrustSchemaValidator : public ndn::security::Validator
  {
  public:
    using CommandInterestOptions = ndn::ValidatorConfig::CommandInterestOptions;
    using SignedInterestOptions = ndn::ValidatorConfig::SignedInterestOptions;

    explicit
    TrustSchemaValidator(ndn::Face& face,
                         const CommandInterestOptions& commandOptions = {},
                         const SignedInterestOptions& signedOptions = {});

    explicit
    TrustSchemaValidator(std::unique_ptr<ndn::security::CertificateFetcher> fetcher,
                         const CommandInterestOptions& commandOptions = {},
                         const SignedInterestOptions& signedOptions = {});

    void
    load(const std::string& filename);

    void
    load(const std::string& input, const std::string& filename);
  };

  /** Load fixed file/base64 anchors; reject permissive or expired configurations. */
  void
  loadTrustSchema(TrustSchemaValidator& validator, const std::string& filename);

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

    /** Apply validated NDNSF environment overrides to SVS PubSub options. */
    void
    configureSvsPubSubOptionsFromEnvironment(ndn::svs::SVSPubSubOptions& options);

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

    struct CollaborationMemberStatus
    {
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestId;
        std::string selectionDigest;
        std::string role;
        std::string operationId;
        std::string operation;
        std::string state = "QUEUED";
        std::string reasonCode;
        std::string message;
        uint64_t attempt = 1;
        uint64_t epoch = 1;
        uint64_t sequence = 1;
        bool progressKnown = false;
        double progress = 0.0;
        uint64_t createdAtMs = 0;
        uint64_t updatedAtMs = 0;
        uint64_t expiresAtMs = 0;
        std::string detailsSchema;
        ndn::Buffer detailsPayload;
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
        // Canonical SelectionDecisionReceipt wire bytes. The enclosing exact-
        // name status Data is Provider-signed, so the receipt is authenticated
        // without introducing unsolicited push traffic.
        ndn::Buffer decisionReceipt;
        std::vector<CollaborationMemberStatus> memberStatuses;
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

    class MessageValidator : public ndn::svs::BaseValidator,
                             public std::enable_shared_from_this<MessageValidator>
    {
    public:
        MessageValidator(std::string trustSchemaPath,
                         std::optional<ndn::Name> = std::nullopt,
                         ndn::Face* callbackFace = nullptr)
            : m_validator(makeCertificateFetcher(m_face, callbackFace),
                         makeCommandInterestOptions(), makeSignedInterestOptions()),
              m_producerSigners(trustSchemaPath + ".producer-signers"),
              m_callbackIo(callbackFace == nullptr
                             ? nullptr
                             : &callbackFace->getIoContext())
        {
            loadTrustSchema(m_validator, trustSchemaPath);
        }

        size_t
        getFailureCountForTesting() const
        {
            return m_failureCount.load();
        }

        /**
         * @brief Return the configured ndn-cxx validator used by the normal
         *        trust-schema validation path.
         *
         * SegmentFetcher accepts ndn::security::Validator rather than the
         * ndn-svs BaseValidator interface implemented by MessageValidator.
         * Exposing this reference keeps segmented retrieval on the same
         * configured trust schema instead of falling back to ValidatorNull.
         */
        TrustSchemaValidator&
        getConfiguredValidatorForSegmentFetcher() noexcept
        {
            return m_validator;
        }

        /** Namespace permission check after configured cryptographic validation.
         * This does not authorize service execution or key access. */
        bool
        isProducerSignerAuthorized(const ndn::Data& data, const ndn::Name& producer) const
        {
            return m_producerSigners.allowsValidatedSigner(data, producer);
        }

        /**
         * @brief Validate Data through the configured trust schema only.
         *
         * Authority material and ordinary application Data use the same
         * configured trust anchors, certificate validation and name rules.
         * Local PIB certificates are candidates, never trust anchors.
         */
        void
        validateWithConfiguredTrustSchema(
          const ndn::Data& data,
          const ndn::security::DataValidationSuccessCallback& successCb,
          const ndn::security::DataValidationFailureCallback& failureCb)
        {
            if (m_callbackIo != nullptr) {
                if (auto self = weak_from_this().lock()) {
                    auto ownedData = data;
                    auto ownedSuccessCb = successCb;
                    auto ownedFailureCb = failureCb;
                    boost::asio::post(
                      *m_callbackIo,
                      [self, data = std::move(ownedData),
                       success = std::move(ownedSuccessCb),
                       failure = std::move(ownedFailureCb)]() mutable {
                        self->validateConfiguredDataOnFace(
                          data, std::move(success), std::move(failure));
                      });
                    return;
                }
            }
            validateConfiguredDataOnFace(data, successCb, failureCb);
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
            validateData(data, successCb, failureCb);
        }

        void
        validateData(
          const ndn::Data& data,
          const ndn::security::DataValidationSuccessCallback& successCb,
          const ndn::security::DataValidationFailureCallback& failureCb)
        {
            // ndn-svs may invoke its validator from parallel Sync workers,
            // while CertificateFetcherFromNetwork completes on the Face
            // io_context. The configured validator and validation states are owned
            // by that Face thread; starting them concurrently from workers can
            // corrupt the stored completion callback. Marshal the entire
            // validation entry back to the Face event loop and own every input
            // until it runs there.
            if (m_callbackIo != nullptr) {
                if (auto self = weak_from_this().lock()) {
                    auto ownedData = data;
                    auto ownedSuccessCb = successCb;
                    auto ownedFailureCb = failureCb;
                    boost::asio::post(
                      *m_callbackIo,
                      [self, data = std::move(ownedData),
                       success = std::move(ownedSuccessCb),
                       failure = std::move(ownedFailureCb)]() mutable {
                        self->validateDataOnFace(
                          data, std::move(success), std::move(failure));
                      });
                    return;
                }
            }
            validateDataOnFace(data, successCb, failureCb);
        }

        void
        validateDataOnFace(
          const ndn::Data& data,
          ndn::security::DataValidationSuccessCallback successCb,
          ndn::security::DataValidationFailureCallback failureCb)
        {
            // This callback runs on the Face executor after configured
            // cryptographic validation. A versioned invocation assembly
            // keeps one signer identity.
            const auto owner = weak_from_this().lock();
            // Every escaping callback below retains the shared owner.
            const auto admittedSuccess = std::move(successCb);
            successCb = [owner, this, admittedSuccess, failureCb](const ndn::Data& validated) {
                if (!acceptValidatedInvocationSegmentSigner(validated)) {
                    ++m_failureCount;
                    if (failureCb) failureCb(validated, ndn::security::ValidationError(
                        ndn::security::ValidationError::POLICY_ERROR,
                        "invocation segment signer conflict or binding capacity exhausted"));
                    return;
                }
                if (admittedSuccess) admittedSuccess(validated);
            };
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
            // Validation may complete only after one or more network
            // certificate fetches.  Own the callbacks for that asynchronous
            // lifetime; retaining references to validateData() parameters
            // makes them dangle as soon as this function returns.
            if (!owner) {
                ++m_failureCount;
                if (failureCb) failureCb(data, ndn::security::ValidationError(
                    ndn::security::ValidationError::IMPLEMENTATION_ERROR,
                    "network Data validation requires a shared MessageValidator owner"));
                return;
            }
            auto ownedSuccessCb = successCb;
            auto ownedFailureCb = failureCb;
            m_validator.validate(
                data,
                [owner, success = std::move(ownedSuccessCb)](const ndn::Data& validatedData)
                {
                    NDN_LOG_DEBUG(
                      "MessageValidator Data validated through configured validator "
                      "name=" << validatedData.getName());
                    if (success) {
                        success(validatedData);
                    }
                },
                [owner, failure = std::move(ownedFailureCb)](
                    const ndn::Data& failedData,
                    const ndn::security::ValidationError& error)
                {
                    NDN_LOG_ERROR("MessageValidator Data validation failed name="
                                  << failedData.getName()
                                  << " reason=" << error);
                    ++owner->m_failureCount;
                    if (failure) {
                        failure(failedData, error);
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
                 const ndn::security::InterestValidationFailureCallback&) override
        {
            // SVS handles these transport notifications separately from
            // application admission. Piggybacked publications and retrieved
            // application Data must both pass the encapsulated Data validator
            // before delivery; accepting this Interest does not admit them.
            successCb(interest);
        }

    private:
        friend class EncapsulatedMessageValidator;

        void validateEncapsulatedData(
          const ndn::Data& data,
          const ndn::security::DataValidationSuccessCallback& success,
          const ndn::security::DataValidationFailureCallback& failure)
        {
            // SVS may immediately process mappings after this callback. Keep
            // its small cached packet ready without changing general callback
            // ordering or running validation concurrently from workers.
            if (m_callbackIo && m_callbackIo->get_executor().running_in_this_thread() &&
                data.wireEncode().size() <= 4096) {
                validateDataOnFace(data, success, failure);
                return;
            }
            validateData(data, success, failure);
        }

    public:
        std::shared_ptr<ndn::svs::BaseValidator> makeEncapsulatedValidator();

    private:
        void
        validateConfiguredDataOnFace(
          const ndn::Data& data,
          ndn::security::DataValidationSuccessCallback successCb,
          ndn::security::DataValidationFailureCallback failureCb)
        {
            const auto owner = weak_from_this().lock();
            if (!owner) {
                ++m_failureCount;
                if (failureCb) failureCb(data, ndn::security::ValidationError(
                    ndn::security::ValidationError::IMPLEMENTATION_ERROR,
                    "configured Data validation requires a shared MessageValidator owner"));
                return;
            }
            auto ownedSuccessCb = std::move(successCb);
            auto ownedFailureCb = std::move(failureCb);
            m_validator.validate(
              data,
              [owner, success = std::move(ownedSuccessCb)](
                const ndn::Data& validatedData) {
                  if (success) {
                      success(validatedData);
                  }
              },
              [owner, failure = std::move(ownedFailureCb)](
                const ndn::Data& failedData,
                const ndn::security::ValidationError& error) {
                  ++owner->m_failureCount;
                  if (failure) {
                      failure(failedData, error);
                  }
              });
        }

        static std::unique_ptr<ndn::security::CertificateFetcher>
        makeCertificateFetcher(ndn::Face& fallbackFace, ndn::Face* callbackFace);

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
        friend struct test::InvocationNamesOracle;
        bool acceptValidatedInvocationSegmentSigner(const ndn::Data& data)
        {
            const auto& name = data.getName();
            if (data.getContentType() == ndn::tlv::Data || !data.getFinalBlock() ||
                !data.getFinalBlock()->isSegment() || name.size() < 2 ||
                !name[-1].isSegment() || !name[-2].isVersion()) return true;
            const auto base = name.getPrefix(-1);
            const auto now = std::chrono::steady_clock::now();
            for (auto it = m_invocationSegmentSigners.begin(); it != m_invocationSegmentSigners.end();) {
                if (now - it->second.second >= std::chrono::minutes(5)) it = m_invocationSegmentSigners.erase(it);
                else ++it;
            }
            ndn::Name signer;
            try {
                const auto& key = data.getSignatureInfo().getKeyLocator().getName();
                signer = ndn::security::isValidKeyName(key) ?
                    ndn::security::extractIdentityFromKeyName(key) :
                    ndn::security::extractIdentityFromCertName(key);
            }
            catch (const std::exception&) { return false; }
            const auto existing = m_invocationSegmentSigners.find(base);
            if (existing != m_invocationSegmentSigners.end()) {
                if (existing->second.first != signer) return false;
                existing->second.second = now;
                return true;
            }
            try {
                if (!m_producerSigners.findValidatedInvocationProducer(data)) return true;
            }
            catch (const std::exception&) { return false; }
            if (m_invocationSegmentSigners.size() >= 4096) return false;
            m_invocationSegmentSigners.emplace(base, std::make_pair(signer, now));
            return true;
        }

    private:
        NDN_LOG_MEMBER_DECL();
        ndn::Face m_face;
        TrustSchemaValidator m_validator;
        ProducerSignerPolicy m_producerSigners;
        // Face-thread owned; idle expiry bounds retention, admission never
        // evicts active original-packet signer evidence.
        std::map<ndn::Name, std::pair<ndn::Name, std::chrono::steady_clock::time_point>>
            m_invocationSegmentSigners;
        boost::asio::io_context* m_callbackIo = nullptr;
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
