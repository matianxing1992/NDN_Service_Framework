#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP

#include "common.hpp"

#include "utils.hpp"

#include "ServiceAuthorizationTable.hpp"
#include "NDNSFMessages.hpp"
#include "InvocationStream.hpp"
#include "ConfigManager.hpp"
#include "HybridMessageCrypto.hpp"
#include "RequestConfidentiality.hpp"
#include "GenericSelectionTxnStore.hpp"
#include "NetworkTelemetry.hpp"
#include "TimelineTrace.hpp"
#include "Stream.hpp"
#include "StreamFacade.hpp"
#include "RevocationState.hpp"
#include "PolicyRefreshCoordinator.hpp"
#include "RuntimeStatusStore.hpp"

#include <functional>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <condition_variable>
#include <deque>
#include <tuple>
#include <utility>
#include <vector>



namespace ndn_service_framework{

    struct ServiceProviderTestAccess;

    using RequestPayload = ndn::Buffer;
    using ResponsePayload = ndn::Buffer;
    using ServiceName = ndn::Name;
    using CollaborationRole = std::string;
    using KeyScope = std::string;
    using Topic = ndn::Name;
    using SessionId = std::string;

    /**
     * Optional request-scoped binding used while discovering NDNSF_DATA_V1
     * segments.  The transport always checks the request id carried by the
     * CollaborationContext; callers that have a signed capability should
     * additionally bind the remaining capability fields before accepting a
     * catch-up publication from the shared SVS stream.
     */
    struct DataV1SegmentNameFilter
    {
        std::function<bool(const ndn::Name&)> predicate;
        /** Optional lifecycle observer invoked on the Face event loop after
         * the legacy SVS subscription is installed.  It does not affect
         * admission and exists so callers can coordinate a publisher without
         * timing sleeps. */
        std::function<void()> subscriptionReady;
    };

    struct LargeDataFetchResult
    {
        bool success = false;
        ndn::Buffer plaintext;
        std::string errorMessage;
    };

    /** Native-only measurements for one collaboration object transfer. */
    struct CollaborationTransferMetrics
    {
        std::string actualDataName;
        std::optional<std::size_t> transportPayloadBytes;
        std::optional<std::size_t> metadataBytes;
        std::optional<std::size_t> wireBytes;
        std::optional<std::size_t> interestCount;
        std::optional<std::size_t> retryCount;
        std::optional<std::size_t> localCopyBytes;
    };

    struct LargeDataResponsePublishResult
    {
        bool success = false;
        ndn::Name encryptedDataName;
        std::string objectId;
        std::string digest;
        std::string errorMessage;
    };

    struct LargeDataReferenceResponseResult
    {
        bool success = false;
        bool usedLargeDataReference = false;
        ndn_service_framework::ResponseMessage responseMessage;
        LargeDataResponsePublishResult largeData;
        std::string errorMessage;
    };

    class ServiceProvider
    {
        public:
            using ServiceKey = ndn::Name;

            struct AckDecision
            {
                bool status = false;
                bool suppressAck = false;
                std::string message;
                ndn::Buffer payload;
                std::optional<SelectionInputKeyOffer> selectionInputKeyOffer;
                std::optional<ReservationLease> reservationLease;
                // Provider-owned local retention horizon for pending
                // Request/Selection state. Zero keeps Core's bounded default.
                uint64_t pendingStateTtlMs = 0;
            };

            struct PeerNetworkMetric
            {
                ndn::Name srcPeer;
                ndn::Name dstPeer;
                double rttMs = 0.0;
                double bandwidthMbps = 0.0;
                double lossRate = 0.0;
                double jitterMs = 0.0;
                uint64_t observedAtMs = 0;
                double confidence = 1.0;
            };

            struct GenericProviderRuntimeHint
            {
                ndn::Name providerName;
                uint64_t queueLength = 0;
                uint64_t estimatedQueueWaitMs = 0;
                double cpuUtilization = 0.0;
                double gpuUtilization = 0.0;
                uint64_t freeMemoryMb = 0;
                uint64_t freeGpuMemoryMb = 0;
                std::vector<PeerNetworkMetric> peerMetrics;
            };

            struct GenericAdmissionLease
            {
                std::string leaseId;
                ndn::Name providerName;
                ndn::Name requesterName;
                ndn::Name serviceName;
                uint64_t expiresAtMs = 0;
                ndn::Buffer resourceBindingProof;
                bool consumed = false;
            };

            struct GenericLeaseValidationResult
            {
                bool status = false;
                std::string reasonCode;
                std::string leaseId;
            };

            struct GenericAckMetadata
            {
                std::optional<GenericProviderRuntimeHint> runtimeHint;
                std::vector<GenericAdmissionLease> leaseOffers;
                std::string servicePayloadSchema;
                ndn::Buffer servicePayload;
            };

            struct DataProductReference
            {
                ndn::Name name;
                ndn::Name producerName;
                ndn::Name serviceName;
                std::string objectClass;
                std::string contentType = "application/octet-stream";
                std::string digest;
                uint64_t sizeBytes = 0;
                uint64_t segmentCount = 0;
                uint64_t freshnessMs = 0;
            };

            struct ServiceOperationStatus
            {
                std::string operationId;
                std::string operation;
                ndn::Name serviceName;
                ndn::Name providerName;
                ndn::Name requestId;
                std::string role;
                uint64_t attempt = 1;
                uint64_t epoch = 1;
                uint64_t sequence = 1;
                std::string state = "QUEUED";
                std::string reasonCode;
                std::string message;
                bool progressKnown = false;
                double progress = 0.0;
                std::optional<DataProductReference> resultReference;
                uint64_t retryAfterMs = 0;
                uint64_t createdAtMs = 0;
                uint64_t updatedAtMs = 0;
                uint64_t expiresAtMs = 0;
                std::string detailsSchema;
                ndn::Buffer detailsPayload;
            };

            struct ProviderCapabilityHint
            {
                ndn::Name providerName;
                ndn::Name serviceName;
                bool ready = true;
                std::string drainState = "ACTIVE";
                std::string reasonCode;
                std::string message;
                std::optional<GenericProviderRuntimeHint> runtimeHint;
                std::vector<GenericAdmissionLease> leaseOffers;
                std::optional<ServiceOperationStatus> operationStatus;
                std::string servicePayloadSchema;
                ndn::Buffer servicePayload;

                bool readyForNewRequest() const;
            };

            class ProviderAdmissionLeaseTable
            {
            public:
                void grant(GenericAdmissionLease lease);
                GenericLeaseValidationResult consume(
                    const std::string& leaseId,
                    const ndn::Name& requesterName,
                    const ndn::Name& providerName,
                    const ndn::Name& serviceName,
                    const ndn::Buffer& resourceBindingProof,
                    uint64_t nowMs);
                size_t size() const;

            private:
                mutable std::mutex m_mutex;
                std::map<std::string, GenericAdmissionLease> m_leases;
            };

            static ndn::Buffer makeGenericAdmissionLeaseAckPayload(
                const GenericAdmissionLease& lease,
                const ndn::Buffer& servicePayload = ndn::Buffer());
            static ndn::Buffer makeGenericAckMetadataPayload(
                const GenericAckMetadata& metadata);
            static GenericAckMetadata parseGenericAckMetadataPayload(
                const ndn::Buffer& payload);
            static ndn::Buffer makePeerNetworkMetricPayload(
                const PeerNetworkMetric& metric);
            static std::optional<PeerNetworkMetric> parsePeerNetworkMetricPayload(
                const ndn::Buffer& payload);
            static ndn::Buffer makeDataProductReferencePayload(
                const DataProductReference& reference);
            static std::optional<DataProductReference> parseDataProductReferencePayload(
                const ndn::Buffer& payload);
            static ndn::Buffer makeServiceOperationStatusPayload(
                const ServiceOperationStatus& status);
            static std::optional<ServiceOperationStatus> parseServiceOperationStatusPayload(
                const ndn::Buffer& payload);
            static ndn::Buffer makeProviderCapabilityHintPayload(
                const ProviderCapabilityHint& hint);
            static std::optional<ProviderCapabilityHint> parseProviderCapabilityHintPayload(
                const ndn::Buffer& payload);

            struct GenericAdmissionLeaseValidationRequest
            {
                ndn::Name requesterName;
                ndn::Name providerName;
                ndn::Name serviceName;
                ndn::Name requestId;
                RequestMessage requestMessage;
                ServiceSelectionMessage selectionMessage;
                ndn::Buffer assignmentPayload;
            };

            using AckStrategyHandler =
                std::function<AckDecision(const RequestMessage&)>;

            using GenericAdmissionLeaseValidator =
                std::function<GenericLeaseValidationResult(
                    const GenericAdmissionLeaseValidationRequest&)>;

            using LegacyAckStrategyHandler =
                std::function<std::pair<bool, ndn::Block>(const RequestAckMessage&)>;

            using SimpleAckStrategyHandler =
                std::function<bool(const RequestMessage&)>;

            using RequestHandler =
                std::function<ResponseMessage(const ndn::Name& requesterIdentity,
                                              const ndn::Name& providerName,
                                              const ndn::Name& serviceName,
                                              const ndn::Name& requestId,
                                              const RequestMessage& requestMessage)>;

            using StreamingHandler =
                std::function<void(const ndn::Name& requesterIdentity,
                                   const ndn::Name& providerName,
                                   const ndn::Name& serviceName,
                                   const ndn::Name& requestId,
                                   const RequestMessage& requestMessage,
                                   StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer)>;

            /** Application-owned model preparation hook for the generic
             * selection-gated deployment protocol. The Core invokes it only
             * after a valid Selection carrying DeploymentPlan. Returning a
             * ProviderReadyMessage does not authorize handler execution. */
            using DeploymentPrepareHandler = std::function<ProviderReadyMessage(
                const ndn::Name& requesterIdentity,
                const ndn::Name& providerIdentity,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& request,
                const DeploymentPlan& plan,
                const std::string& selectionDigest)>;
            using ProviderReadyPublisher = std::function<void(
                const ndn::Name& requesterIdentity,
                const ProviderReadyMessage& ready)>;
            /** Application-owned reservation transition. Core authenticates
             * and fences the exact-target decision before invoking it. */
            using R1SelectionDecisionHandler =
                std::function<SelectionDecisionReceipt(const SelectionDecision&)>;
            using R1ReservationTerminalHandler =
                std::function<void(const std::string& reservationId,
                                   const std::string& cause)>;

            void setDeploymentPrepareHandler(DeploymentPrepareHandler handler);
            void setProviderReadyPublisher(ProviderReadyPublisher publisher);
            void setR1SelectionDecisionHandler(
                const ndn::Name& serviceName,
                R1SelectionDecisionHandler handler);
            void setR1ReservationTerminalHandler(
                const ndn::Name& serviceName,
                R1ReservationTerminalHandler handler);
            void setGenericSelectionTxnStore(
                std::shared_ptr<GenericSelectionTxnStore> store);
            void registerOpaqueSelectionParticipant(
                const ndn::Name& serviceName,
                std::shared_ptr<OpaqueSelectionParticipant> participant);
            bool acceptExecutionActivate(const ExecutionActivateMessage& activation,
                                         std::string* rejectionReason = nullptr);

            using SimpleRequestHandler =
                std::function<ResponseMessage(const RequestMessage& requestMessage)>;

            enum class ServiceMode
            {
                Normal,
                // Targeted services accept Request->Response invocation from
                // a requester that already names this provider as the target.
                Targeted,
            };

            enum class ServiceInvocationMode
            {
                NormalOnly,
                TargetedOnly,
                NormalAndTargeted,
            };

            struct CollaborationAssignment
            {
                CollaborationRole role;
                ServiceName service;
                ndn::Name assignedArtifact;
                ndn::Name artifactDataName;
                bool requiresProvisioning = false;
                int provisioningTimeoutMs = 0;
                ndn::Buffer assignmentPayload;
                std::map<KeyScope, ndn::Buffer> scopeKeys;
                std::map<KeyScope, ndn::Name> scopeKeyDataNames;
                std::map<CollaborationRole, ndn::Name> roleProviders;
                ndn::Buffer artifactPayload;
                std::string selectionDigest;
            };

            struct CollaborationData
            {
                SessionId sessionId;
                KeyScope keyScope;
                Topic topic;
                ndn::Name producer;
                CollaborationRole producerRole;
                uint64_t sequence = 0;
                ndn::Buffer payload;
            };

            class CollaborationContext
            {
            public:
                CollaborationContext(ServiceProvider& provider,
                                     ndn::Name requesterName,
                                     ndn::Name requestId,
                                     RequestMessage requestMessage,
                                     CollaborationAssignment assignment);

                SessionId sessionId() const;
                ndn::Name requesterName() const;
                CollaborationRole role() const;
                ndn::Name localProvider() const;
                const CollaborationAssignment& assignment() const;

                bool hasArtifact(const ndn::Name& artifactName) const;
                bool fetchArtifact(const ndn::Name& artifactName, int timeoutMs);
                std::optional<ndn::Buffer> getArtifact(const ndn::Name& artifactName) const;
                std::optional<ndn::Buffer> fetchEncryptedLargeData(
                    const ndn::Name& dataName,
                    const ndn::Name& serviceName = ndn::Name());
                void fail(const std::string& reason);

                void publish(KeyScope keyScope,
                             Topic topic,
                             const ndn::Buffer& payload);
                /**
                 * Publish one exact-name segmented object for collaboration.
                 *
                 * Use this family for large static or planned objects such as
                 * files, model artifacts, catalog snapshots, recordings, and
                 * DI tensor bundles. Consumers retrieve the returned name with
                 * fetchLarge(), which uses segmented Data / SegmentFetcher-style
                 * exact-name retrieval. Do not use StreamChunk for these objects
                 * unless an application explicitly needs a metadata-envelope
                 * experiment.
                 */
                ndn::Name publishLarge(KeyScope keyScope,
                                       Topic topic,
                                       const ndn::Buffer& payload,
                                       size_t maxSegmentSize = 7000,
                                       int freshnessMs = 60000);
                ndn::Name publishLarge(KeyScope keyScope,
                                       Topic topic,
                                       const ndn::Buffer& payload,
                                       size_t maxSegmentSize,
                                       int freshnessMs,
                                       CollaborationTransferMetrics* metrics);
                /**
                 * Publish one segmented object under a caller-chosen exact name.
                 *
                 * This is the preferred collaboration primitive when a plan or
                 * manifest already assigns the Data name, as in DI activation
                 * exchange. It is separate from the continuous stream substrate.
                 */
                ndn::Name publishLargeNamed(KeyScope keyScope,
                                            const ndn::Name& dataName,
                                            const ndn::Buffer& payload,
                                            size_t maxSegmentSize = 7000,
                                            int freshnessMs = 60000);
                ndn::Name publishLargeNamed(KeyScope keyScope,
                                            const ndn::Name& dataName,
                                            const ndn::Buffer& payload,
                                            size_t maxSegmentSize,
                                            int freshnessMs,
                                            CollaborationTransferMetrics* metrics);
                /**
                 * Fetch one exact-name segmented collaboration object.
                 *
                 * This is the large-object counterpart to publishLarge() and
                 * publishLargeNamed(). It is intended for exact-name object
                 * retrieval, not continuous stream consumption.
                 */
                std::optional<ndn::Buffer> fetchLarge(const ndn::Name& dataName,
                                                      KeyScope keyScope,
                                                      int timeoutMs);
                std::optional<ndn::Buffer> fetchLarge(const ndn::Name& dataName,
                                                      KeyScope keyScope,
                                                      int timeoutMs,
                                                      std::size_t expectedSegments);
                std::optional<ndn::Buffer> fetchLarge(const ndn::Name& dataName,
                                                      KeyScope keyScope,
                                                      int timeoutMs,
                                                      std::size_t expectedSegments,
                                                      CollaborationTransferMetrics* metrics);
                /**
                 * Publish request-scoped NDNSF_DATA_V1 segments through the
                 * SVSPubSub data path.  Each pair contains the complete
                 * segment Data name and the already-authenticated segment
                 * wire bytes; the transport must not wrap them in a second
                 * large-object envelope.
                 */
                bool publishDataV1Segments(
                    KeyScope keyScope,
                    const std::vector<std::pair<ndn::Name, ndn::Buffer>>& segments,
                    int freshnessMs = 60000);
                /**
                 * Fetch request-scoped NDNSF_DATA_V1 segment wires from a
                 * Provider's SVS publication stream.  The returned vector is
                 * ordered by segment number and contains no plaintext.
                 * ``expectedSegments == 0`` enables bounded in-subscription
                 * discovery: the caller-supplied decoder reads segment zero's
                 * authenticated manifest count, after which this same fetch
                 * waits for the exact complete segment set.
                 */
                std::optional<std::vector<ndn::Buffer>> fetchDataV1Segments(
                    KeyScope keyScope,
                    const ndn::Name& producerPrefix,
                    std::uint64_t operationIndex,
                    const std::string& producerRank,
                    const std::string& tensorDigest,
                    std::size_t expectedSegments,
                    std::size_t maxSegments,
                    int timeoutMs,
                    std::function<std::size_t(const ndn::Buffer&)>
                        segmentCountDecoder = {},
                    DataV1SegmentNameFilter nameFilter = {});
                /** Publish only the exact signed Data names authorized by a
                 * sealed V3 mayPublish contract. No SVS notification or
                 * process-local sequence is introduced. */
                bool publishSignedExactData(
                    KeyScope keyScope,
                    const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
                    int freshnessMs = 60000);
                bool publishSignedExactData(
                    KeyScope keyScope,
                    const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
                    int freshnessMs,
                    CollaborationTransferMetrics* metrics);
                /** Express and, on timeout/Nack, re-express the same exact
                 * Interest name until the bounded deadline. The returned
                 * content is released only after trust-schema and expected
                 * producer-identity validation. */
                std::optional<ndn::Buffer> fetchSignedExactData(
                    KeyScope keyScope,
                    const ndn::Name& dataName,
                    const ndn::Name& expectedProducer,
                    int timeoutMs,
                    std::function<bool()> shouldCancel = {});
                std::optional<ndn::Buffer> fetchSignedExactData(
                    KeyScope keyScope,
                    const ndn::Name& dataName,
                    const ndn::Name& expectedProducer,
                    int timeoutMs,
                    std::function<bool()> shouldCancel,
                    CollaborationTransferMetrics* metrics);
                void subscribe(KeyScope keyScope,
                               Topic topicPrefix,
                               std::function<void(const CollaborationData&)> onData);
                void subscribe(KeyScope keyScope,
                               Topic topicPrefix,
                               std::function<void(CollaborationContext&,
                                                  const CollaborationData&)> onData);
                /**
                 * Allow encrypted collaboration Data for this request only
                 * when its scope and topic match the supplied binding.
                 *
                 * This is useful for applications that consume through
                 * waitOne()/waitFor(): the receive filter is installed before
                 * any Data is decrypted, so unrelated role traffic is dropped
                 * without attempting authentication with the wrong scope key.
                 */
                void allowData(KeyScope keyScope, Topic topicPrefix);
                std::optional<CollaborationData> waitOne(KeyScope keyScope,
                                                         Topic topicPrefix,
                                                         int timeoutMs);
                std::vector<CollaborationData> waitFor(KeyScope keyScope,
                                                       Topic topicPrefix,
                                                       size_t minCount,
                                                       int timeoutMs);
                void reportOperationStatus(ServiceOperationStatus status);
                void publishFinalResponse(const ndn::Buffer& payload);

                /** Streamed DI bridge over the already-selected collaboration.
                 * These methods are valid only when the original Request carried
                 * StreamRequestOptions and Core attached its publisher after
                 * Selection; they never allocate another request or plan. */
                bool isStreamed() const;
                uint64_t publishStreamEvent(const ndn::Buffer& payload);
                bool finishStream(const ndn::Buffer& payload,
                                  StreamFinishReason reason);
                bool failStream(StreamedInvocationErrorCode code,
                                const std::string& message);
                /**
                 * Mark this selected collaboration role complete without
                 * publishing a user-facing Response.  This is required for
                 * non-final streamed DI roles: only the final role owns End
                 * and the terminal Response, while every other role must
                 * still release its provider-side pending request/lease.
                 */
                bool completeRole();
                bool streamCancelled() const;
                std::chrono::milliseconds streamRemainingDeadline() const;

            private:
                std::shared_ptr<StreamEventPublisher> streamPublisher() const;
                ServiceProvider& m_provider;
                ndn::Name m_requesterName;
                ndn::Name m_requestId;
                RequestMessage m_requestMessage;
                CollaborationAssignment m_assignment;
                bool m_streamTerminal = false;
            };

            using CollaborationHandler =
                std::function<void(CollaborationContext& ctx,
                                   const RequestMessage& initialRequest)>;

            // Test-only publication boundary for LocalMockTag integration
            // fixtures. Production instances publish through SVSPubSub.
            using LocalPublicationHandler =
                std::function<void(const ndn::Name& messageName,
                                   const ndn::Buffer& wire)>;

            /**
             * Test-only hook at the real streamed-event publication boundary.
             * Returning false suppresses the SVS publication after the exact
             * signed Data has been retained in the Provider IMS.  This lets
             * integration tests exercise the production exact-Interest retry
             * path without intercepting DummyFace traffic.
             */
            using StreamPublicationInterceptorForTest =
                std::function<bool(const ndn::Data& data)>;

            /** Test-only hook at the encrypted collaboration publication
             * boundary. Returning false suppresses the SVS publication after
             * the real Provider has built the collaboration Data name and
             * encrypted payload. */
            using CollaborationPublicationInterceptorForTest =
                std::function<bool(const ndn::Name& dataName)>;

            /** Test-only hook that can suppress retention before publication. */
            using StreamRetentionInterceptorForTest =
                std::function<bool(const ndn::Data& data)>;

            /** Test-only observer fired after a retained event is evicted. */
            using StreamRetentionExpiryObserverForTest =
                std::function<void(const ndn::Name& eventName)>;

            enum class ProviderRequestLifecycleState
            {
                REQUEST_OBSERVED,
                ACK_ADMISSION_CHECKED,
                ACK_SUPPRESSED_OVERLOAD,
                ACK_PUBLISHED,
                SELECTION_RECEIVED,
                EXECUTION_STARTED,
                EXECUTION_DONE,
                RESPONSE_PUBLISHED,
                PROVIDER_REQUEST_EXPIRED,
            };

            struct ProviderRequestLifecycleStatus
            {
                ndn::Name requestId;
                ndn::Name serviceName;
                ndn::Name providerName;
                ProviderRequestLifecycleState state =
                    ProviderRequestLifecycleState::REQUEST_OBSERVED;
                uint64_t requestObservedTimestampUs = 0;
                uint64_t ackAdmissionDecisionTimestampUs = 0;
                uint64_t ackPublishedOrSuppressedTimestampUs = 0;
                std::string suppressionReason;
                size_t providerPendingCountAtDecision = 0;
                uint64_t eventLoopLagUs = 0;
                uint64_t selectionLagUs = 0;
                uint64_t selectionReceivedTimestampUs = 0;
                uint64_t executionStartTimestampUs = 0;
                uint64_t executionDoneTimestampUs = 0;
                uint64_t responsePublishedTimestampUs = 0;
                std::string finalStatus;
            };

            using ProviderRequestLifecycleCallback =
                std::function<void(const ProviderRequestLifecycleStatus&)>;

            struct LocalMockTag
            {
            };

            ServiceProvider(ndn::Face& face, ndn::Name group_prefix, ndn::security::Certificate identityCert, ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
            ServiceProvider(ndn::Face& face,
                            ndn::Name group_prefix,
                            ndn::security::Certificate encryptionCert,
                            ndn::security::Certificate signingCert,
                            ndn::security::Certificate attrAuthorityCertificate,
                            std::string trustSchemaPath);
            ServiceProvider(LocalMockTag,
                            ndn::Face& face,
                            ndn::Name group_prefix,
                            ndn::security::Certificate identityCert,
                            ndn::security::Certificate attrAuthorityCertificate,
                            std::string trustSchemaPath);
            ServiceProvider(LocalMockTag,
                            ndn::Face& face,
                            ndn::Name group_prefix,
                            ndn::security::Certificate encryptionCert,
                            ndn::security::Certificate signingCert,
                            ndn::security::Certificate attrAuthorityCertificate,
                            std::string trustSchemaPath);
            virtual ~ServiceProvider();

            void init();

            /**
             * Install an SVSPubSub instance on a LocalMock provider.
             *
             * This is intentionally restricted to the LocalMock construction
             * path so in-process integration tests can exercise the same
             * production collaboration transport without entering NAC-ABE
             * bootstrap. Normal providers create their SVS instance during
             * construction and must not call this hook.
             */
            void attachLocalMockPubSubForTest(
                std::shared_ptr<ndn::svs::SVSPubSub> pubSub);

            /**
             * Install only the LocalMock exact-Data/IMS interest filters.
             *
             * This keeps the LocalMock publication callback path intact while
             * allowing a test to fetch Data objects retained in the Provider
             * IMS.  It intentionally does not attach an SVSPubSub instance or
             * change publication semantics.
             */
            void installLocalMockDataIngressForTest();

            /**
             * Seed a receive key for a LocalMock integration test.  This keeps
             * the test on the real HybridMessageEnvelope ingress while
             * intentionally omitting the controller/NAC bootstrap from the
             * transport gate.
             */
            void cacheHybridReceiveKeyForTest(const std::string& keyId,
                                              const std::string& epochId,
                                              const ndn::Buffer& key);

            /**
             * Prime one LocalMock outbound epoch and return the matching key to
             * the integration fixture.  The fixture installs it in the peer's
             * receive cache, modelling a completed NAC-ABE MessageKey exchange
             * while retaining the real encrypted ACK/Response wire path.
             */
            HybridMessageKey prepareHybridSendKeyForTest(
                const ndn::Name& serviceName,
                const std::string& messageType);

            /**
             * Mark the LocalMock response epoch as already wrapped.  The
             * in-process transport gate has no controller/NAC-ABE producer,
             * so this lets it verify the real encrypted Response publication
             * without silently replacing the post-Selection path with a
             * plaintext callback.
             */
            void markHybridResponseKeyWrappedForTest(
                const ndn::Name& serviceName);

            /** Bind LocalMock signing to the fixture KeyChain that owns the
             * supplied certificate and private key. Production providers keep
             * using their process KeyChain. */
            void useSigningKeyChainForTest(ndn::KeyChain& keyChain);

            /** Return whether the active LocalMock NAC-ABE Consumer has
             * obtained its DKEY. */
            bool isNacConsumerReadyForTest();

            /** Return whether the active LocalMock NAC-ABE Producer has
             * obtained public parameters. */
            bool isNacProducerReadyForTest();

            /** Re-issue the LocalMock NAC-ABE Producer public-parameter
             * fetch after fixture transport wiring is complete. */
            void refreshNacProducerForTest();

            /** Install or clear the LocalMock streamed-event publication hook. */
            void setStreamPublicationInterceptorForTest(
                StreamPublicationInterceptorForTest interceptor);
            void setCollaborationPublicationInterceptorForTest(
                CollaborationPublicationInterceptorForTest interceptor);
            void setStreamRetentionInterceptorForTest(
                StreamRetentionInterceptorForTest interceptor);
            void setStreamRetentionExpiryObserverForTest(
                StreamRetentionExpiryObserverForTest observer);
            /** Re-publish one already signed streamed Data packet through the
             * production SVS endpoint.  This is test-only fault-controller
             * plumbing used to inject duplicate and reordered publications;
             * it does not re-sign or modify the packet. */
            void publishStreamPacketForTest(const ndn::Data& data);
            size_t streamPublisherHighWaterMarkForTest(
                const ndn::Name& requesterName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId);

            ndn::Name getName();

            /** Public names of the certificate used for Provider-signed Data.
             * These expose no private key material and let an external
             * application signer select the exact Provider signing key. */
            ndn::Name getSigningKeyName() const;
            ndn::Name getSigningCertificateName() const;

            /** Create the sole Core-owned semantic-name live-stream publisher. */
            std::shared_ptr<LiveStreamPublisher>
            createLiveStream(const LiveStreamDefinition& definition);

            /** Create the additive high-level stream facade. */
            std::shared_ptr<StreamPublisher>
            createStream(const StreamConfig& config);

            void fetchPermissionsFromController(const ndn::Name& controllerPrefix);
            /** Re-fetch the identity-wide ProviderPermission renewal after an
             * already-installed policy status advanced to a newer
             * ControllerVersion (a revocation discovered through the
             * scheduled refresh).  Cold-bootstrap first installs skip this:
             * the enclosing bootstrap already fetched permissions. */
            void refreshProviderPermissionsAfterAdvance(
                const ndn::Name& serviceName);
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
            /**
             * Return whether the current provider authorization table contains
             * permission for the requested service.  This is a runtime
             * readiness query, not an authorization bypass; request handling
             * still performs its own permission check.
             */
            bool hasProviderPermissionForService(const ndn::Name& serviceName) const;
            size_t getCurrentPolicyEpoch() const;
            size_t getCurrentPolicyEpoch(const ndn::Name& serviceName) const;
            static bool handlePermissionResponseData(const ndn::Data& data,
                                                     const ndn::Name& identity,
                                                     ndn::KeyChain& keyChain,
                                                     ServiceAuthorizationTable& permissionTable);

            size_t getPendingRequestCountForTesting() const;
            size_t getSelectedOutstandingRequestCountForTesting() const;
            size_t getPendingProviderTokenCountForTesting() const;
            size_t getCleanupInvocationCountForTesting() const;
            size_t getTokenConsumeCountForTesting() const;
            void setPendingRequestTimeoutGrace(ndn::time::milliseconds grace);
            void setPerformanceMode(bool enabled);
            void setHandlerThreads(size_t n);
            size_t getHandlerThreads() const;
            size_t getHandlerQueueDepth() const;
            void setAckThreads(size_t n);
            size_t getAckThreads() const;
            size_t getAckQueueDepth() const;
            void setUseTokens(bool enabled);
            bool getUseTokens() const;
            HybridCryptoCounters& getHybridCryptoCounters();
            void setTimelineTrace(bool enabled);
            void setAdaptiveAckAdmission(bool enabled);
            void setProviderAckMaxPending(size_t maxPending);
            void setProviderAckMaxEventLoopLag(ndn::time::milliseconds maxLag);
            void setProviderAckMaxSelectionLag(ndn::time::milliseconds maxLag);
            void setProviderRequestLifecycleCallback(
                ProviderRequestLifecycleCallback callback);
            std::optional<ProviderRequestLifecycleStatus>
            getProviderRequestStatus(const ndn::Name& requestId) const;
            std::vector<ProviderRequestLifecycleStatus>
            getActiveProviderRequestStatuses() const;
            std::optional<SelectionExecutionStatus>
            getSelectionExecutionStatus(const std::string& selectionDigest) const;
            std::map<std::string, uint64_t> getProviderAdmissionCounters() const;
            static const char* providerRequestLifecycleStateToString(
                ProviderRequestLifecycleState state);

            void publishServiceInfo(const ndn::Name& serviceName,
                                    int serviceLifetimeSeconds,
                                    std::map<std::string, std::string> serviceMetaInfo = {});

            /// Update one key in the internal NDNSD meta dict (thread-safe).
            void updateNdnsdMeta(const std::string& key, const std::string& value);

            /// Replace the entire internal NDNSD meta dict (thread-safe).
            void setNdnsdMeta(const std::map<std::string, std::string>& meta);

            /// Start periodic NDNSD heartbeat for all registered services.
            /// Meta is read from the internal dict (updated via updateNdnsdMeta).
            void startNdnsdPeriodicPublish(int intervalSeconds);

            /// Stop the periodic NDNSD heartbeat. Must be called on the Face
            /// event-loop thread because the scheduler is not thread-safe.
            void stopNdnsdPeriodicPublish();

            /**
             * Opaque move-only RAII handle for a scoped registration.  The
             * handle keeps the registration generation alive; close() (also
             * run on destruction) atomically closes the generation so no new
             * ACK/Selection dispatch and no queued execution can observe it,
             * then schedules Core entry cleanup on the Face event loop.
             * Closing a handle after the Provider has been destroyed is a
             * harmless no-op.  Handles are created only by the addScoped*
             * registration APIs below.
             */
            class ServiceRegistration;

            /**
             * Register a service under exclusive scoped ownership: the
             * returned handle's generation is bound to every authenticated
             * Request this service accepts, so closing the handle stops late
             * ACK completion, Selection dispatch and queued execution from
             * publishing results for that generation.  The name must not be
             * occupied by any other registration (legacy or active scoped);
             * closed generations are drained before the check.  All legacy
             * addService/addCollaborationHandler signatures and behavior are
             * unchanged; legacy adds refuse to overwrite an active scoped
             * registration.  Like the legacy registration APIs, these may be
             * called only on the Face event thread or before the event loop
             * starts.
             */
            ServiceRegistration addScopedService(const ndn::Name& serviceName,
                                                 AckStrategyHandler ackHandler,
                                                 RequestHandler requestHandler,
                                                 ServiceInvocationMode invocationMode);
            ServiceRegistration addScopedService(const ndn::Name& serviceName,
                                                 AckStrategyHandler ackHandler,
                                                 RequestHandler requestHandler,
                                                 ServiceMode mode);

            /**
             * Scoped variant of addCollaborationHandler with the same
             * generation binding and exclusivity rules as addScopedService.
             */
            ServiceRegistration addScopedCollaborationHandler(
                const ndn::Name& serviceName,
                std::vector<CollaborationRole> allowedRoles,
                AckStrategyHandler ackHandler,
                CollaborationHandler handler);

            void OnRequest(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            // After receiving service selection message, this function is called to consumeRequest.
            // Generic dynamic providers can rely on this safe default; legacy generated providers
            // may still override it for service-specific dispatch.
            void addService(const ndn::Name& serviceName,
                            AckStrategyHandler ackHandler,
                            RequestHandler requestHandler);

            /** Register a generic streamed handler. The handler receives the
             * same authenticated Request/Selection context as unary dispatch;
             * the Core owns cursor, AEAD, signed Data publication and the
             * terminal Response. */
            void addStreamingHandler(const ndn::Name& serviceName,
                                     StreamingHandler handler);

            template<typename RequestT, typename EventT, typename ResponseT>
            void addStreamingHandler(
                const ndn::Name& serviceName,
                std::function<void(const RequestT&,
                                   StreamedResponseWriter<EventT, ResponseT>&)> handler)
            {
                addStreamingHandler(
                    serviceName,
                    [handler = std::move(handler)](
                        const ndn::Name&, const ndn::Name&, const ndn::Name&,
                        const ndn::Name&, const RequestMessage& request,
                        StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
                        RequestT typedRequest;
                        const auto payload = request.getPayload();
                        if constexpr (std::is_same<RequestT, ndn::Buffer>::value) {
                            typedRequest = payload;
                        }
                        else if (!typedRequest.ParseFromArray(payload.data(), payload.size())) {
                            writer.fail(StreamedInvocationErrorCode::InvalidOptions,
                                        "stream request serialization failed");
                            return;
                        }
                        auto core = writer.core();
                        StreamedResponseWriter<EventT, ResponseT> typedWriter(core);
                        handler(typedRequest, typedWriter);
                    });
            }

            void addService(const ndn::Name& serviceName,
                            AckStrategyHandler ackHandler,
                            RequestHandler requestHandler,
                            ServiceMode mode);

            void addService(const ndn::Name& serviceName,
                            AckStrategyHandler ackHandler,
                            RequestHandler requestHandler,
                            ServiceInvocationMode invocationMode);

            void addService(const ndn::Name& serviceName,
                            LegacyAckStrategyHandler ackHandler,
                            RequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            RequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            AckStrategyHandler ackHandler,
                            SimpleRequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            AckStrategyHandler ackHandler,
                            SimpleRequestHandler requestHandler,
                            ServiceInvocationMode invocationMode);

            void addTargetedService(const ndn::Name& serviceName,
                                    RequestHandler requestHandler);

            void addTargetedService(const ndn::Name& serviceName,
                                    SimpleRequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            LegacyAckStrategyHandler ackHandler,
                            SimpleRequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            SimpleAckStrategyHandler ackHandler,
                            RequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            SimpleAckStrategyHandler ackHandler,
                            SimpleRequestHandler requestHandler);

            void addService(const ndn::Name& serviceName,
                            SimpleAckStrategyHandler ackHandler,
                            SimpleRequestHandler requestHandler,
                            ServiceInvocationMode invocationMode);

            void addCollaborationHandler(const ndn::Name& serviceName,
                                         AckStrategyHandler ackHandler,
                                         CollaborationHandler handler);

            void addCollaborationHandler(const ndn::Name& serviceName,
                                         std::vector<CollaborationRole> allowedRoles,
                                         AckStrategyHandler ackHandler,
                                         CollaborationHandler handler);

            void addCollaborationHandler(const ndn::Name& serviceName,
                                         CollaborationHandler handler);

            void addCollaborationHandler(const ndn::Name& serviceName,
                                         std::vector<CollaborationRole> allowedRoles,
                                         CollaborationHandler handler);

            void setAckStrategyHandler(const ndn::Name& serviceName,
                                       AckStrategyHandler ackHandler);

            void setLocalPublicationHandler(LocalPublicationHandler handler);

            void setLegacyAckStrategyHandler(const ndn::Name& serviceName,
                                             LegacyAckStrategyHandler ackHandler);

            void setSelectionStatusQueryable(const ndn::Name& serviceName,
                                              bool enabled = true);

            /** Attach the latest bounded member snapshot before a
             * CollaborationContext exists. The selection digest is the
             * responsibility binding; it is not readiness authority. */
            void reportSelectionOperationStatus(
                const std::string& selectionDigest,
                ServiceOperationStatus status);
            void setGenericAdmissionLeaseValidator(
                const ndn::Name& serviceName,
                GenericAdmissionLeaseValidator validator,
                bool required = true);
            void setGenericAdmissionLeaseRequired(const ndn::Name& serviceName,
                                                  bool required = true);
            void grantGenericAdmissionLease(GenericAdmissionLease lease);

            template<typename RequestT, typename ResponseT>
            void addHandler(const ndn::Name& serviceName,
                            std::function<void(const ndn::Name& requesterIdentity,
                                               const RequestT& request,
                                               ResponseT& response)> handler)
            {
                addService(serviceName,
                           [handler = std::move(handler)](
                               const ndn::Name& requesterIdentity,
                               const ndn::Name&,
                               const ndn::Name& serviceName,
                               const ndn::Name&,
                               const RequestMessage& requestMessage) {
                               const auto payload = requestMessage.getPayload();

                               RequestT typedRequest;
                               if (!typedRequest.ParseFromArray(payload.data(), payload.size())) {
                                   return makeErrorResponse("Failed to parse request payload for " +
                                                            serviceName.toUri());
                               }

                               ResponseT typedResponse;
                               handler(requesterIdentity, typedRequest, typedResponse);

                               std::string responseBytes;
                               if (!typedResponse.SerializeToString(&responseBytes)) {
                                   return makeErrorResponse("Failed to serialize response payload for " +
                                                            serviceName.toUri());
                               }

                               ndn::Buffer responsePayload(
                                   reinterpret_cast<const uint8_t*>(responseBytes.data()),
                                   responseBytes.size());

                               ResponseMessage responseMessage;
                               responseMessage.setStatus(true);
                               responseMessage.setErrorInfo("No error");
                               responseMessage.setPayload(responsePayload, responsePayload.size());
                               return responseMessage;
                           });
            }

            bool hasService(const ndn::Name& serviceName) const;

            LargeDataFetchResult fetchAndDecryptLargeData(
                const ndn::Name& encryptedDataName,
                const std::string& serviceName);

            LargeDataFetchResult resolveLargeDataReferencePayload(
                ndn::Buffer payload,
                const std::string& serviceName);

            // Request-scoped large responses cannot use the legacy
            // service-wide HybridMessageCrypto carrier.  Each segment is an
            // independently authenticated AeadEnvelope bound to the same
            // invocation and a unique segment identity.
            LargeDataReferenceResponseResult makeRequestScopedResponseWithLargeDataOptimization(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                ResponseMessage response,
                const RequestKeyBundle& keys,
                RequestSecurityBinding binding,
                size_t thresholdBytes = 0,
                ndn::time::milliseconds freshness = ndn::DEFAULT_FRESHNESS_PERIOD);

            ResponseMessage dispatchRequest(const ndn::Name& requesterIdentity,
                                            const ndn::Name& providerName,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId,
                                            const RequestMessage& requestMessage) const;

            ResponseMessage handleDecryptedRequestByName(const ndn::Name& requestName,
                                                         const RequestMessage& requestMessage) const;

            ResponseMessage handleDecryptedRequestByName(const ndn::Name& requestName,
                                                         const ndn::Block& requestBlock) const;

            void OnRequestDecryptionSuccessCallbackV2(const ndn::Name& requesterIdentity,
                                                       const ndn::Name& serviceName,
                                                       const ndn::Name& requestId,
                                                       const ndn::Buffer& buffer);

            void OnRequestDecryptionErrorCallback(const ndn::Name& requesterIdentity,
                                                  const ndn::Name& serviceName,
                                                  const ndn::Name& requestId,
                                                  const std::string& error);
            
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
                const std::optional<ControllerVersion>& messageVersion) const;
            /** Enforce an installed Controller-signed revocation state at a
             * protected Provider transition while preserving the pre-status
             * bootstrap path. */
            #if defined(__GNUC__)
            __attribute__((noinline))
            #endif
            bool authorizeControllerTransition(
                const ndn::Name& serviceName,
                ProtectedTransition transition) const;
            void adoptControllerVersion(const ControllerVersion& version);
            /**
             * Evict per-service authorization material after an authenticated
             * ControllerVersion change.  Replay tombstones remain bounded and
             * are not removed merely because a newer policy arrived.
             */
            void invalidateControllerScopedCaches(
                const ndn::Name& serviceName,
                const ControllerVersion& version,
                bool abeGenerationChanged = true,
                const PolicyStatusData* status = nullptr,
                bool grantOnlyDkeyRefresh = false);
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


            bool replyFromIMS(const ndn::Interest &interest);
            void rememberPendingImsInterest(const ndn::Interest& interest);
            void insertDataIntoIMS(const ndn::Data& data);
            void insertDataIntoIMS(const ndn::Data& data,
                                   const ndn::time::milliseconds& freshness);
            void satisfyPendingImsInterestsLocked(const ndn::Data& insertedData);
            void satisfyPendingImsInterestsLocked();
            void pruneExpiredPendingImsInterestsLocked();

            void onPrefixRegisterFailure(const ndn::Name& prefix, const std::string& reason);

            void onInterest(const ndn::InterestFilter &, const ndn::Interest &interest);
            bool handleExecutionActivateInterest(const ndn::Interest& interest);
            void publishProviderReady(const ndn::Name& requesterIdentity,
                                      const ProviderReadyMessage& ready,
                                      const std::string& statusHandle,
                                      int attempt = 0);

            /** Stable process-incarnation fence exposed to opaque application
             * participants. It is an identity binding, not a secret. */
            std::string getProviderBootEpoch() const
            {
                return std::to_string(m_processStartedAtUs);
            }

            void serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data>& contentData, ndn::nacabe::SPtrVector<ndn::Data>& ckData);

            void PublishRequestAckMessageV2(const ndn::Name& requesterIdentity,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId,
                                            bool status,
                                            const std::string& msg,
                                            const ndn::Buffer& payload = ndn::Buffer(),
                                            const std::string& userToken = "",
                                            const std::string& providerToken = "",
                                            const RequestMessage* sourceRequest = nullptr,
                                            const AckDecision* ackDecision = nullptr);
    
            void onServiceSelectionMessage(const ndn::svs::SVSPubSub::SubscriptionData &subscription);
            void handleServiceSelectionMessage(const ndn::svs::SVSPubSub::SubscriptionData& subscription,
                                               bool checkFreshness);
            void prefetchSelectionMessageV2(const ndn::Name& requesterIdentity,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId);

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

            void OnServiceSelectionMessageDecryptionSuccessCallbackV2(const ndn::Name& requesterName,
                                                                          const ndn::Name& providerName,
                                                                          const ndn::Name& serviceName,
                                                                          const ndn::Name& msgId,
                                                                          const ndn::Buffer& buffer);

            void OnServiceSelectionMessageDecryptionErrorCallback(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& msgId,
                const std::string& reason);
            
            // Register NDNSF Messages in the ndn-svs
            void registerNDNSFMessages();
            // Scoped services may be added after init(). Keep their V2
            // request subscription aligned with the service table so a
            // dynamically served collaboration can receive Requests.
            void registerRequestSubscription(const ndn::Name& serviceName);

            // Register service info using ndnsd(). Generic dynamic providers may use the no-op
            // default; legacy generated providers may still override it.
            virtual void registerServiceInfo();

            bool isFresh(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

        protected:
            void
            onMissingData(const std::vector<ndn::svs::MissingDataInfo> &);

        protected:
            friend struct ServiceProviderTestAccess;

            static ndn::Name extractLargeDataProducerPrefix(
                const ndn::Name& dataName);

            // ---- spec182 scoped registration ----
            // Each addScoped* registration allocates a non-zero generation and
            // a RegistrationState shared by the Core entry, pending-request
            // binds and collaboration-selection binds.  The public
            // ServiceRegistration handle is the only RAII that keeps a state
            // alive; close() flips the atomic closed flag and schedules
            // Core-entry cleanup on the Face event thread, and a closed state
            // is detached (never removed) only when it still owns its map
            // entry -- so an old handle can never delete a newer generation.
            struct RegistrationState
            {
                RegistrationState(ndn::Name name, uint64_t gen)
                    : serviceName(std::move(name)), generation(gen) {}

                ndn::Name serviceName;
                uint64_t generation = 0;
                std::atomic<bool> closed{false};
            };

            struct RegistrationControl
            {
                // Serializes ownership hand-off between the Face event thread
                // (registration and cleanup) and the Provider destructor.
                std::mutex mutex;
                ServiceProvider* owner = nullptr;
            };

        public:
            // Public so the opaque handle can be held by value and its
            // methods called; construction stays private to ServiceProvider.
            class ServiceRegistration
            {
            public:
                ServiceRegistration() noexcept = default;
                ServiceRegistration(ServiceRegistration&& other) noexcept;
                ServiceRegistration& operator=(ServiceRegistration&& other) noexcept;
                ServiceRegistration(const ServiceRegistration&) = delete;
                ServiceRegistration& operator=(const ServiceRegistration&) = delete;
                ~ServiceRegistration();

                void close() noexcept;
                bool closed() const noexcept;
                bool valid() const noexcept;
                uint64_t generation() const noexcept;
                ndn::Name serviceName() const;

            private:
                friend class ServiceProvider;
                ServiceRegistration(std::shared_ptr<RegistrationState> state,
                                    std::weak_ptr<RegistrationControl> control);

                std::shared_ptr<RegistrationState> m_state;
                std::weak_ptr<RegistrationControl> m_control;
            };

        protected:
            struct RegisteredService
            {
                AckStrategyHandler ackHandler;
                RequestHandler requestHandler;
                RequestHandler targetedRequestHandler;
                StreamingHandler streamingHandler;
                ServiceMode mode = ServiceMode::Normal;
                bool selectionStatusQueryable = false;
                bool genericAdmissionLeaseRequired = false;
                GenericAdmissionLeaseValidator genericAdmissionLeaseValidator;
                // Non-null exactly for entries owned by an active or closed
                // scoped registration.  A closed state kept in the map means
                // cleanup has not been drained yet; legacy registration may
                // take the entry over by clearing the pointer.
                std::shared_ptr<RegistrationState> registrationState;
            };

            struct RegisteredCollaborationService
            {
                AckStrategyHandler ackHandler;
                CollaborationHandler handler;
                std::vector<CollaborationRole> allowedRoles;
                // Same ownership rules as RegisteredService::registrationState.
                std::shared_ptr<RegistrationState> registrationState;
            };

            // Face-serialized helpers for scoped registration.  These run on
            // the Face event thread (registration calls or posted cleanup
            // closures), never concurrently with each other; the destructor
            // additionally serializes against them with
            // m_registrationControl->mutex.
            uint64_t allocateRegistrationGeneration();
            // Legacy-registration guard: refuses (returns false) when the
            // service name in m_services is owned by a live scoped
            // registration, and clears a closed parked state (legacy
            // takeover) when the name is free.  Callers must hold
            // m_registrationControl->mutex.
            bool allowLegacyServiceTakeover(const ndn::Name& serviceName);
            // True when the m_services record already carries registration
            // content (scoped state, legacy handlers, stream sentinels, the
            // collaboration shell flag, or admission-lease settings).
            static bool serviceEntryBusy(const RegisteredService& entry);
            // Removes every map entry that parks a closed registration state
            // and appends the retired handler owners to the out-vectors, so
            // callers destroy them only after the control lock is released.
            // Callers must hold m_registrationControl->mutex.
            void drainClosedRegistrations(
                std::vector<RegisteredService>& retiredServices,
                std::vector<RegisteredCollaborationService>& retiredCollaborations);
            // Removes map entries still owned by the given closed state and
            // moves their handler owners into the out-parameters, so callers
            // destroy them only after releasing any held lock.
            void detachClosedRegistration(
                const std::shared_ptr<RegistrationState>& state,
                RegisteredService& retiredService,
                RegisteredCollaborationService& retiredCollaboration);
            void closeAllRegistrationStates();

            struct PendingEncryptedCollaborationData
            {
                ndn::Name dataName;
                ndn::Name requestId;
                ndn::Name producer;
                CollaborationDataMessage message;
            };

            struct CollaborationSubscription
            {
                ndn::Name requestId;
                ndn::Name requesterName;
                KeyScope keyScope;
                Topic topicPrefix;
                RequestMessage requestMessage;
                CollaborationAssignment assignment;
                std::function<void(const CollaborationData&)> onData;
                std::function<void(CollaborationContext&,
                                   const CollaborationData&)> onContextData;
                bool receiveFilterOnly = false;
            };

            struct TargetedProviderTokenState
            {
                ndn::Name requesterIdentity;
                ndn::Name serviceName;
                std::string userToken;
            };

            static ResponseMessage makeErrorResponse(const std::string& errorInfo);

            static AckDecision makeDefaultAckDecision();

            void schedulePendingRequestCleanup(const ndn::Name& pendingKey,
                                               ndn::time::milliseconds ttl = ndn::time::seconds(30),
                                               bool authoritative = false);

            struct CollaborationWorkFence
            {
                std::chrono::steady_clock::time_point deadline;
                std::function<bool()> current;
            };
            CollaborationWorkFence makeCollaborationWorkFence(
                const ndn::Name& requesterName, const ndn::Name& requestId,
                const ndn::Name& serviceName,
                std::optional<ControllerVersion> version,
                std::shared_ptr<RegistrationState> registrationState = nullptr);
            LargeDataFetchResult fetchAndDecryptLargeDataUntil(
                const ndn::Name& encryptedDataName, const std::string& serviceName,
                std::chrono::steady_clock::time_point deadline,
                std::function<bool()> current = {});

            void cleanupPendingRequestState(const ndn::Name& pendingKey,
                                            bool preserveReplayTombstone = false);
            // spec182: execution-side generation fence.  Under
            // m_pendingRequestMutex, binds pendingKey to the registration
            // state of the entry the dispatcher chose unless the request was
            // already bound to a different (earlier) generation.  Returns an
            // empty string when execution may proceed, else the refusal
            // reason.  Callers pass a null state only for legacy entries,
            // which always pass.
            std::string fencePendingRegistrationExecution(
                const ndn::Name& pendingKey,
                const std::shared_ptr<RegistrationState>& entryState);
            // spec182: inline (pool-0) execution fence.  Resolves the current
            // m_services entry for serviceName and refuses (publishing the
            // failure on the caller's behalf) when a scoped registration
            // retired or the pending binding names an earlier generation.
            // On success, *registrationState carries the state to forward to
            // finishRequestExecutionOnEventLoop (null for legacy entries).
            // Face-thread only.  Returns true when inline execution may
            // proceed.
            bool gateInlineRequestExecution(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& requestMessage,
                std::string selectionDigest,
                std::shared_ptr<RegistrationState>& registrationState);

            bool expirePendingRequestState(const ndn::Name& pendingKey);

            bool shouldSuppressAdaptiveAck(const ndn::Name& requesterIdentity,
                                           const ndn::Name& serviceName,
                                           const ndn::Name& requestId);
            void updateProviderRequestLifecycleState(
                const ndn::Name& requestId,
                const ndn::Name& serviceName,
                ProviderRequestLifecycleState state,
                const std::string& suppressionReason = "",
                const std::string& finalStatus = "");

            /** Attach streamed provider state to an existing pending request. */
            std::shared_ptr<StreamInvocationLifecycle>
            attachStreamLifecycle(const ndn::Name& pendingKey);

            std::shared_ptr<StreamInvocationLifecycle>
            getStreamLifecycle(const ndn::Name& pendingKey) const;
            void updateSelectionExecutionStatus(
                const std::string& selectionDigest,
                SelectionExecutionState state,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const std::string& message = "",
                const ndn::Name& responseName = ndn::Name());
            bool replySelectionExecutionStatus(const ndn::Interest& interest);
            static std::string encodeSelectionExecutionStatus(
                const SelectionExecutionStatus& status);
            static SelectionExecutionStatus makeUnknownSelectionExecutionStatus(
                const ndn::Name& providerName,
                const std::string& selectionDigest);
            // registrationState (spec182): non-null when the ack decision was
            // taken against a scoped registration; the worker fences against
            // it and forwards it to finishAckDecisionOnEventLoop.
            bool dispatchAckDecisionAsync(
                const ndn::Name& requesterIdentity,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                AckStrategyHandler ackHandler,
                std::shared_ptr<RegistrationState> registrationState = nullptr);
            // registrationState (spec182): the scoped registration the
            // pending acceptance was bound to; when it closed or was
            // superseded before the decision finished, the positive decision
            // degrades to the negative path (no pending store, no positive
            // ACK).  Null for legacy acceptances.
            void finishAckDecisionOnEventLoop(
                const ndn::Name& requesterIdentity,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                AckDecision decision,
                std::shared_ptr<RegistrationState> registrationState = nullptr);
            GenericLeaseValidationResult validateGenericAdmissionLeaseForSelection(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& requestMessage,
                const ServiceSelectionMessage& selectionMessage,
                const ndn::Buffer& assignmentPayload);
            void finishDecodedRequestOnEventLoop(
                const ndn::Name& requesterIdentity,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage);
            bool finishTargetedRequestOnEventLoop(
                const ndn::Name& requesterIdentity,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage);
            bool consumeTargetedProviderToken(const ndn::Name& requesterIdentity,
                                              const ndn::Name& serviceName,
                                              const RequestMessage& requestMessage,
                                              std::string& error) const;
            void attachTargetedTokenBatch(const ndn::Name& requesterIdentity,
                                          const ndn::Name& serviceName,
                                          const RequestMessage& requestMessage,
                                          ResponseMessage& response) const;
            // registrationStateOut (spec182): when non-null and the dispatch
            // resolved a scoped registration entry, receives the entry's
            // registration state (legacy entries leave it null).  Inline
            // fallback callers use it to fence a pool-0 dispatch.
            bool dispatchRequestExecutionAsync(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                std::string selectionDigest = "",
                std::shared_ptr<RegistrationState>* registrationStateOut = nullptr);
            bool initializeStreamPublisher(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& requestMessage,
                const ServiceSelectionMessage& selectionMessage,
                const std::string& selectionDigest);
            void publishStreamEventOnFaceEventLoop(
                PublishedStreamEvent event,
                uint64_t retentionMs);
            void onStreamEvent(const ndn::svs::SVSPubSub::SubscriptionData& subscription);
            bool dispatchCollaborationExecutionAsync(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                CollaborationAssignment assignment,
                std::string selectionDigest = "");
            // registrationState (spec182): the scoped collaboration
            // registration the request was accepted against; bound under
            // m_collaborationMutex next to m_collaborationServiceNamesByRequest.
            void prepareCollaborationAssignmentAsync(
                const ndn::Name& requesterName,
                const ndn::Name& requestId,
                CollaborationAssignment assignment,
                std::function<void(bool, std::string,
                                   CollaborationAssignment)> onReady,
                std::shared_ptr<RegistrationState> registrationState = nullptr);
            // registrationState (spec182): non-null when the executing
            // request was bound to a scoped registration; a positive response
            // whose registration closed meanwhile degrades to an error (the
            // retired registration must never commit work in its name).
            void finishRequestExecutionOnEventLoop(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& requestMessage,
                ResponseMessage response,
                std::string selectionDigest = "",
                std::shared_ptr<RegistrationState> registrationState = nullptr);
            void fetchRequestScopedInputAndDispatch(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                const ServiceSelectionMessage& selectionMessage,
                const ndn::Buffer& assignmentPayload,
                const std::string& selectionDigest);
            void publishExecutionFailureOnEventLoop(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& requestMessage,
                const std::string& error,
                std::string selectionDigest = "");
            void completeCollaborationRoleOnEventLoop(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                std::string selectionDigest = "");
            void publishCollaborationData(const ndn::Name& requesterName,
                                          const ndn::Name& requestId,
                                          const std::string& producerRole,
                                          const std::string& keyScope,
                                          const ndn::Name& topic,
                                          const ndn::Buffer& payload);
            ndn::Name publishCollaborationLargeData(
                const ndn::Name& requesterName,
                const ndn::Name& requestId,
                const std::string& producerRole,
                const std::string& keyScope,
                const ndn::Name& topic,
                const ndn::Buffer& payload,
                size_t maxSegmentSize,
                int freshnessMs);
            ndn::Name publishCollaborationLargeData(
                const ndn::Name& requesterName,
                const ndn::Name& requestId,
                const std::string& producerRole,
                const std::string& keyScope,
                const ndn::Name& topic,
                const ndn::Buffer& payload,
                size_t maxSegmentSize,
                int freshnessMs,
                CollaborationTransferMetrics* metrics);
            ndn::Name publishCollaborationLargeDataNamed(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const ndn::Name& dataName,
                const ndn::Buffer& payload,
                size_t maxSegmentSize,
                int freshnessMs);
            ndn::Name publishCollaborationLargeDataNamed(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const ndn::Name& dataName,
                const ndn::Buffer& payload,
                size_t maxSegmentSize,
                int freshnessMs,
                CollaborationTransferMetrics* metrics);
            std::optional<ndn::Buffer> fetchCollaborationLargeData(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const ndn::Name& dataName,
                int timeoutMs,
                std::size_t expectedSegments = 0);
            std::optional<ndn::Buffer> fetchCollaborationLargeData(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const ndn::Name& dataName,
                int timeoutMs,
                std::size_t expectedSegments,
                CollaborationTransferMetrics* metrics);
            bool publishCollaborationDataV1Segments(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const std::vector<std::pair<ndn::Name, ndn::Buffer>>& segments,
                int freshnessMs);
            std::optional<std::vector<ndn::Buffer>> fetchCollaborationDataV1Segments(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const ndn::Name& producerPrefix,
                std::uint64_t operationIndex,
                const std::string& producerRank,
                const std::string& tensorDigest,
                std::size_t expectedSegments,
                std::size_t maxSegments,
                int timeoutMs,
                std::function<std::size_t(const ndn::Buffer&)>
                    segmentCountDecoder = {},
                DataV1SegmentNameFilter nameFilter = {});
            bool publishCollaborationSignedExactData(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
                int freshnessMs);
            bool publishCollaborationSignedExactData(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
                int freshnessMs,
                CollaborationTransferMetrics* metrics);
            std::optional<ndn::Buffer> fetchCollaborationSignedExactData(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const ndn::Name& dataName,
                const ndn::Name& expectedProducer,
                int timeoutMs,
                std::function<bool()> shouldCancel = {});
            std::optional<ndn::Buffer> fetchCollaborationSignedExactData(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const ndn::Name& dataName,
                const ndn::Name& expectedProducer,
                int timeoutMs,
                std::function<bool()> shouldCancel,
                CollaborationTransferMetrics* metrics);
            void publishCollaborationFinalResponse(
                const ndn::Name& requesterName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& requestMessage,
                const ndn::Buffer& payload,
                const std::string& selectionDigest);
            void onCollaborationDataMessage(
                const ndn::svs::SVSPubSub::SubscriptionData& subscription);
            void deliverCollaborationData(const CollaborationData& data);
            void addCollaborationSubscription(
                const ndn::Name& requestId,
                KeyScope keyScope,
                Topic topicPrefix,
                std::function<void(const CollaborationData&)> onData);
            void addCollaborationSubscription(
                const ndn::Name& requesterName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                CollaborationAssignment assignment,
                KeyScope keyScope,
                Topic topicPrefix,
                std::function<void(CollaborationContext&,
                                   const CollaborationData&)> onData);
            void addCollaborationReceiveFilter(const ndn::Name& requestId,
                                                KeyScope keyScope,
                                                Topic topicPrefix);
            void decryptCollaborationDataOrQueue(
                const ndn::Name& dataName,
                const ndn::Name& requestId,
                const ndn::Name& producer,
                const CollaborationDataMessage& message);
            bool maybeFetchCollaborationScopeKey(
                const ndn::Name& requestId,
                const KeyScope& keyScope);
            std::vector<CollaborationData> waitForCollaborationData(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const ndn::Name& topicPrefix,
                size_t minCount,
                int timeoutMs);
            static CollaborationAssignment parseCollaborationAssignment(
                const ndn::Name& serviceName,
                const ndn::Buffer& payload);
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
            ndn::Name identity;
            ndn::KeyChain m_keyChain;
            ndn::KeyChain* m_testSigningKeyChain = nullptr;
            std::vector<std::shared_ptr<ndn::ScopedRegisteredPrefixHandle>> m_contentRegistrations;
            std::shared_ptr<ndn::svs::SVSPubSub> m_svsps;
            LocalPublicationHandler m_localPublicationHandler;
            mutable std::mutex m_streamPublicationInterceptorMutex;
            StreamPublicationInterceptorForTest m_streamPublicationInterceptorForTest;
            StreamRetentionInterceptorForTest m_streamRetentionInterceptorForTest;
            StreamRetentionExpiryObserverForTest m_streamRetentionExpiryObserverForTest;
            mutable std::mutex m_collaborationPublicationInterceptorMutex;
            CollaborationPublicationInterceptorForTest m_collaborationPublicationInterceptorForTest;
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
            NetworkTelemetryStore m_networkTelemetry;
            ndn::nacabe::CacheProducer nacProducer;
            // LocalMock may sign with a fixture-owned in-memory KeyChain.
            // NAC-ABE Producer stores its KeyChain by reference, so changing
            // only the direct signing pointer is insufficient.
            std::unique_ptr<ndn::nacabe::CacheProducer> m_testNacProducer;
            ndn::security::SigningInfo m_signingInfo;
            // Environment-driven tracing must be resolved before the native
            // Python wrapper starts the Face loop; Python does not call the
            // C++ setter used by the standalone examples.
            bool m_timelineTrace = timelineTraceEnvEnabled();
            size_t m_currentPolicyEpoch = 0;
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
            // Process-incarnation fence used by deployment capability and
            // readiness contracts. It is never an authorization secret.
            const uint64_t m_processStartedAtUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            size_t m_requiredKeyEpoch = 0;
            uint64_t m_policyGracePeriodMs = 0;
            HybridMessageCrypto m_hybridMessageCrypto;
            HybridCryptoCounters m_hybridCryptoCounters;
            SerializedWorkerQueue m_cryptoProduceQueue{"ServiceProvider NAC-ABE produce"};
            BoundedWorkerPool m_handlerPool{"ServiceProvider application handlers"};
            BoundedWorkerPool m_ackPool{"ServiceProvider ACK handlers"};
            // Shared with queued Face callbacks; inspect before dereferencing this.
            std::shared_ptr<std::atomic<bool>> m_fetchStopping =
                std::make_shared<std::atomic<bool>>(false);
            BoundedWorkerPool m_fetchPool{"ServiceProvider collaboration fetches"};

            // ChanllengeID->(Token->RequestNameWithoutRequestID)
            std::map<ndn::Name,std::pair<std::string, ndn::Name>> chanllengeRecords;
            // RequestPrefix is a Request Name Without RequestID
            std::set<ndn::Name> authorizedRequestPrefixSet;
            // Requests that are authorized request -> requestPrefix
            std::map<ndn::Name,ndn::Name> unauthorizedRequestMap;

            /*
                pending requests waiting for Service Selection Message;
                (/<requesterName>/<serviceName>/<requestID> -> RequestMessage)
            */
            std::map<ndn::Name,std::shared_ptr<RequestMessage>> pendingRequests;
            // spec182: registration-generation binding per pending request.
            // Mirrors pendingRequests (same key space, same lock
            // m_pendingRequestMutex, same lifetime); non-null only when the
            // request was accepted against a scoped registration (legacy
            // acceptances leave no binding).
            std::map<ndn::Name, std::shared_ptr<RegistrationState>>
                m_pendingRegistrationStates;
            std::map<ndn::Name,std::string> pendingProviderTokens;
            // Keyed by the existing requester/service/request-id pending key.
            // Stream attachment never creates a second request identity.
            std::map<ndn::Name, std::shared_ptr<StreamInvocationLifecycle>>
                m_streamLifecycles;
            std::map<ndn::Name, std::shared_ptr<StreamEventPublisher>>
                m_streamPublishers;
            std::map<ndn::Name, StreamBinding> m_streamBindings;
            std::map<ndn::Name, ReservationLease> pendingReservationLeases;
            std::set<ndn::Name> m_recentProviderRequests;
            std::set<ndn::Name> m_selectedProviderRequests;
            std::set<ndn::Name> m_selectionDecryptsInFlight;
            struct R1AcceptedSelectionDecision
            {
                std::string decisionDigest;
                std::string providerTokenHash;
                std::string decision;
                ndn::Buffer receiptWire;
                uint64_t retainUntilMs = 0;
            };
            // First authenticated decision for a reservation is immutable.
            // Retaining the token proof permits exact duplicate decisions to
            // be acknowledged after their pending request has been consumed.
            std::map<std::string, R1AcceptedSelectionDecision>
                m_r1AcceptedSelectionDecisions;
            std::map<ndn::Name, std::string> m_pendingRequestTokenHashes;
            std::map<ndn::Name, std::string> m_selectedProviderTokenHashes;
            std::set<std::string> m_recentProviderRequestTokenHashes;
            std::set<std::string> m_consumedProviderTokenHashes;
            struct RequestScopedInvocationState
            {
                RequestKeyBundle keys;
                RequestSecurityBinding binding;
            };
            // One state entry is created only after a Selection envelope is
            // authenticated and consumed.  It is retained until the
            // terminal Response is published so the response key cannot be
            // reconstructed from a service-wide ABE key.
            std::map<ndn::Name, RequestScopedInvocationState>
                m_requestScopedInvocations;
            NonceRegistry m_requestScopedNonceRegistry;
            struct PreparedDeploymentExecution
            {
                ndn::Name requesterName;
                ndn::Name providerName;
                ndn::Name serviceName;
                ndn::Name requestId;
                RequestMessage request;
                DeploymentPlan plan;
                ProviderReadyMessage ready;
                std::string selectionDigest;
                std::string activationDigest;
                bool activated = false;
            };
            DeploymentPrepareHandler m_deploymentPrepareHandler;
            ProviderReadyPublisher m_providerReadyPublisher;
            std::map<ndn::Name, R1SelectionDecisionHandler>
                m_r1SelectionDecisionHandlers;
            std::shared_ptr<GenericSelectionTxnStore>
                m_genericSelectionTxnStore;
            std::map<ndn::Name, std::shared_ptr<OpaqueSelectionParticipant>>
                m_opaqueSelectionParticipants;
            std::map<ndn::Name, R1ReservationTerminalHandler>
                m_r1ReservationTerminalHandlers;
            std::map<ndn::Name, std::string> m_r1ReservationByRequest;
            std::map<std::string, PreparedDeploymentExecution> m_preparedDeployments;
            mutable std::map<std::string, TargetedProviderTokenState>
                m_targetedProviderTokens;
            mutable std::set<std::string> m_consumedTargetedProviderTokenHashes;
            mutable std::mutex m_pendingRequestMutex;
            std::map<ndn::Name, RegisteredCollaborationService> m_collaborationServices;
            std::map<ndn::Name, std::vector<CollaborationData>> m_collaborationDataByRequest;
            std::map<ndn::Name, std::map<KeyScope, ndn::Buffer>> m_collaborationScopeKeysByRequest;
            std::map<ndn::Name, std::map<KeyScope, ndn::Name>> m_collaborationScopeKeyDataNamesByRequest;
            std::map<ndn::Name, ndn::Name> m_collaborationServiceNamesByRequest;
            // spec182: registration-generation binding per collaboration
            // request.  Mirrors m_collaborationServiceNamesByRequest (same
            // requestId key space, same lock m_collaborationMutex, same
            // lifetime); non-null only when the collaboration request was
            // accepted against a scoped collaboration registration.
            std::map<ndn::Name, std::shared_ptr<RegistrationState>>
                m_collaborationRegistrationStates;
            std::set<std::string> m_collaborationScopeKeyFetchesInFlight;
            std::map<ndn::Name, std::vector<PendingEncryptedCollaborationData>> m_pendingEncryptedCollaborationData;
            std::map<std::string, ndn::Buffer> m_collaborationArtifacts;
            std::vector<CollaborationSubscription> m_collaborationSubscriptions;
            std::mutex m_collaborationMutex;
            std::condition_variable m_collaborationCv;
            std::atomic<uint64_t> m_collaborationSequence{0};
            std::atomic<size_t> m_selectedOutstandingRequests{0};
            size_t m_cleanupInvocationCount = 0;
            size_t m_tokenConsumeCount = 0;
            ndn::time::milliseconds m_pendingRequestTimeoutGrace{1000};
            bool m_performanceMode = false;
            bool m_useTokens = true;
            bool m_adaptiveAckAdmission = false;
            size_t m_providerAckMaxPending = 0;
            ndn::time::milliseconds m_providerAckMaxEventLoopLag{0};
            ndn::time::milliseconds m_providerAckMaxSelectionLag{0};
            mutable std::mutex m_pendingCleanupDeadlineMutex;
            std::map<ndn::Name, std::chrono::steady_clock::time_point>
                m_pendingCleanupDeadlines;
            std::map<ndn::Name, uint64_t> m_pendingCleanupExpiryUnixMs;
            std::set<ndn::Name> m_authoritativePendingCleanupDeadlines;
            std::map<ndn::Name, ProviderRequestLifecycleStatus>
                m_providerRequestLifecycleStatuses;
            std::map<std::string, SelectionExecutionStatus>
                m_selectionExecutionStatuses;
            // Selection operation status is updated by collaboration worker
            // threads and may be queried from the Face thread concurrently.
            // Keep map/vector lifetime and snapshot reads under one lock.
            mutable std::mutex m_selectionExecutionStatusMutex;
            ProviderRequestLifecycleCallback m_providerRequestLifecycleCallback;
            std::map<std::string, uint64_t> m_providerAdmissionCounters;
            ProviderAdmissionLeaseTable m_genericAdmissionLeases;

            ndn::random::RandomNumberEngine random;

            ndn::InMemoryStorageFifo m_IMS;
            std::mutex _cache_mutex;
            struct PendingImsInterest
            {
                ndn::Interest interest;
                ndn::time::steady_clock::time_point requestedAt;
                ndn::time::steady_clock::time_point expiresAt;
            };
            std::map<ndn::Name, std::deque<PendingImsInterest>> m_pendingImsInterestsByName;
            std::vector<PendingImsInterest> m_pendingPrefixImsInterests;
            std::deque<ndn::Name> m_pendingImsInsertionOrder;
            size_t m_pendingImsInterestCount = 0;

            OptionalServiceDiscovery m_ServiceDiscovery;
            std::map<std::string, std::string> m_ndnsdMeta;
            mutable std::mutex m_ndnsdMetaMutex;
            std::unique_ptr<ndn::Scheduler> m_ndnsdScheduler;
            ndn::scheduler::ScopedEventId m_ndnsdHeartbeatEvent;
            int m_ndnsdHeartbeatIntervalSeconds = 0;

            ServiceAuthorizationTable m_authorizations;

            ConfigManager m_configManager;

            // SVS may replay an older publication when a later sequence is
            // synchronized. Freshness binds producer session and each
            // publication name's highest accepted sequence.  The sequence
            // frontier is per name because independent valid publications can
            // arrive out of order on separate face/event-loop paths; a global
            // frontier would discard an unseen lower-sequence publication.
            std::map<ndn::Name, std::pair<int, ndn::svs::SeqNo>> m_sessionIDMap;
            std::map<ndn::Name, std::map<ndn::Name, ndn::svs::SeqNo>>
                m_publicationSeqMap;

            std::mutex svs_mutex;

            // spec182 scoped registration bookkeeping.  All registration-path
            // code runs on the Face event thread, so m_registrationGeneration
            // needs no extra lock; m_registrationControl->mutex serializes
            // only against the Provider destructor.  m_registrationControl is
            // null only if construction never completed.
            std::shared_ptr<RegistrationControl> m_registrationControl;
            uint64_t m_registrationGeneration = 0;

            std::map<ServiceKey, RegisteredService> m_services;
    };
}

#endif
