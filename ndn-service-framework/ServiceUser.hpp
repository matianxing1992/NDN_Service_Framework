#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP

#include "common.hpp"
#include "utils.hpp"

#include "ServiceAuthorizationTable.hpp"
#include "NDNSFMessages.hpp"
#include "ConfigManager.hpp"
#include "HybridMessageCrypto.hpp"
#include "NetworkTelemetry.hpp"
#include "NegativeAckReason.hpp"
#include "TimelineTrace.hpp"
#include "Stream.hpp"
#include "StreamFacade.hpp"

#include <functional>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
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

    struct AckSelectionCandidate
    {
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestId;
        ndn_service_framework::RequestAckMessage ack;
        std::optional<ndn_service_framework::NetworkTelemetrySnapshot> telemetry;
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

    struct LargeDataPublishResult
    {
        bool success = false;
        ndn::Name encryptedDataName;
        std::string objectId;
        std::string errorMessage;
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

            /** Seed a receive key for a LocalMock ingress test. */
            void cacheHybridReceiveKeyForTest(const std::string& keyId,
                                              const std::string& epochId,
                                              const ndn::Buffer& key);

            /** Cache a pre-built Data packet for LocalMock fetcher tests. */
            void cacheDataForTest(
                const ndn::Data& data,
                ndn::time::milliseconds freshness = ndn::DEFAULT_FRESHNESS_PERIOD);

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
            void applyPermissionResponse(const PermissionResponse& response);
            size_t getCurrentPolicyEpoch() const;
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

            LargeDataPublishResult publishEncryptedLargeData(
                const PreparedServiceRequest& ctx,
                const std::vector<uint8_t>& plaintext,
                const std::string& objectLabel = "",
                ndn::time::milliseconds freshness = ndn::DEFAULT_FRESHNESS_PERIOD);

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
                                             RequestCapabilities());

            bool CommitCollaborationPlan(const RequestId& requestId,
                                         const std::string& ackClosedDigest,
                                         CollaborationPlan plan);

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
                                        const ndn_service_framework::RequestAckMessage& ackMessage);

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
                                             ndn_service_framework::RequestAckMessage AckMessage);

            virtual void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

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
            bool isAcceptablePolicyEpoch(size_t messageEpoch) const;

            void OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            void OnRequestAckDecryptionSuccessCallback(const ndn::Name& providerName,
                                                       const ndn::Name& serviceName,
                                                       const ndn::Name& requestID,
                                                       const ndn::Buffer& buffer);

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
            void
            onMissingData(const std::vector<ndn::svs::MissingDataInfo> &);

        protected:
            struct StoredAck
            {
                ndn::Name providerName;
                ndn::Name serviceName;
                ndn::Name requestId;
                ndn_service_framework::RequestAckMessage message;
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

            bool evaluateAckSelection(const ndn::Name& requestId);

            bool handleAckCollectionTimeout(const ndn::Name& requestId);

            bool closeDeferredCollaborationAcks(const ndn::Name& requestId,
                                                PendingCall& pendingCall);

            bool selectLateAckAfterAckTimeout(PendingCall& pendingCall,
                                              const StoredAck& storedAck);

            bool evaluateCustomAckSelection(PendingCall& pendingCall);

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

            ndn::Face& m_face;
            ndn::Scheduler m_scheduler;
            ndn::Name identity;
            ndn::KeyChain m_keyChain;
            std::shared_ptr<ndn::svs::SVSPubSub> m_svsps;
            std::shared_ptr<MessageValidator> validator;
            std::vector<std::string> m_serviceNames;

            //ndn::security::Validator nac_validator;
            ndn::ValidatorConfig nac_validator{m_face};
            ndn::security::Certificate identityCert;
            ndn::security::Certificate signingCert;
            
            ndn::nacabe::Consumer nacConsumer;
            //ndn::nacabe::Producer nacProducer;
            ndn::nacabe::CacheProducer nacProducer;
            ndn::security::SigningInfo m_signingInfo;
            bool m_useTokens = true;
            bool m_timelineTrace = false;
            size_t m_currentPolicyEpoch = 0;
            size_t m_requiredKeyEpoch = 0;
            uint64_t m_policyGracePeriodMs = 0;
            HybridMessageCrypto m_hybridMessageCrypto;
            HybridCryptoCounters m_hybridCryptoCounters;
            SerializedWorkerQueue m_cryptoProduceQueue{"ServiceUser NAC-ABE produce"};
            BoundedWorkerPool m_handlerPool{"ServiceUser response callbacks"};
            BoundedWorkerPool m_ackProcessingPool{"ServiceUser ACK processing"};

            ndn::InMemoryStorageFifo m_IMS;
            std::mutex _cache_mutex;

            OptionalServiceDiscovery m_ServiceDiscovery;
            ServiceAuthorizationTable m_authorizations;

            std::map<ndn::Name, size_t> m_strategyMap;

            // a map used for load balancing requestID 
            std::map<ndn::Name, std::vector<AckInfo>> m_AckInfoMap;

            ConfigManager m_configManager;

            std::map<ndn::Name, int> m_sessionIDMap;

            std::mutex svs_mutex;

            std::map<ndn::Name, PendingCall> m_pendingCalls;
            std::mutex m_targetedTokenPoolsMutex;
            std::map<std::string, std::deque<TargetedTokenPair>> m_targetedTokenPools;
            std::map<std::string, TargetedTokenPoolControl>
                m_targetedTokenPoolControls;
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
            RuntimeDiagnostics m_runtimeDiagnostics;
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
