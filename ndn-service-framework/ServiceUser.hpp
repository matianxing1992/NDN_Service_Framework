#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP

#include "common.hpp"
#include "utils.hpp"

#include "ServiceAuthorizationTable.hpp"
#include "NDNSFMessages.hpp"
#include "InvocationStream.hpp"
#include "ConfigManager.hpp"
#include "HybridMessageCrypto.hpp"
#include "RequestConfidentiality.hpp"
#include "NetworkTelemetry.hpp"
#include "NegativeAckReason.hpp"
#include "TimelineTrace.hpp"
#include "Stream.hpp"
#include "StreamFacade.hpp"
#include "RevocationState.hpp"
#include "PolicyRefreshCoordinator.hpp"
#include "RuntimeStatusStore.hpp"
#include "EncryptedLargeDataRangeStore.hpp"

#include <functional>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>


namespace ndn_service_framework{

    struct AckInfo{
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestID;
    };

    /**
     * Non-secret authentication evidence for one validated ACK Data packet.
     *
     * The outer ACK Data is validated by the configured Trust Schema before it
     * reaches ServiceUser.  Keep the resulting signer provenance alongside the
     * decrypted ACK so higher layers can bind an embedded capability offer to
     * the authenticated Provider without reconstructing identity from the ACK
     * name or a caller-provided key map.  Empty values are retained for direct
     * unit-test helpers that do not have a validated Data packet.
     */
    struct AckAuthenticationEvidence
    {
        std::string signerIdentity;
        std::string signerKeyLocator;
        std::string wireDigest;
        // True only for an ACK delivered through the validated ServiceUser
        // subscription path. Direct/unit fixtures leave this false.
        bool trustSchemaValidated = false;
    };

    struct AckSelectionCandidate
    {
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestId;
        ndn_service_framework::RequestAckMessage ack;
        std::optional<ndn_service_framework::NetworkTelemetrySnapshot> telemetry;
        AckAuthenticationEvidence authenticationEvidence{};
    };

    using ProviderId = ndn::Name;
    using ServiceName = ndn::Name;
    using RequestId = ndn::Name;
    using RequestPayload = ndn::Buffer;
    using ResponsePayload = ndn::Buffer;
    using AckCandidate = AckSelectionCandidate;
    using CollaborationRole = std::string;
    using KeyScope = std::string;

    class AckSelectionPolicy
    {
    public:
        virtual std::vector<ProviderId>
        select(const std::vector<AckCandidate>& candidates) const = 0;

        virtual size_t
        requestStrategy() const
        {
            return ndn_service_framework::tlv::FirstResponding;
        }

        virtual ~AckSelectionPolicy() = default;
    };

    namespace strategy
    {
        extern const std::shared_ptr<const AckSelectionPolicy> FirstResponding;
        extern const std::shared_ptr<const AckSelectionPolicy> RandomSelection;
        extern const std::shared_ptr<const AckSelectionPolicy> AllSelected;
    }

    struct CollaborationRoleSpec
    {
        CollaborationRole role;
        ServiceName service;
        ndn::Name requiredArtifact;
        bool allowDynamicProvisioning = false;
        int provisioningTimeoutMs = 30000;
        ndn::Buffer appRequirement;
        // Optional exact participant payload chosen after ACK closure.  This is
        // generic opaque application data; NDNSF does not parse its schema.
        ndn::Buffer assignmentPayload;
        size_t minProviders = 1;
        size_t maxProviders = 1;
        // A streamed collaboration has exactly one user-facing terminal
        // owner.  Other selected roles may publish internal data and must
        // complete without claiming the shared End/Response lifecycle.
        bool terminalResponseOwner = false;
    };

    struct CollaborationKeyScope
    {
        KeyScope name;
        std::vector<CollaborationRole> roles;
    };

    struct CollaborationDependency
    {
        std::vector<CollaborationRole> producers;
        std::vector<CollaborationRole> consumers;
        KeyScope keyScope;
        ndn::Name topicPrefix;
        bool required = true;
    };

    struct SelectedParticipant
    {
        CollaborationRole role;
        ServiceName service;
        ProviderId provider;
        ndn::Name assignedArtifact;
        bool requiresProvisioning = false;
        int provisioningTimeoutMs = 0;
        ndn::Buffer assignmentPayload;
        AckCandidate ack;
        // Assignment-bound canonical root Data name. Keep this at the end so
        // existing aggregate initializers remain source-compatible.
        ndn::Name artifactDataName;
    };

    class ParticipantSelectionPolicy
    {
    public:
        virtual std::vector<SelectedParticipant>
        select(const std::vector<AckCandidate>& candidates,
               const std::vector<CollaborationRoleSpec>& roles) const = 0;

        virtual ~ParticipantSelectionPolicy() = default;
    };

    struct CollaborationPlan
    {
        int ackCollectionTimeMs = 200;
        int timeoutMs = 5000;
        std::vector<CollaborationRoleSpec> roles;
        std::vector<CollaborationKeyScope> keyScopes;
        std::vector<CollaborationDependency> dependencies;
        // Generic metadata shared by selected participants and transported
        // separately from each exact opaque assignment.
        ndn::Buffer sharedAssignmentMetadata;
        std::shared_ptr<const ParticipantSelectionPolicy> participantSelector;
    };

    /** Provider-signed and request-scope-decrypted collaboration record
     * observed by the requesting User. The SVS validator authenticates the
     * outer Data before this value is admitted. */
    struct VerifiedCollaborationData
    {
        ndn::Name dataName;
        ndn::Name requestId;
        KeyScope keyScope;
        ndn::Name topic;
        ndn::Name producer;
        CollaborationRole producerRole;
        uint64_t sequence = 0;
        ndn::Buffer payload;
        std::string signerCertificate;
        std::string wireDigest;
    };

    struct CollaborationAckClosure
    {
        RequestId requestId;
        std::vector<AckCandidate> candidates;
        std::string digest;
        uint64_t closedAtUs = 0;
        uint64_t requestDeadlineUs = 0;
    };

    using CollaborationAckClosedHandler =
        std::function<void(const CollaborationAckClosure&)>;

    // Optional, application-owned ACK coverage hook.  The hook is evaluated
    // only after a candidate has passed the normal ACK authentication and
    // replay checks.  Returning true closes the ACK window early; it does not
    // select providers, create a plan, or bypass ACK_CLOSED immutability.
    using CollaborationAckCoverageHandler =
        std::function<bool(const std::vector<AckCandidate>&)>;

    struct PreparedServiceRequest
    {
        ndn::Name serviceName;
        ndn::Name requestId;
        mutable bool used = false;
    };

    /** Core-owned policy for immutable large-data publication. */
    struct LargeDataPublishOptions
    {
        EncryptedLargeDataRetention retention = EncryptedLargeDataRetention::Transient;
        // Stable, non-secret identity supplied by the native preparation owner.
        std::string publicationIdentity;
    };

    struct LargeDataPublishResult
    {
        bool success = false;
        ndn::Name encryptedDataName;
        std::string objectId;
        size_t plaintextSize = 0;
        std::string contentDigest;
        std::string manifestDigest;
        std::string authorizationScope;
        std::string protectionEpoch;
        std::string publicationIdentity;
        std::string keyReferenceId;
        std::string keyReferenceVersion;
        std::string ciphertextManifestDigest;
        std::string servingLocator;
        bool encrypted = true;
        std::string errorMessage;
        // Local names and scoped wrapped-key identity retained for an aborting
        // multi-object publication. These are never sent on the wire.
        std::vector<std::string> rollbackDataNames;
        std::string rollbackKeyId;
        std::string rollbackServiceName;
        // Optional owner-bound serving pin. Copies share one pin; no key or
        // payload bytes are exposed. Timed publications leave this empty.
        std::shared_ptr<void> servingLease;
        bool fileBacked = false;
    };

    /** Bounded file-backed serving counters exposed only for native selectors.
     * The counters describe the serving window, not a qualification result. */
    struct LargeDataServingMetrics
    {
        size_t publicationCount = 0;
        uint64_t segmentReadCount = 0;
        uint64_t retransmissionHitCount = 0;
        size_t peakWindowSegments = 0;
    };

    struct LargeDataReferenceRequestResult
    {
        bool success = false;
        bool usedLargeDataReference = false;
        ndn_service_framework::RequestMessage requestMessage;
        LargeDataPublishResult largeData;
        std::string errorMessage;
    };

    using Timeout_Callback = std::function<void(const std::string & reason)>;

    class ServiceUser
    {
        public:
            /** Queue work on this user's existing Face I/O context.
             * Never invokes inline or starts an event loop. The application
             * keeps the Face alive and running; the task must retain its
             * own owners and contain its asynchronous exceptions. */
            void postToIo(std::function<void()> task) const;

            /** True while this thread executes the existing Face context.
             * Blocking application waits must be rejected on that thread. */
            bool isOnIoThread() const;

            using AcksHandler =
                std::function<std::vector<ndn_service_framework::RequestAckMessage>(
                    const std::vector<ndn_service_framework::RequestAckMessage>&)>;

            using AckCandidatesHandler =
                std::function<std::vector<ndn_service_framework::AckSelectionCandidate>(
                    const std::vector<ndn_service_framework::AckSelectionCandidate>&)>;

            enum class AckSelectionStrategy
            {
                FirstRespondingSelection,
                RandomSelection,
                AllSelected,
                CustomSelectionStrategy,
            };

            using ResponseHandler =
                std::function<void(const ndn_service_framework::ResponseMessage&)>;

            using TimeoutHandler =
                std::function<void(const ndn::Name&)>;

            struct SelectionStatusOptions
            {
                explicit SelectionStatusOptions(bool enabled = true,
                                                int queryIntervalMs = 1000,
                                                int queryTimeoutMs = 500)
                  : enabled(enabled),
                    queryIntervalMs(queryIntervalMs),
                    queryTimeoutMs(queryTimeoutMs)
                {
                }

                bool enabled;
                int queryIntervalMs;
                int queryTimeoutMs;
            };

            using SelectionStatusHandler =
                std::function<void(const SelectionExecutionStatus&)>;

            using SelectionStatusTimeoutHandler =
                std::function<void(const ndn::Name& requestId,
                                   const std::vector<SelectionExecutionStatus>& statuses)>;

            enum class RequestLifecycleState
            {
                QUEUED_LOCAL,
                ADMISSION_DELAYED,
                ADMITTED,
                REQUEST_PUBLISHED,
                ACK_MATCHED,
                PROVIDER_SELECTED,
                SELECTION_PUBLISHED,
                RESPONSE_OBSERVED,
                RESPONSE_DECRYPTED,
                CALLBACK_FIRED,
                COMPLETED,
                ADMISSION_REJECTED,
                TIMED_OUT,
                CANCELLED_OR_DROPPED,
            };

            struct RequestLifecycleStatus
            {
                std::string applicationTaskId;
                ndn::Name requestId;
                ndn::Name serviceName;
                RequestLifecycleState state = RequestLifecycleState::QUEUED_LOCAL;
                ndn::Name selectedProviderName;
                uint64_t enqueueTimestampUs = 0;
                uint64_t admissionTimestampUs = 0;
                uint64_t publishTimestampUs = 0;
                uint64_t ackMatchedTimestampUs = 0;
                uint64_t providerSelectionTimestampUs = 0;
                uint64_t selectionPublishTimestampUs = 0;
                uint64_t responseObservedTimestampUs = 0;
                uint64_t responseDecryptedTimestampUs = 0;
                uint64_t callbackTimestampUs = 0;
                uint64_t completionTimestampUs = 0;
                uint64_t timeoutTimestampUs = 0;
                double queuedDurationMs = 0.0;
                double inflightDurationMs = 0.0;
                double endToEndLatencyMs = 0.0;
                bool delayedByAdmissionControl = false;
                std::string finalCleanupReason;
            };

            using RequestLifecycleCallback =
                std::function<void(const RequestLifecycleStatus&)>;

            struct AdmissionControlStatus
            {
                ndn::Name requestId;
                size_t queueDepth = 0;
                size_t softQueueLimit = 0;
                size_t hardQueueLimit = 0;
                size_t remainingHardSlots = 0;
                std::string reason;
            };

            struct ResponseRetryOptions
            {
                bool enabled = false;
                int attemptTimeoutMs = 1000;
                /** Maximum selections, including the initial Provider. */
                size_t maxAttempts = 4;
            };

            using AdmissionControlWarningHandler =
                std::function<void(const AdmissionControlStatus&)>;

            using AdmissionControlRejectHandler =
                std::function<void(const AdmissionControlStatus&)>;

            using RequestPublisher =
                std::function<void(const ndn::Name& requestId,
                                   const ndn::Name& requestName,
                                   const std::vector<ndn::Name>& providers,
                                   const ndn::Name& serviceName,
                                   const ndn_service_framework::RequestMessage& requestMessage,
                                   size_t strategy)>;

            // Test-only publication boundary for LocalMockTag integration
            // fixtures. Production instances publish through SVSPubSub.
            using LocalPublicationHandler =
                std::function<void(const ndn::Name& messageName,
                                   const ndn::Buffer& wire)>;

            struct LocalMockTag
            {
            };

            ServiceUser(ndn::Face& face,ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
            ServiceUser(ndn::Face& face,
                        ndn::Name group_prefix,
                        ndn::security::Certificate encryptionCert,
                        ndn::security::Certificate signingCert,
                        ndn::security::Certificate attrAuthorityCertificate,
                        std::string trustSchemaPath);
            /** Production owner injection: all SVS/NAC signing uses this
             * KeyChain, which must outlive the ServiceUser. */
            ServiceUser(ndn::Face& face,
                        ndn::Name group_prefix,
                        ndn::security::Certificate encryptionCert,
                        ndn::security::Certificate signingCert,
                        ndn::security::Certificate attrAuthorityCertificate,
                        std::string trustSchemaPath,
                        ndn::KeyChain& signingKeyChain);
            struct ExternalKeyChainTag
            {
            };
            ServiceUser(ExternalKeyChainTag,
                        ndn::Face& face,
                        ndn::Name group_prefix,
                        ndn::security::Certificate encryptionCert,
                        ndn::security::Certificate signingCert,
                        ndn::security::Certificate attrAuthorityCertificate,
                        std::string trustSchemaPath,
                        ndn::KeyChain* signingKeyChain);
            ServiceUser(LocalMockTag,
                        ndn::Face& face,
                        ndn::Name group_prefix,
                        ndn::security::Certificate identityCert,
                        ndn::security::Certificate attrAuthorityCertificate,
                        std::string trustSchemaPath);
            ServiceUser(LocalMockTag,
                        ndn::Face& face,
                        ndn::Name group_prefix,
                        ndn::security::Certificate encryptionCert,
                        ndn::security::Certificate signingCert,
                        ndn::security::Certificate attrAuthorityCertificate,
                        std::string trustSchemaPath);

            virtual ~ServiceUser();
            void init();

            /**
             * Install an SVSPubSub instance on a LocalMock user.
             *
             * This test-only hook lets the in-process integration fixture run
             * the same SVS publication and subscription path as production
             * without performing the controller/NAC bootstrap.
             */
            void attachLocalMockPubSubForTest(
                std::shared_ptr<ndn::svs::SVSPubSub> pubSub);

            /** Bind all LocalMock signing, including NAC-ABE wrapping, to the
             * fixture-owned KeyChain that contains the supplied certificate. */
            void useSigningKeyChain(ndn::KeyChain& keyChain);
            /** Backward-compatible test spelling for LocalMock fixtures. */
            void useSigningKeyChainForTest(ndn::KeyChain& keyChain);

            /** Bind only the LocalMock Data-signing owner for tests that
             * pre-provision their hybrid key and do not exercise NAC-ABE
             * bootstrap.  Unlike useSigningKeyChainForTest(), this hook does
             * not start a Consumer or fetch public parameters. */
            void useSigningKeyChainForSigningOnlyForTest(ndn::KeyChain& keyChain);

            /** Return whether the active LocalMock NAC-ABE Consumer has
             * obtained its DKEY.  Integration bootstrap uses this as a hard
             * readiness condition instead of inferring readiness from public
             * parameter traffic alone. */
            bool isNacConsumerReadyForTest();

            /** Return whether the active LocalMock NAC-ABE Producer has
             * obtained public parameters. */
            bool isNacProducerReadyForTest();

            /** Re-issue the LocalMock NAC-ABE Producer public-parameter
             * fetch after fixture transport wiring is complete. */
            void refreshNacProducerForTest();

            /** Seed a receive key for a LocalMock ingress test. */
            void cacheHybridReceiveKeyForTest(const std::string& keyId,
                                              const std::string& epochId,
                                              const ndn::Buffer& key);

            /** Prepare a deterministic LocalMock outbound key and mark its
             * wrapped-key state as already bootstrapped.  This is test-only
             * plumbing; production authorization still uses NAC-ABE. */
            HybridMessageKey prepareHybridSendKeyForTest(
                const ndn::Name& serviceName,
                const std::string& messageType);

            /** Cache a pre-built Data packet for LocalMock fetcher tests. */
            void cacheDataForTest(
                const ndn::Data& data,
                ndn::time::milliseconds freshness = ndn::DEFAULT_FRESHNESS_PERIOD);

            /** Trigger the real streamed cancellation path from LocalMock
             * integration fixtures that start through deferred collaboration. */
            void cancelStreamRequestForTest(const ndn::Name& requestId);
            /** Cancel callback delivery and terminal acceptance for one active
             * streamed request, including deferred collaboration requests. */
            void cancelStreamRequest(const ndn::Name& requestId);
            StreamedInvocationMetrics getStreamMetricsForTest(
                const ndn::Name& requestId) const;
            bool hasStreamStateForTest(const ndn::Name& requestId) const;
            size_t streamCallbackQueueHighWaterMarkForTest(
                const ndn::Name& requestId) const;

            ndn::Name getName();

            /** Open a validated semantic-name live stream on this user's Face. */
            std::shared_ptr<LiveStreamConsumerHandle>
            openLiveStream(const LiveStreamDescriptor& descriptor,
                           LiveStreamOpenOptions options);

            /** Open and start the predictive high-level stream subscription. */
            std::shared_ptr<PredictiveStreamSubscriber>
            subscribeStream(const PredictiveStreamDescriptor& descriptor,
                            StreamSubscriptionOptions options);

            void fetchPermissionsFromController(const ndn::Name& controllerPrefix);
            /**
             * Return whether a configured production requester has completed
             * the local authorization bootstrap needed to begin a protected
             * request. This predicate is intended for the Core request owner
             * while running on the Face I/O thread; it never waits or performs
             * network I/O. LocalMock users retain their explicit fixture
             * boundary and return ready here.
             */
            bool isRequestBootstrapReady(
                const ndn::Name& serviceName,
                const std::vector<ndn::Name>& providers = {});
            void applyPermissionResponse(const PermissionResponse& response);
            /** Install a Controller-signed policy status after the enclosing
             * Data has passed the configured trust validator. */
            bool installControllerStatus(const PolicyStatusData& status,
                                         bool controllerSignatureValid = true);
            /** Persist an accepted Controller status Data (opt-in durable
             * mode, FR-039); no-op when the store is disabled. */
            void persistAcceptedControllerStatus(
                const ndn::Data& validatedData,
                const PolicyStatusData& status);
            /** Restore persisted per-service statuses after startup (opt-in
             * durable mode, FR-039): unexpired, never-superseded records are
             * re-verified against the configured trust anchor and seeded as
             * authority; each seed schedules a bounded online confirmation
             * refresh whose Controller answer is authority. */
            void restorePersistedRuntimeStatuses();
            std::optional<ControllerVersion> getControllerVersion() const;
            std::optional<ControllerVersion> getControllerVersion(
                const ndn::Name& serviceName) const;
            size_t getCurrentPolicyEpoch() const;
            size_t getCurrentPolicyEpoch(const ndn::Name& serviceName) const;
            std::vector<std::tuple<std::string, std::string, size_t>>
            getAllowedServices() const;
            /// Return received NDNSD service details keyed by provider identity.
            std::map<std::string, ndnsd::discovery::Details>
            getNdnsdReceivedDetails() const;
            static bool handlePermissionResponseData(const ndn::Data& data,
                                                     const ndn::Name& identity,
                                                     ndn::KeyChain& keyChain,
                                                     ServiceAuthorizationTable& permissionTable);
            void setRequestPublisher(RequestPublisher publisher);
            void setLocalPublicationHandler(LocalPublicationHandler handler);
            static ndn::Buffer makeGenericAdmissionLeaseSelectionPayload(
                const std::string& leaseId,
                const ndn::Buffer& resourceBindingProof = ndn::Buffer());
            bool setSelectionAssignmentPayloadForRequest(
                const ndn::Name& requestId,
                const ndn::Name& providerName,
                const ndn::Buffer& assignmentPayload);
            void setRequestLifecycleCallback(RequestLifecycleCallback callback);
            void setAdmissionControlWarningHandler(AdmissionControlWarningHandler handler);
            void setAdmissionControlRejectHandler(AdmissionControlRejectHandler handler);
            std::optional<RequestLifecycleStatus>
            getRequestStatus(const ndn::Name& requestId) const;
            std::vector<RequestLifecycleStatus> getActiveRequestStatuses() const;
            static const char* requestLifecycleStateToString(RequestLifecycleState state);
            size_t getPendingCallCount() const;
            void setPendingCallTimeoutGrace(ndn::time::milliseconds grace);
            void setResponseRetryOptions(ResponseRetryOptions options);
            ResponseRetryOptions getResponseRetryOptions() const;
            void setPerformanceMode(bool enabled);
            void setHandlerThreads(size_t n);
            size_t getHandlerThreads() const;
            size_t getHandlerQueueDepth() const;
            void setAckProcessingThreads(size_t n);
            size_t getAckProcessingThreads() const;
            size_t getAckProcessingQueueDepth() const;
            void setUseTokens(bool enabled);
            bool getUseTokens() const;
#if defined(HAVE_TESTS)
            // Test-only clock seam for deterministic deadline assertions. It
            // is not present in production builds and does not alter the
            // public runtime contract.
            static void setTestClockForUnitTests(std::function<uint64_t()> clock);
#endif
            HybridCryptoCounters& getHybridCryptoCounters();
            void setTimelineTrace(bool enabled);
            struct RuntimeDiagnostics
            {
                uint64_t callbackSkippedNoPending = 0;
                uint64_t callbackSkippedTimeout = 0;
                uint64_t responseAfterPendingTimeout = 0;
                std::vector<double> ackLatenciesMs;
            };
            RuntimeDiagnostics consumeRuntimeDiagnostics();

            /** Wait off the Face/io_context thread for validated collaboration
             * records from the selected Providers. Records are retained until
             * explicitly consumed so a terminal Response cannot race receipt
             * delivery. */
            std::vector<VerifiedCollaborationData>
            waitForVerifiedCollaborationData(const RequestId& requestId,
                                             const KeyScope& keyScope,
                                             const ndn::Name& topicPrefix,
                                             size_t minCount,
                                             int timeoutMs,
                                             bool consume = true);

            /** Release verified collaboration records and the request-scope
             * key after a multi-step conversation transaction has finished.
             * Waiting with consume=true removes matching records but keeps the
             * key alive so a later authenticated control/ack can still use
             * the same request scope. */
            void clearVerifiedCollaborationData(const RequestId& requestId,
                                                const KeyScope& keyScope);

            struct AdaptiveAdmissionOptions
            {
                bool enabled = true;
                size_t minWindow = 1;
                size_t maxWindow = 512;
                size_t initialWindow = 16;
                size_t hardInflightLimit = 512;
                size_t aiStep = 4;
                double mdFactor = 0.85;
                double severeMdFactor = 0.5;
                int controlIntervalMs = 500;
                int targetLatencyMs = 350;
                int hardTargetLatencyMs = 500;
                size_t softQueueLimit = 0;
                size_t hardQueueLimit = 0;
                bool rateRecommendationEnabled = true;
                double initialRecommendedRateRps = 0.0;
                double minRecommendedRateRps = 1.0;
                double maxRecommendedRateRps = 0.0;
            };
            void setAdaptiveAdmissionControl(const AdaptiveAdmissionOptions& options);
            AdaptiveAdmissionOptions getAdaptiveAdmissionOptions() const;
            size_t getAdaptiveAdmissionWindow() const;
            size_t getAdaptiveAdmissionInflight() const;
            size_t getAdaptiveAdmissionQueueDepth() const;
            double getAdaptiveAdmissionRecommendedRateRps() const;
            void recordAdaptiveAdmissionBackpressure();

            static AckCandidatesHandler makeAckSelectionHandler(
                AckSelectionStrategy strategy);

            static std::vector<ndn_service_framework::AckSelectionCandidate>
            selectFirstRespondingAck(
                const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates);

            static std::vector<ndn_service_framework::AckSelectionCandidate>
            selectRandomAck(
                const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates);

            static std::vector<ndn_service_framework::AckSelectionCandidate>
            selectAllResponderAcks(
                const std::vector<ndn_service_framework::AckSelectionCandidate>& candidates);

            void PublishRequestV2(const std::vector<ndn::Name>& serviceProviderNames,
                                  const ndn::Name& serviceName,
                                  const ndn::Name& requestId,
                                  const ndn::Buffer& payload,
                                  const size_t& strategy=ndn_service_framework::tlv::FirstResponding);

            PreparedServiceRequest prepareServiceRequest(const std::string& serviceName);

            /** Publish an encrypted envelope as versioned, finalized Data
             * segments. With active publication enabled, the complete set is
             * staged in IMS before Face::put is queued; the method's success
             * result therefore means that publication was queued. A later
             * transport failure is reported by the Face/event path and cannot
             * be rolled back through this synchronous API. Active publication
             * rejects objects whose estimated segment set exceeds the current
             * IMS staging capacity; larger artifacts require a bounded-window
             * publication protocol. File-backed publications retain their
             * local range source for the bounded
             * ``NDNSF_REQUEST_LARGE_DATA_RETENTION_MS`` period; this is
             * independent of the wire FreshnessPeriod so delayed Provider
             * fetches do not observe a deleted request source.
             */
            LargeDataPublishResult publishEncryptedLargeData(
                const PreparedServiceRequest& ctx,
                const std::vector<uint8_t>& plaintext,
                const std::string& objectLabel = "",
                ndn::time::milliseconds freshness = ndn::DEFAULT_FRESHNESS_PERIOD,
                bool retainWhileLeased = false,
                const std::function<void()>& requireActive = {});

            /** Durable variant used by native preparation owners. */
            LargeDataPublishResult publishEncryptedLargeData(
                const PreparedServiceRequest& ctx,
                const std::vector<uint8_t>& plaintext,
                const std::string& objectLabel,
                ndn::time::milliseconds freshness,
                bool retainWhileLeased,
                const LargeDataPublishOptions& options,
                const std::function<void()>& requireActive = {});

            /** Blocking worker entry: caller keeps this User and its running
             * Face alive through return. Hash/encryption/storage run here;
             * key preparation and serving registration are marshalled to I/O.
             * Never call on the Face thread. Cancellation is cooperative. */
            LargeDataPublishResult publishEncryptedLargeDataFromWorker(
                const PreparedServiceRequest& ctx, const std::vector<uint8_t>& plaintext,
                const std::string& objectLabel,
                ndn::time::milliseconds freshness = ndn::DEFAULT_FRESHNESS_PERIOD,
                const std::function<void()>& requireActive = {});

            LargeDataPublishResult publishEncryptedLargeDataFromWorker(
                const PreparedServiceRequest& ctx, const std::vector<uint8_t>& plaintext,
                const std::string& objectLabel, ndn::time::milliseconds freshness,
                const LargeDataPublishOptions& options,
                const std::function<void()>& requireActive = {});

            /** True only when the configured range-store has durable ownership. */
            bool supportsDurableEncryptedLargeData() const noexcept;

            /** Configure before the first publication, on the owning thread.
             * The store handles ciphertext only; Core keeps naming/signing. */
            void setEncryptedLargeDataRangeStore(
                std::shared_ptr<EncryptedLargeDataRangeStore> store);

            /** Remove locally staged data for an aborted multi-object
             * publication.  This is a best-effort local transaction fence;
             * packets already queued on Face may still expire naturally. */
            void abortLargeDataPublications(
                const std::vector<LargeDataPublishResult>& publications) noexcept;

            /** Return bounded file-serving counters for a native selector. */
            LargeDataServingMetrics getLargeDataServingMetricsForTest() const;

            using SignedAppDataHandler = std::function<void(const ndn::Data&)>;
            using SignedAppDataFailureHandler =
                std::function<void(const ndn::Name&, const std::string&)>;

            /** Publish exact-name APP data signed by this ServiceUser identity.
             *
             * This is a small transport primitive for versioned APP records.
             * It does not define a new NDNSF message or invocation mode. The
             * name must remain below /<identity>/NDNSF/DI so another
             * application cannot use this user as an arbitrary Data signer.
             */
            ndn::Name publishSignedAppData(
                const ndn::Name& dataName,
                const ndn::Buffer& payload,
                ndn::time::milliseconds freshness = ndn::DEFAULT_FRESHNESS_PERIOD);

            /** Publish one requester-authenticated collaboration control record.
             * The payload is encrypted with the request scope key and the
             * outer SVS Data is signed by this User identity. It is intended
             * for request-scoped Provider commit/rollback controls, not for
             * application data or a new invocation mode.
             */
            bool publishCollaborationData(
                const ndn::Name& targetProvider,
                const ndn::Name& requestId,
                const KeyScope& keyScope,
                const ndn::Name& topic,
                const ndn::Buffer& payload);

            /** Fetch and validate one exact-name APP record.
             *
             * Validation uses the configured trust schema and additionally
             * requires the Data KeyLocator to belong to expectedSigner.
             */
            void fetchSignedAppData(
                const ndn::Name& dataName,
                const ndn::Name& expectedSigner,
                int timeoutMs,
                SignedAppDataHandler onData,
                SignedAppDataFailureHandler onFailure);

            LargeDataReferenceRequestResult makeRequestWithLargeDataOptimization(
                const PreparedServiceRequest& ctx,
                const std::vector<uint8_t>& payload,
                const std::string& objectLabel = "",
                const std::string& objectType = "",
                size_t thresholdBytes = 1024,
                ndn::time::milliseconds freshness = ndn::DEFAULT_FRESHNESS_PERIOD);

            ndn::Name RequestService(const PreparedServiceRequest& ctx,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name RequestService(const PreparedServiceRequest& ctx,
                                 const std::vector<ndn::Name>& providers,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name RequestService(const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name RequestServiceTracked(
                                 const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 SelectionStatusTimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding,
                                 SelectionStatusOptions statusOptions = SelectionStatusOptions());

            void QuerySelectionStatus(const ndn::Name& providerName,
                                      const ndn::Name& serviceName,
                                      const std::string& selectionDigest,
                                      SelectionStatusHandler onStatus,
                                      TimeoutHandler onTimeout,
                                      int timeoutMs = 500);

            std::vector<SelectionExecutionStatus>
            GetCollaborationStatusSnapshot(const ndn::Name& requestId) const;

            ndn::Name RequestServiceTargeted(const ndn::Name& provider,
                                 const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler);

            ndn::Name RequestService(const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name RequestService(const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int ackTimeoutMs,
                                 AcksHandler onAcksHandler,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler);

            ndn::Name RequestService(const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int ackTimeoutMs,
                                 AckCandidatesHandler onAcksHandler,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler);

            ndn::Name RequestService(const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int ackTimeoutMs,
                                 AckCandidatesHandler onAcksHandler,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t requestStrategy = ndn_service_framework::tlv::FirstResponding,
                                 const RequestId& requestId = RequestId());

            ndn::Name RequestService(const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int ackTimeoutMs,
                                 AckSelectionStrategy selectionStrategy,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 const RequestId& requestId = RequestId());

            ndn::Name RequestService(const ServiceName& service,
                                     const RequestPayload& request,
                                     int ackCollectionTimeMs,
                                     std::shared_ptr<const AckSelectionPolicy> selectionPolicy,
                                     int timeoutMs,
                                     ResponseHandler onResponse,
                                     TimeoutHandler onTimeout,
                                     const RequestId& requestId = RequestId());

            template<typename RequestT, typename EventT, typename ResponseT>
            std::shared_ptr<StreamedInvocationHandle<EventT, ResponseT>>
            RequestServiceStreaming(
                const ndn::Name& serviceName,
                const RequestT& request,
                StreamedInvocationOptions options,
                std::function<void(const EventT&)> onEvent,
                std::function<void(const ResponseT&)> onComplete,
                std::function<void(const StreamedInvocationError&)> onError,
                size_t strategy = ndn_service_framework::tlv::FirstResponding)
            {
                ndn::Buffer requestBytes;
                try {
                    requestBytes = serializeStreamValue(request);
                }
                catch (const std::exception&) {
                    return nullptr;
                }
                auto state = requestServiceStreamingBytes(
                    serviceName, ndn::Name(), requestBytes, std::move(options),
                    [onEvent = std::move(onEvent)](const ndn::Buffer& bytes) {
                        if (!onEvent) return;
                        if constexpr (std::is_same<EventT, ndn::Buffer>::value) {
                            onEvent(bytes);
                        }
                        else {
                            EventT event;
                            if (event.ParseFromArray(bytes.data(), bytes.size())) onEvent(event);
                        }
                    },
                    [onComplete = std::move(onComplete)](const ndn::Buffer& bytes) {
                        if (!onComplete) return;
                        if constexpr (std::is_same<ResponseT, ndn::Buffer>::value) {
                            onComplete(bytes);
                        }
                        else {
                            ResponseT response;
                            if (response.ParseFromArray(bytes.data(), bytes.size())) onComplete(response);
                        }
                    }, std::move(onError), strategy);
                return state ? std::make_shared<StreamedInvocationHandle<EventT, ResponseT>>(std::move(state)) : nullptr;
            }

            template<typename RequestT, typename EventT, typename ResponseT>
            std::shared_ptr<StreamedInvocationHandle<EventT, ResponseT>>
            RequestServiceStreaming(
                const ndn::Name& provider,
                const ndn::Name& serviceName,
                const RequestT& request,
                StreamedInvocationOptions options,
                std::function<void(const EventT&)> onEvent,
                std::function<void(const ResponseT&)> onComplete,
                std::function<void(const StreamedInvocationError&)> onError)
            {
                if (options.mode != InvocationMode::Targeted || provider.empty()) return nullptr;
                ndn::Buffer requestBytes;
                try { requestBytes = serializeStreamValue(request); }
                catch (const std::exception&) { return nullptr; }
                auto state = requestServiceStreamingBytes(
                    serviceName, provider, requestBytes, std::move(options),
                    [onEvent = std::move(onEvent)](const ndn::Buffer& bytes) {
                        if (!onEvent) return;
                        if constexpr (std::is_same<EventT, ndn::Buffer>::value) onEvent(bytes);
                        else { EventT event; if (event.ParseFromArray(bytes.data(), bytes.size())) onEvent(event); }
                    },
                    [onComplete = std::move(onComplete)](const ndn::Buffer& bytes) {
                        if (!onComplete) return;
                        if constexpr (std::is_same<ResponseT, ndn::Buffer>::value) onComplete(bytes);
                        else { ResponseT response; if (response.ParseFromArray(bytes.data(), bytes.size())) onComplete(response); }
                    }, std::move(onError), ndn_service_framework::tlv::FirstResponding);
                return state ? std::make_shared<StreamedInvocationHandle<EventT, ResponseT>>(std::move(state)) : nullptr;
            }

            ndn::Name RequestCollaboration(const ServiceName& service,
                                           const RequestPayload& initialRequest,
                                           CollaborationPlan plan,
                                           ResponseHandler onFinalResponse,
                                           TimeoutHandler onTimeout,
                                           const RequestId& requestId = RequestId());

            ndn::Name BeginCollaboration(const ServiceName& service,
                                         const RequestPayload& initialRequest,
                                         int ackCollectionTimeMs,
                                         int timeoutMs,
                                         CollaborationAckClosedHandler onAckClosed,
                                         ResponseHandler onFinalResponse,
                                         TimeoutHandler onTimeout,
                                         const RequestId& requestId = RequestId());

            ndn::Name BeginCollaboration(const ServiceName& service,
                                         const RequestPayload& initialRequest,
                                         int ackCollectionTimeMs,
                                         int timeoutMs,
                                         CollaborationAckClosedHandler onAckClosed,
                                         ResponseHandler onFinalResponse,
                                         TimeoutHandler onTimeout,
                                         const RequestId& requestId,
                                         CollaborationAckCoverageHandler onAckCoverage,
                                         const RequestCapabilities& requestCapabilities =
                                             RequestCapabilities(),
                                         const std::optional<StreamRequestOptions>&
                                             streamOptions = std::nullopt,
                                         std::function<void(const ndn::Buffer&)>
                                             onStreamEvent = {},
                                         std::function<void(const ndn::Buffer&)>
                                             onStreamComplete = {},
                                         std::function<void(const StreamedInvocationError&)>
                                             onStreamError = {});

            ndn::Name BeginCollaborationWithProviders(
                                         const ServiceName& service,
                                         const RequestPayload& initialRequest,
                                         int ackCollectionTimeMs,
                                         int timeoutMs,
                                         CollaborationAckClosedHandler onAckClosed,
                                         ResponseHandler onFinalResponse,
                                         TimeoutHandler onTimeout,
                                         const RequestId& requestId,
                                         CollaborationAckCoverageHandler onAckCoverage,
                                         const RequestCapabilities& requestCapabilities,
                                         const std::optional<StreamRequestOptions>&
                                             streamOptions,
                                         std::function<void(const ndn::Buffer&)>
                                             onStreamEvent,
                                         std::function<void(const ndn::Buffer&)>
                                             onStreamComplete,
                                         std::function<void(const StreamedInvocationError&)>
                                             onStreamError,
                                         const std::vector<ndn::Name>& providerNames);

            bool CommitCollaborationPlan(const RequestId& requestId,
                                         const std::string& ackClosedDigest,
                                         CollaborationPlan plan);

            /** Cancel a locally pending collaboration on the Face I/O thread.
             * Removes pending admission/timers and request keys, prevents a
             * later commit, and invokes no success/timeout callback. Returns
             * false for an absent or non-collaboration request. This is local
             * cancellation, not a receipt proving remote execution stopped. */
            bool CancelCollaboration(const RequestId& requestId);

            template<typename RequestT, typename ResponseT>
            ndn::Name RequestService(const ServiceName& service,
                                     const RequestT& request,
                                     int ackCollectionTimeMs,
                                     std::shared_ptr<const AckSelectionPolicy> selectionPolicy,
                                     int timeoutMs,
                                     std::function<void(const ResponseT&)> onResponse,
                                     std::function<void(const RequestId&)> onTimeout)
            {
                std::string requestBytes;
                if (!request.SerializeToString(&requestBytes)) {
                    return ndn::Name();
                }

                RequestPayload payload(
                    reinterpret_cast<const uint8_t*>(requestBytes.data()),
                    requestBytes.size());

                return RequestService(
                    service,
                    payload,
                    ackCollectionTimeMs,
                    std::move(selectionPolicy),
                    timeoutMs,
                    [response = std::move(onResponse)](
                        const ndn_service_framework::ResponseMessage& responseMessage) {
                        const auto responsePayload = responseMessage.getPayload();
                        ResponseT typedResponse;
                        if (!typedResponse.ParseFromArray(responsePayload.data(),
                                                          responsePayload.size())) {
                            return;
                        }
                        if (response) {
                            response(typedResponse);
                        }
                    },
                    std::move(onTimeout));
            }

            template<typename RequestT, typename ResponseT>
            ndn::Name RequestService(const std::vector<ndn::Name>& providers,
                                     const ndn::Name& serviceName,
                                     const RequestT& request,
                                     std::function<void(const ResponseT&)> onResponse,
                                     std::function<void()> onTimeout,
                                     int timeoutMs,
                                     size_t strategy = ndn_service_framework::tlv::FirstResponding)
            {
                std::string requestBytes;
                if (!request.SerializeToString(&requestBytes)) {
                    return ndn::Name();
                }

                ndn::Buffer payload(reinterpret_cast<const uint8_t*>(requestBytes.data()),
                                    requestBytes.size());

                ndn_service_framework::RequestMessage requestMessage;
                requestMessage.setPayload(payload, payload.size());
                requestMessage.setStrategy(strategy);

                return RequestService(providers,
                                  serviceName,
                                  std::move(requestMessage),
                                  timeoutMs,
                                  [timeout = std::move(onTimeout)](const ndn::Name&) {
                                      if (timeout) {
                                          timeout();
                                      }
                                  },
                                  [response = std::move(onResponse)](
                                      const ndn_service_framework::ResponseMessage& responseMessage) {
                                      const auto payload = responseMessage.getPayload();

                                      ResponseT typedResponse;
                                      if (!typedResponse.ParseFromArray(payload.data(), payload.size())) {
                                          return;
                                      }

                                      if (response) {
                                          response(typedResponse);
                                      }
                                  },
                                  strategy);
            }

            template<typename RequestT, typename ResponseT>
            ndn::Name RequestServiceTargeted(const ndn::Name& provider,
                                           const ndn::Name& serviceName,
                                           const RequestT& request,
                                           std::function<void(const ResponseT&)> onResponse,
                                           std::function<void()> onTimeout,
                                           int timeoutMs)
            {
                std::string requestBytes;
                if (!request.SerializeToString(&requestBytes)) {
                    return ndn::Name();
                }

                ndn::Buffer payload(reinterpret_cast<const uint8_t*>(requestBytes.data()),
                                    requestBytes.size());

                ndn_service_framework::RequestMessage requestMessage;
                requestMessage.setPayload(payload, payload.size());

                return RequestServiceTargeted(
                    provider,
                    serviceName,
                    std::move(requestMessage),
                    timeoutMs,
                    [timeout = std::move(onTimeout)](const ndn::Name&) {
                        if (timeout) {
                            timeout();
                        }
                    },
                    [response = std::move(onResponse)](
                        const ndn_service_framework::ResponseMessage& responseMessage) {
                        const auto payload = responseMessage.getPayload();

                        ResponseT typedResponse;
                        if (!typedResponse.ParseFromArray(payload.data(), payload.size())) {
                            return;
                        }

                        if (response) {
                            response(typedResponse);
                        }
                    });
            }

            void handleResponse(const ndn::Name& requestId,
                                const ndn::Name& providerName,
                                const ndn_service_framework::ResponseMessage& responseMessage);

            bool handleDecryptedResponse(const ndn::Name& requestId,
                                         const ndn::Name& providerName,
                                         const ndn_service_framework::ResponseMessage& responseMessage);

            // resolvedLargeResponse=true marks a Response whose payload was
            // reconstructed by resolveLargeResponseReferencePayload; that
            // path already authenticated every segment against the
            // invocation binding, nonce registry, and reference digest, so
            // the envelope decrypt step is skipped here.
            bool handleDecryptedResponse(const ndn::Name& requestId,
                                         const ndn::Name& providerName,
                                         const ndn_service_framework::ResponseMessage& responseMessage,
                                         bool resolvedLargeResponse);

            bool handleDecryptedResponse(const ndn::Name& requestId,
                                         const ndn_service_framework::ResponseMessage& responseMessage);

            bool handleDecryptedResponse(const ndn::Name& requestId,
                                         const ndn::Block& responseBlock);

            bool handleDecryptedResponseByName(const ndn::Name& responseName,
                                               const ndn_service_framework::ResponseMessage& responseMessage);

            bool handleDecryptedResponseByName(const ndn::Name& responseName,
                                               const ndn::Block& responseBlock);

            std::optional<ResponseMessage>
            resolveLargeResponseReferencePayload(const ResponseMessage& responseMessage,
                                                 const ndn::Name& responseName,
                                                 const ndn::Name& serviceName,
                                                 std::string& errorMessage);

            bool handleRequestAckByName(const ndn::Name& ackName,
                                        const ndn_service_framework::RequestAckMessage& ackMessage,
                                        AckAuthenticationEvidence authenticationEvidence = {});

            bool handleRequestAckByName(const ndn::Name& ackName,
                                        const ndn::Block& ackBlock);
            void dispatchDecryptedResponseByName(const ndn::Name& responseName,
                                                 const ndn::Name& requestId,
                                                 const ndn::Buffer& buffer,
                                                 const std::string& dataName = {},
                                                 const std::string& signerCertificate = {},
                                                 const std::string& wireDigest = {});
            void finishDecryptedResponseByName(const ndn::Name& responseName,
                                               const ndn::Name& requestId,
                                               ndn_service_framework::ResponseMessage responseMessage);
            void finishRequestAckOnEventLoop(const ndn::Name& providerName,
                                             const ndn::Name& ServiceName,
                                             const ndn::Name& requestID,
                                             ndn_service_framework::RequestAckMessage AckMessage,
                                             AckAuthenticationEvidence authenticationEvidence = {});

            virtual void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription);
            void OnCollaborationData(
                const ndn::svs::SVSPubSub::SubscriptionData& subscription);

            // ndnsd serviceinfo discovery callback
            void processNDNSDServiceInfoCallback(const ndnsd::discovery::Details& callback);

            void onPermissionResponseData(const ndn::Interest& interest,
                                           const ndn::Data& data);
            void onPermissionResponseTimeout(const ndn::Interest& interest,
                                             int attempt = 1);
            void fetchPolicyManifestFromController(const ndn::Name& controllerPrefix,
                                                   int attempt = 1);
            void onPolicyManifestData(const ndn::Interest& interest,
                                      const ndn::Data& data);
            void onPolicyManifestTimeout(const ndn::Interest& interest,
                                         int attempt = 1);
            void fetchPolicyStatusFromController(const ndn::Name& controllerPrefix,
                                                 const ndn::Name& serviceName,
                                                 int attempt = 1,
                                                 std::optional<ControllerVersion> expectedVersion = std::nullopt);
            void onPolicyStatusData(const ndn::Interest& interest,
                                    const ndn::Data& data,
                                    int attempt = 1);
            void onPolicyStatusTimeout(const ndn::Interest& interest,
                                       int attempt = 1);
            void scheduleControllerStatusRefresh(
                const ndn::Name& serviceName,
                const PolicyStatusData& status);
            // Bounded DKEY re-arm retries for a LocalMock whose immediate
            // refresh was deferred (no fixture-owned Consumer).  The retry
            // fires only while the Face is pumped and stops as soon as the
            // Consumer is ready or the attempt bound is exhausted.
            void scheduleDeferredDkeyRefreshRetry(const ndn::Name& serviceName);
            bool isAcceptablePolicyEpoch(size_t messageEpoch) const;
            bool isAcceptablePolicyEpoch(const ndn::Name& serviceName,
                                         size_t messageEpoch) const;
            bool isAcceptableControllerVersion(
                const std::optional<ControllerVersion>& messageVersion) const;
            bool isAcceptableControllerVersion(
                const ndn::Name& serviceName,
                const std::optional<ControllerVersion>& messageVersion) const;
            void maybeRefreshControllerVersionHint(
                const ndn::Name& serviceName,
                const std::optional<ControllerVersion>& messageVersion);
            /** Enforce an installed Controller-signed revocation state at a
             * protected User transition.  Before the first status snapshot is
             * installed, preserve bootstrap compatibility and let the normal
             * permission/version path proceed. */
            #if defined(__GNUC__)
            __attribute__((noinline))
            #endif
            bool authorizeControllerTransition(
                const ndn::Name& serviceName,
                ProtectedTransition transition) const;
            void adoptControllerVersion(const ControllerVersion& version);
            /**
             * Evict per-service authorization material after an authenticated
             * ControllerVersion change.  This is deliberately separate from
             * RevocationState's evidence set: the latter records which cache
             * families were affected, while this hook removes live User-side
             * token, binding, nonce, stream, collaboration, and incomplete
             * request state.
             */
            void invalidateControllerScopedCaches(
                const ndn::Name& serviceName,
                const ControllerVersion& version,
                bool abeGenerationChanged = true,
                const PolicyStatusData* status = nullptr,
                bool grantOnlyDkeyRefresh = false);
            /**
             * Retire only protected publications older than a strictly newer
             * service-scoped ControllerVersion.  The caller holds the
             * protected-reuse write fence; this method never touches Repo
             * durable ownership or performs Face/I/O work.
             */
            void retireProtectedPublications(
                const ndn::Name& serviceName,
                const ControllerVersion& version);
            /**
             * Run the DKEY-only refresh leg of a Controller-status install
             * (grant-only target-policy replacement, FR-017/SC-022).  Called
             * from invalidateControllerScopedCaches on a version-advance
             * install and directly from installControllerStatus when an
             * equal-version install consumes a pending grant-only refresh
             * that arrived after the version-advance install already ran
             * (reverse order: the status channel installed the version before
             * the PermissionResponse recorded the grant).  A LocalMock
             * without its fixture-owned Consumer defers to the explicit
             * bootstrap path via scheduleDeferredDkeyRefreshRetry.
             */
            void refreshNacDkeyForControllerStatus(
                const ndn::Name& serviceName,
                const ControllerVersion& version,
                bool abeGenerationChanged,
                bool grantOnlyDkeyRefresh);

            void OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            void OnRequestAckDecryptionSuccessCallback(const ndn::Name& providerName,
                                                       const ndn::Name& serviceName,
                                                       const ndn::Name& requestID,
                                                       const ndn::Buffer& buffer,
                                                       AckAuthenticationEvidence authenticationEvidence = {});

            void OnRequestAckDecryptionErrorCallback(const ndn::Name& providerName,
                                                     const ndn::Name& serviceName,
                                                     const ndn::Name& requestID,
                                                     const std::string& error);

            void PublishServiceSelectionMessageV2(const ndn::Name& providerName,
                                                     const ndn::Name& serviceName,
                                                     const ndn::Name& requestId);

            void OnResponseDecryptionErrorCallback(const ndn::Name& providerName,
                                                   const ndn::Name& serviceName,
                                                   const ndn::Name& requestID,
                                                   const std::string& error);

            bool replyFromIMS(const ndn::Interest &interest);

            // Large encrypted model objects may be too large for the
            // process-local IMS.  The file-backed path serves one finalized
            // segment per Interest while retaining the encrypted object on
            // disk until its freshness deadline or ServiceUser destruction.
            // Objects at or above the native streaming threshold use this
            // path automatically; NDNSF_REQUEST_LARGE_FILE_BACKED forces it
            // for smaller fixtures.  Smaller objects retain the legacy IMS
            // path.
            bool replyFromLargeDataFile(const ndn::Interest& interest);

            void onPrefixRegisterFailure(const ndn::Name& prefix, const std::string& reason);

            void onInterest(const ndn::InterestFilter &, const ndn::Interest &interest);

            void serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data>& contentData, ndn::nacabe::SPtrVector<ndn::Data>& ckData);

            void PublishMessage(const ndn::Name& messageName, const ndn::Name &messageNameWithoutPrefix, AbstractMessage& message);
            void publishHybridMessage(const ndn::Name& messageName,
                                      const ndn::Name& messageNameWithoutPrefix,
                                      AbstractMessage& message);
            void publishHybridEncodedMessage(const ndn::Name& messageName,
                                             ndn::Buffer plaintext);
            bool decryptHybridMessage(const ndn::Name& messageName,
                                      const ndn::Block& envelopeBlock,
                                      std::function<void(const ndn::Buffer&)> onSuccess,
                                      std::function<void(const std::string&)> onError);

            // Register NDNSF Messages in the ndn-svs
            void registerNDNSFMessages();

            // search for service info using ndnsd();
            void requestForServiceInfo();

            bool isFresh(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            

        protected:
            void registerIdentityPrefixWithRetry(size_t attempts = 0);

            void
            onMissingData(const std::vector<ndn::svs::MissingDataInfo> &);

        protected:
            struct StoredAck
            {
                ndn::Name providerName;
                ndn::Name serviceName;
                ndn::Name requestId;
                ndn_service_framework::RequestAckMessage message;
                AckAuthenticationEvidence authenticationEvidence{};
            };

            struct PendingCall;

            void PublishCompactServiceSelectionMessageV2(const std::vector<StoredAck>& selectedAcks);
            bool usesR1ReservationSelection(const PendingCall& pendingCall) const;
            void PublishR1SelectionDecision(const StoredAck& ack, bool selected);
            void closeR1ReservationDecisions(PendingCall& pendingCall);
            void pollR1DecisionReceipt(const ndn::Name& requestId,
                                       const std::string& reservationId);
            void retryR1Decision(const ndn::Name& requestId,
                                 const std::string& reservationId);
            ndn_service_framework::AckSelectionCandidate
            makeAckSelectionCandidate(const StoredAck& storedAck) const;

            struct PendingCall
            {
                struct R1DecisionDelivery
                {
                    ndn::Name providerName;
                    ndn::Name serviceName;
                    ndn::Name messageName;
                    ndn::Name messageSuffix;
                    ServiceSelectionMessage message;
                    std::string selectionDigest;
                    std::string decisionDigest;
                    uint64_t expiresAtMs = 0;
                    size_t transmissions = 0;
                    bool receiptAccepted = false;
                };
                std::vector<ndn::Name> providers;
                ndn::Name serviceName;
                ndn::Name requestName;
                ndn::Name requestNameWithoutPrefix;
                ndn_service_framework::RequestMessage requestMessage;
                size_t strategy = ndn_service_framework::tlv::FirstResponding;
                int timeoutMs = 0;
                int ackTimeoutMs = 0;
                uint64_t createdAtUs = 0;
                uint64_t publishedAtUs = 0;
                uint64_t firstAckAtUs = 0;
                uint64_t ackWindowDeadlineUs = 0;
                uint64_t ackSelectionAtUs = 0;
                uint64_t ackSelectionCompletedAtUs = 0;
                uint64_t selectionScheduledAtUs = 0;
                uint64_t selectionPublishedAtUs = 0;
                uint64_t responseObservedAtUs = 0;
                uint64_t responseDecryptedAtUs = 0;
                uint64_t responseValidatedAtUs = 0;
                uint64_t requestDeadlineUs = 0;
                AcksHandler acksHandler;
                AckCandidatesHandler ackCandidatesHandler;
                TimeoutHandler timeoutHandler;
                ResponseHandler responseHandler;
                bool hasResponse = false;
                bool admissionPublished = false;
                bool admissionReleased = false;
                bool ackTimeoutScheduled = false;
                bool requestTimeoutScheduled = false;
                bool scheduleAckTimeoutAfterPublish = false;
                bool scheduleImmediateAckTimeoutAfterPublish = false;
                bool ackWindowExpired = false;
                bool providerSelected = false;
                bool targetedMode = false;
                bool timedOut = false;
                bool timeoutGraceActive = false;
                ndn::scheduler::EventId requestTimeoutEvent;
                ndn::scheduler::EventId responseAttemptTimeoutEvent;
                bool responseRetryEnabled = false;
                bool responseRetryTimerArmed = false;
                int responseAttemptTimeoutMs = 0;
                size_t responseMaxAttempts = 1;
                uint64_t responseAttemptStartedAtUs = 0;
                std::vector<ndn::Name> responseAttemptProviders;
                size_t ackDecryptsInFlight = 0;
                size_t ackSelectionDeferrals = 0;
                size_t learnedAckProviderCountAtPublish = 0;
                std::vector<StoredAck> requestAcks;
                std::vector<StoredAck> customSelectedAcks;
                std::vector<ndn::Name> successfulAckProviders;
                std::vector<ndn::Name> negativeAckProviders;
                std::vector<ndn::Name> selectionPublishedProviders;
                std::vector<ndn::Name> expectedResponseProviders;
                std::vector<ndn::Name> responseProviders;
                std::vector<ndn::Name> responseDecryptProvidersInFlight;
                std::vector<ndn::Name> largeResponseReferenceProvidersInFlight;
                ndn::Name selectedProvider;
                std::map<std::string, std::string> providerTokens;
                ndn::Buffer selectionGatedInputKey;
                // Request-scoped confidentiality keeps the application input
                // out of the discovery Request.  The plaintext is retained
                // only until the selected Provider's exact-name Data packet
                // has been published, then the key bundle is retained only
                // for response decryption.
                struct RequestScopedProviderState
                {
                    RequestKeyBundle keys;
                    RequestSecurityBinding binding;
                    ndn::Name inputDataName;
                };
                ndn::Buffer requestScopedPlaintext;
                std::optional<RequestKeyBundle> requestScopedKeys;
                std::optional<RequestSecurityBinding> requestScopedBinding;
                ndn::Name requestScopedInputDataName;
                // Collaboration selection may authorize several Providers
                // in one request.  Each Provider has its own wrapped key
                // bundle and authenticated input Data name; keeping this
                // state per Provider prevents same-name ciphertext collisions
                // and response decryption with the last Provider's key.
                std::map<std::string, RequestScopedProviderState>
                    requestScopedProviderStates;
                bool requestScopedConfidentiality = false;
                std::map<std::string, std::string> negativeAckReasons;
                bool isCollaboration = false;
                bool collaborationDeferred = false;
                bool collaborationAcksClosed = false;
                bool collaborationPlanCommitted = false;
                CollaborationPlan collaborationPlan;
                CollaborationAckClosedHandler collaborationAckClosedHandler;
                CollaborationAckCoverageHandler collaborationAckCoverageHandler;
                std::vector<StoredAck> collaborationClosedAcks;
                std::vector<SelectedParticipant> collaborationCommittedParticipants;
                std::string collaborationAckClosedDigest;
                std::string collaborationCommittedPlanDigest;
                uint64_t collaborationAcksClosedAtUs = 0;
                std::map<std::string, ndn::Buffer> collaborationAssignments;
                // One generated key per committed collaboration dependency
                // scope.  The keys are carried only in the framework-owned
                // assignment envelope and are reused for an idempotent
                // Selection retransmission.
                std::map<std::string, ndn::Buffer> collaborationScopeKeys;
                std::map<std::string, ndn::Buffer> selectionAssignmentPayloads;
                bool trackSelectionStatus = false;
                SelectionStatusOptions selectionStatusOptions;
                SelectionStatusTimeoutHandler statusTimeoutHandler;
                std::map<std::string, std::string> selectionDigestsByProvider;
                std::map<std::string, SelectionExecutionStatus> selectionStatusesByProvider;
                std::map<std::string, R1DecisionDelivery> r1DecisionDeliveries;
                std::optional<DeploymentPlan> deploymentPlan;
                std::map<std::string, ProviderReadyMessage> deploymentReadyByMember;
                bool deploymentActivationSent = false;
                // Populated only by the streamed API. Unary and legacy
                // Targeted calls intentionally allocate no stream owner.
                std::shared_ptr<StreamInvocationLifecycle> streamLifecycle;
                std::optional<StreamRequestOptions> streamOptions;
                ndn::Buffer streamEventKey;
            };

            struct TargetedTokenPair
            {
                std::string providerToken;
                std::string userToken;
            };

            struct TargetedTokenPoolControl
            {
                size_t nextBatch = 0;
                size_t capacity = 0;
                size_t consumedSinceStore = 0;
                uint64_t lastStoredAtUs = 0;
                uint64_t refillStartedAtUs = 0;
                bool observed = false;
                bool refillInFlight = false;
            };

            struct TargetedStreamOffer
            {
                std::string providerBootEpoch;
                std::string recipientCertName;
                std::string recipientCertDigest;
                std::string recipientPublicKey;
            };

            /**
             * Attach streamed delivery to an already-created request. The
             * public API and local fixtures use this same ownership rule;
             * attachment cannot manufacture a second request ID or Request.
             */
            std::shared_ptr<StreamInvocationLifecycle>
            attachStreamLifecycle(const ndn::Name& requestId,
                                  bool deferredCollaboration);

            std::shared_ptr<StreamInvocationLifecycle>
            getStreamLifecycle(const ndn::Name& requestId) const;
            using StreamBytesCallback = std::function<void(const ndn::Buffer&)>;
            std::shared_ptr<StreamedInvocationSharedState>
            requestServiceStreamingBytes(
                const ndn::Name& serviceName,
                const ndn::Name& targetProvider,
                ndn::Buffer requestPayload,
                StreamedInvocationOptions options,
                StreamBytesCallback onEvent,
                StreamBytesCallback onComplete,
                std::function<void(const StreamedInvocationError&)> onError,
                size_t strategy);
            void onStreamEvent(const ndn::svs::SVSPubSub::SubscriptionData& subscription);
            bool initializeStreamConsumer(const ndn::Name& providerName,
                                          const ndn::Name& serviceName,
                                      const ndn::Name& requestId,
                                      const std::string& selectionDigest,
                                      const std::string& expectedProgressOperationId = {},
                                      std::vector<StreamProgressBinding>
                                        expectedProgressBindings = {});
            void armStreamInactivityTimer(const ndn::Name& requestId,
                                          uint64_t interestLifetimeMs);
            void disarmStreamInactivityTimer(const ndn::Name& requestId);
            struct PendingCallTraceRecord
            {
                uint64_t createdAtUs = 0;
                uint64_t erasedAtUs = 0;
                bool timedOut = false;
                bool completed = false;
                bool matchedAck = false;
                ndn::Name requestName;
            };

            static ndn::Name makeRequestId();

            static std::string sanitizeLargeDataObjectId(const std::string& objectLabel);

            static bool shouldTrackAckDecrypt(const PendingCall& pendingCall);

            static bool requiresRequestScopedConfidentiality(
                const ndn_service_framework::RequestMessage& requestMessage);
            /**
             * Apply the request-scoped confidentiality default for configured
             * Controller runtimes and move the application payload out of the
             * discovery Request.  Controller-free LocalMock callers and
             * explicit compatibility/feature modes retain their existing
             * behavior.  The returned plaintext is held only until the
             * selected Provider's exact-name Input Data is published.
             */
            bool prepareRequestScopedRequest(
                ndn_service_framework::RequestMessage& requestMessage,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                ndn::Buffer& plaintext) const;
            /**
             * Apply the current service-scoped ControllerVersion to a request
             * before any raw-payload overload publishes it.  The typed and
             * prepared paths already pass through startRequestServiceWithRequestId;
             * legacy payload overloads must share the same authority check.
             */
            bool prepareRequestControllerVersion(
                ndn_service_framework::RequestMessage& requestMessage,
                const ndn::Name& serviceName,
                const ndn::Name& requestId);
            ndn::Name makeRequestScopedInputDataName(
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                uint64_t attempt,
                const ndn::Name& providerName) const;

            bool evaluateAckSelection(const ndn::Name& requestId);

            bool handleAckCollectionTimeout(const ndn::Name& requestId,
                                             bool timerFired = true);

            bool closeDeferredCollaborationAcks(const ndn::Name& requestId,
                                                PendingCall& pendingCall);

            bool selectLateAckAfterAckTimeout(PendingCall& pendingCall,
                                              const StoredAck& storedAck);

            bool evaluateCustomAckSelection(PendingCall& pendingCall);

            ndn::Buffer externalizeLargeCollaborationAssignment(
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const ndn::Buffer& assignmentPayload);

            bool evaluateBuiltInAckSelection(PendingCall& pendingCall);
            void recordNegativeAck(PendingCall& pendingCall,
                                   const ndn::Name& requestId,
                                   const ndn::Name& providerName,
                                   const ndn_service_framework::RequestAckMessage& ackMessage);
            bool maybeEarlyStopAllKnownProvidersNegative(const ndn::Name& requestId);
            bool hasReachedLatePipelineStage(const PendingCall& pendingCall) const;
            void recordObservedAckProvider(const ndn::Name& serviceName,
                                           const ndn::Name& providerName,
                                           uint64_t timestampUs);
            size_t getRecentAckProviderCount(const ndn::Name& serviceName,
                                             uint64_t nowUs);
            bool collaborationAckRoleCoverageSatisfied(const ndn::Name& requestId,
                                                       const PendingCall& pendingCall) const;
            void scheduleRequestTimeout(const ndn::Name& requestId, int timeoutMs);
            void finalizeTimedOutPendingCall(const ndn::Name& requestId);
            void scheduleSelectionStatusQuery(const ndn::Name& requestId,
                                              const ndn::Name& providerName,
                                              const std::string& selectionDigest);
            void scheduleInitialSelectionStatusQuery(const ndn::Name& requestId,
                                                      const ndn::Name& providerName,
                                                      const std::string& selectionDigest);
            void querySelectionStatusForTimeoutDiagnostics(const ndn::Name& requestId,
                                                           const PendingCall& pendingCall);
            static SelectionExecutionStatus parseSelectionExecutionStatusPayload(
                const ndn::Data& data,
                const ndn::Name& providerName,
                const std::string& selectionDigest);
            void admitOrQueuePendingCall(const ndn::Name& requestId,
                                         bool scheduleAckTimeout,
                                         bool scheduleImmediateAckTimeout);
            std::pair<size_t, size_t>
            getEffectiveAdaptiveAdmissionQueueLimits(size_t activeLimit) const;
            AdmissionControlStatus makeAdmissionControlStatus(const ndn::Name& requestId,
                                                              size_t queueDepth,
                                                              const char* reason,
                                                              size_t softQueueLimit = 0,
                                                              size_t hardQueueLimit = 0) const;
            void notifyAdmissionControlWarning(const ndn::Name& requestId,
                                               size_t queueDepth,
                                               const char* reason,
                                               size_t softQueueLimit = 0,
                                               size_t hardQueueLimit = 0);
            void rejectPendingCallByAdmission(const ndn::Name& requestId,
                                              const char* reason,
                                              size_t softQueueLimit = 0,
                                              size_t hardQueueLimit = 0);
            void publishAdmittedPendingCall(const ndn::Name& requestId);
            void drainAdaptiveAdmissionQueue();
            void scheduleAdaptiveAdmissionControl();
            void controlAdaptiveAdmissionWindow();
            size_t getEffectiveAdaptiveAdmissionWindow() const;
            void releaseAdaptiveAdmissionSlot(const ndn::Name& requestId,
                                               PendingCall& pendingCall,
                                               const char* reason,
                                              uint64_t terminalTimestampUs);

            static bool containsName(const std::vector<ndn::Name>& names,
                                     const ndn::Name& name);

            static void addUniqueName(std::vector<ndn::Name>& names,
                                      const ndn::Name& name);
            static void removeName(std::vector<ndn::Name>& names,
                                   const ndn::Name& name);

            static ndn::Name selectRandomProvider(const std::vector<ndn::Name>& providers);

            bool hasUserPermissionForProvider(const ndn::Name& providerName,
                                              const ndn::Name& serviceName) const;
            bool hasUserPermissionForRequest(
                const std::vector<ndn::Name>& providers,
                const ndn::Name& serviceName) const;
            static std::string makeTargetedTokenPoolKey(
                const ndn::Name& providerName,
                const ndn::Name& serviceName);
            bool popTargetedTokenPair(const ndn::Name& providerName,
                                      const ndn::Name& serviceName,
                                      TargetedTokenPair& pair);
            void storeTargetedTokenPairs(const ndn::Name& providerName,
                                         const ndn::Name& serviceName,
                                         const ndn_service_framework::ResponseMessage& responseMessage);
            size_t getTargetedTokenBatchHint(const ndn::Name& providerName,
                                             const ndn::Name& serviceName);
            bool markTargetedTokenRefillInFlight(const ndn::Name& providerName,
                                                 const ndn::Name& serviceName,
                                                 size_t requestedBatch);
            void clearTargetedTokenRefill(const ndn::Name& providerName,
                                          const ndn::Name& serviceName);
            void maybeRefillTargetedTokenPool(const ndn::Name& providerName,
                                              const ndn::Name& serviceName);

            static const StoredAck* findStoredAck(
                const PendingCall& pendingCall,
                const ndn_service_framework::RequestAckMessage& ackMessage);

            ndn::Name startRequestServiceWithRequestId(const ndn::Name& requestId,
                                                  const std::vector<ndn::Name>& providers,
                                                  const ndn::Name& serviceName,
                                                  ndn_service_framework::RequestMessage requestMessage,
                                                  int timeoutMs,
                                                  TimeoutHandler onTimeout,
                                                  ResponseHandler onResponseHandler,
                                                  size_t strategy,
                                                  bool trackSelectionStatus = false,
                                                  SelectionStatusTimeoutHandler statusTimeoutHandler = {},
                                                  SelectionStatusOptions statusOptions = SelectionStatusOptions());
            ndn::Buffer prepareSelectionGatedInput(
                ndn_service_framework::RequestMessage& requestMessage,
                const ndn::Name& serviceName,
                const ndn::Name& requestId);

            void cleanupPendingCallState(const ndn::Name& requestId);
            bool handleProviderReadyInterest(const ndn::Interest& interest);
            void maybeActivateReadyDeployment(const ndn::Name& requestId,
                                              PendingCall& pendingCall);
            void publishExecutionActivate(const ndn::Name& provider,
                                          const std::string& controlHandle,
                                          const ExecutionActivateMessage& activation,
                                          int attempt = 0);
            void logRequestPendingCreated(const ndn::Name& requestId,
                                          const PendingCall& pendingCall);
            void erasePendingCallWithTrace(const ndn::Name& requestId,
                                           std::map<ndn::Name, PendingCall>::iterator pendingCall,
                                           const char* reason);
            void logAckMatchAttempt(const ndn::Name& requestId,
                                    const ndn::Name& ackName,
                                    const ndn::Name& providerName,
                                    uint64_t ackReceiveUs,
                                    const char* phase);
            void logAckNoPending(const ndn::Name& requestId,
                                 const ndn::Name& ackName,
                                 const ndn::Name& providerName,
                                 uint64_t ackReceiveUs);
            void updateRequestLifecycleState(const ndn::Name& requestId,
                                             RequestLifecycleState state,
                                             const char* cleanupReason = nullptr);
            std::string samplePendingCallKeys(size_t limit = 5) const;
            void dispatchResponseHandler(ResponseHandler responseHandler,
                                         const ndn::Name& requestId,
                                         ResponseMessage responseMessage);
            void scheduleResponseAttemptTimeout(const ndn::Name& requestId,
                                                const ndn::Name& providerName);
            bool retryResponseWithNextProvider(const ndn::Name& requestId,
                                               const char* trigger);

            ndn::nacabe::Consumer& activeNacConsumer()
            {
                return m_testNacConsumer ? *m_testNacConsumer : nacConsumer;
            }

            ndn::nacabe::CacheProducer& activeNacProducer()
            {
                return m_testNacProducer ? *m_testNacProducer : nacProducer;
            }

            ndn::Face& m_face;
            ndn::Scheduler m_scheduler;
            // Retry callbacks retain only a weak reference to this route owner.
            std::shared_ptr<ndn::ScopedRegisteredPrefixHandle> m_identityRegistration;
            std::vector<std::shared_ptr<ndn::ScopedRegisteredPrefixHandle>> m_serviceRegistrations;
            std::vector<std::shared_ptr<ndn::ScopedInterestFilterHandle>>
                m_testInterestFilters;
            ndn::Name identity;
            ndn::KeyChain m_keyChain;
            ndn::KeyChain* m_testSigningKeyChain = nullptr;
            std::shared_ptr<ndn::svs::SVSPubSub> m_svsps;
            std::shared_ptr<MessageValidator> validator;
            std::vector<std::string> m_serviceNames;

            //ndn::security::Validator nac_validator;
            ndn::ValidatorConfig nac_validator{m_face};
            ndn::security::Certificate identityCert;
            ndn::security::Certificate signingCert;
            ndn::security::Certificate attrAuthorityCertificate;
            
            ndn::nacabe::Consumer nacConsumer;
            std::unique_ptr<ndn::nacabe::Consumer> m_testNacConsumer;
            // LocalMock fixtures do not model the production Controller/AA
            // bootstrap lifecycle.  Keep their refresh path fail-closed until
            // the fixture explicitly completes its bootstrap.
            bool m_isLocalMock = false;
            //ndn::nacabe::Producer nacProducer;
            ndn::nacabe::CacheProducer nacProducer;
            std::unique_ptr<ndn::nacabe::CacheProducer> m_testNacProducer;
            ndn::security::SigningInfo m_signingInfo;
            bool m_useTokens = true;
            // Allow Python/experiment launchers to enable the existing
            // ndn-cxx TimelineTrace logger without a wrapper-only API seam.
            bool m_timelineTrace = timelineTraceEnvEnabled() || phaseTimingEnvEnabled();
            size_t m_currentPolicyEpoch = 0;
            size_t m_requiredKeyEpoch = 0;
            mutable std::mutex m_controllerVersionMutex;
            std::optional<ControllerVersion> m_controllerVersion;
            std::map<std::string, RevocationState> m_revocationStates;
            std::map<std::string, PolicyRefreshCoordinator>
                m_policyRefreshCoordinators;
            ndn::Name m_controllerPrefix;
            std::set<std::string> m_policyStatusFetchInFlight;
            std::map<std::string, ControllerVersion>
                m_policyStatusRefreshScheduled;
            // A grant-only PermissionResponse can add a service while the
            // ABE generation remains unchanged. Defer one identity-scoped
            // DKEY replacement for the affected service until its signed
            // PolicyStatus is installed; keep this per-service so an update
            // for S cannot trigger a refresh while installing T's status.
            std::set<std::string> m_nacDkeyRefreshPendingServices;
            // ControllerVersion of the last permission wave for which a
            // DKEY refresh was requested; same-wave service status installs
            // collapse into that single identity-wide replacement fetch.
            std::optional<ControllerVersion> m_lastDkeyRefreshWave;
            // Opt-in durable runtime status (NDNSF_PERSIST_RUNTIME_STATE):
            // accepted per-service statuses are persisted and restored across
            // process restart.  DKEY material is never stored.
            std::unique_ptr<RuntimeStatusStore> m_runtimeStatusStore;
            std::map<std::string, RuntimeStatusStore::Record>
                m_persistedRuntimeStatuses;
            uint64_t m_policyGracePeriodMs = 0;
            HybridMessageCrypto m_hybridMessageCrypto;
            HybridCryptoCounters m_hybridCryptoCounters;
            NonceRegistry m_requestScopedNonceRegistry;
            SerializedWorkerQueue m_cryptoProduceQueue{"ServiceUser NAC-ABE produce"};
            BoundedWorkerPool m_handlerPool{"ServiceUser response callbacks"};
            BoundedWorkerPool m_ackProcessingPool{"ServiceUser ACK processing"};

            ndn::InMemoryStorageFifo m_IMS;
            mutable std::mutex _cache_mutex;
            struct LargeDataFilePublication;
            struct LargeDataKeyReleaseState;
            struct ProtectedImsOwner
            {
                std::string serviceName;
                ControllerVersion version;
            };
            struct ProtectedPublicationPending
            {
                std::string serviceName;
                std::string publicationKey;
                ControllerVersion version;
                bool versioned = false;
                std::vector<std::string> imsNames;
                bool cancelled = false;
            };
            std::shared_ptr<LargeDataKeyReleaseState> m_largeDataKeyReleaseState;
            LargeDataPublishResult publishEncryptedLargeDataImpl(
                const PreparedServiceRequest&, const std::vector<uint8_t>&,
                const std::string&, ndn::time::milliseconds, bool,
                const std::function<void()>&, const LargeDataPublishOptions&,
                bool marshalIo);
            void expireLargeDataPublication(const std::string& publicationKey,
                std::weak_ptr<LargeDataFilePublication> publication);
            std::shared_ptr<EncryptedLargeDataRangeStore> m_largeDataRangeStore;
            std::map<std::string, std::shared_ptr<LargeDataFilePublication>>
                m_largeDataFiles;
            std::uintmax_t m_largeDataReservedBytes = 0;
            // Linearization fence for protected durable publication reuse.
            // Status transitions take the write side; final hit/registration
            // takes the read side.  No Repo/FileLock/Face callback is taken
            // while the write side is held.
            mutable std::shared_mutex m_protectedReuseMutex;
            // Serializes the short durable-reference commit protocol with a
            // status transition.  It is acquired before
            // m_protectedReuseMutex; no Face callback or cache lock is held
            // while waiting for it.
            mutable std::mutex m_protectedReferenceCommitMutex;
            std::map<std::string, std::map<std::string, ProtectedImsOwner>>
                m_protectedImsOwners;
            std::map<std::string, ProtectedPublicationPending>
                m_protectedPendingPublications;
            std::uint64_t m_protectedPendingSequence = 0;
            // Interest prefixes retired by a policy transition suppress an
            // IMS fallback even if a stale packet survived an unexpected
            // local cleanup path.
            std::set<std::string> m_retiredProtectedPrefixes;

            OptionalServiceDiscovery m_ServiceDiscovery;
            ServiceAuthorizationTable m_authorizations;

            std::map<ndn::Name, size_t> m_strategyMap;

            // a map used for load balancing requestID 
            std::map<ndn::Name, std::vector<AckInfo>> m_AckInfoMap;

            ConfigManager m_configManager;

            std::map<ndn::Name, int> m_sessionIDMap;

            std::mutex svs_mutex;

            std::map<ndn::Name, PendingCall> m_pendingCalls;
            std::mutex m_verifiedCollaborationMutex;
            std::condition_variable m_verifiedCollaborationCv;
            std::map<ndn::Name, std::map<KeyScope, ndn::Buffer>>
                m_userCollaborationScopeKeys;
            std::map<ndn::Name, std::vector<VerifiedCollaborationData>>
                m_verifiedCollaborationData;
            std::atomic<uint64_t> m_collaborationSequence{0};
            std::map<ndn::Name, std::shared_ptr<StreamEventConsumer>> m_streamConsumers;
            std::map<ndn::Name, std::shared_ptr<StreamedInvocationSharedState>> m_streamStates;
            std::mutex m_streamInactivityMutex;
            std::map<ndn::Name, uint64_t> m_streamInactivityEpochs;
            std::map<ndn::Name, ndn::Buffer> m_streamEventKeysPending;
            std::mutex m_targetedTokenPoolsMutex;
            std::map<std::string, std::deque<TargetedTokenPair>> m_targetedTokenPools;
            std::map<std::string, TargetedTokenPoolControl>
                m_targetedTokenPoolControls;
            std::map<std::string, TargetedStreamOffer> m_targetedStreamOffers;
            std::map<ndn::Name, std::map<std::string, uint64_t>>
                m_recentAckProvidersByService;
            std::map<ndn::Name, PendingCallTraceRecord> m_pendingCallTraceHistory;
            std::map<ndn::Name, RequestLifecycleStatus> m_requestLifecycleStatuses;
            RequestLifecycleCallback m_requestLifecycleCallback;
            AdmissionControlWarningHandler m_admissionControlWarningHandler;
            AdmissionControlRejectHandler m_admissionControlRejectHandler;
            RequestPublisher m_requestPublisher;
            LocalPublicationHandler m_localPublicationHandler;
            ndn::time::milliseconds m_pendingCallTimeoutGrace{500};
            ResponseRetryOptions m_responseRetryOptions;
            bool m_performanceMode = false;
            // const request-preparation paths record compatibility counters;
            // keep the diagnostics mutable so those observations are retained.
            mutable RuntimeDiagnostics m_runtimeDiagnostics;
            NetworkTelemetryStore m_networkTelemetry;
            AdaptiveAdmissionOptions m_adaptiveAdmissionOptions;
            size_t m_adaptiveAdmissionWindow = 16;
            size_t m_adaptiveAdmissionSlowStartThreshold = 512;
            size_t m_adaptiveAdmissionInflight = 0;
            bool m_adaptiveAdmissionControlScheduled = false;
            uint64_t m_adaptiveAdmissionIntervalSuccesses = 0;
            uint64_t m_adaptiveAdmissionIntervalTimeouts = 0;
            uint64_t m_adaptiveAdmissionIntervalBackpressure = 0;
            uint64_t m_adaptiveAdmissionIntervalQueueWarnings = 0;
            double m_adaptiveAdmissionIntervalLatencySumMs = 0.0;
            uint64_t m_adaptiveAdmissionIntervalLatencyCount = 0;
            std::vector<double> m_adaptiveAdmissionIntervalLatenciesMs;
            double m_adaptiveAdmissionBaselineLatencyMs = 0.0;
            double m_adaptiveAdmissionPreviousQueueDelayMs = 0.0;
            double m_adaptiveAdmissionPreviousAverageLatencyMs = 0.0;
            double m_adaptiveAdmissionPreviousP95LatencyMs = 0.0;
            double m_adaptiveAdmissionCompletionRateEmaRps = 0.0;
            double m_adaptiveAdmissionRecommendedRateRps = 0.0;
            size_t m_adaptiveAdmissionLatencyRisingIntervals = 0;
            size_t m_adaptiveAdmissionAverageLatencyRisingIntervals = 0;
            size_t m_adaptiveAdmissionRecoveryIntervals = 0;
            size_t m_adaptiveAdmissionSuccessfulControlIntervals = 0;
            size_t m_adaptiveAdmissionQueueDelayOverTargetIntervals = 0;
            bool m_adaptiveAdmissionIntervalCongested = false;
            bool m_adaptiveAdmissionIntervalSevere = false;
            std::deque<ndn::Name> m_adaptiveAdmissionQueue;
    };
}

namespace ndnsf
{
    using ProviderId = ndn_service_framework::ProviderId;
    using ServiceName = ndn_service_framework::ServiceName;
    using RequestId = ndn_service_framework::RequestId;
    using RequestPayload = ndn_service_framework::RequestPayload;
    using ResponsePayload = ndn_service_framework::ResponsePayload;
    using AckCandidate = ndn_service_framework::AckCandidate;
    using AckSelectionPolicy = ndn_service_framework::AckSelectionPolicy;

    namespace strategy
    {
        extern const std::shared_ptr<const ndn_service_framework::AckSelectionPolicy>
            FirstResponding;
        extern const std::shared_ptr<const ndn_service_framework::AckSelectionPolicy>
            RandomSelection;
        extern const std::shared_ptr<const ndn_service_framework::AckSelectionPolicy>
            AllSelected;
    }
}

#endif
