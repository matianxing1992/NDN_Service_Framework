#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_USER_HPP

#include "common.hpp"
#include "ServiceStub.hpp"
#include "utils.hpp"

#include "BloomFilter.hpp"
#include "UserPermissionTable.hpp"
#include "NDNSFMessages.hpp"
#include "ConfigManager.hpp"
#include "HybridMessageCrypto.hpp"
#include "TimelineTrace.hpp"

#include <functional>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>


namespace ndn_service_framework{

    struct AckInfo{
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name functionName;
        ndn::Name requestID;
    };

    struct AckSelectionCandidate
    {
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestId;
        ndn_service_framework::RequestAckMessage ack;
    };

    using ProviderId = ndn::Name;
    using ServiceName = ndn::Name;
    using RequestId = ndn::Name;
    using RequestPayload = ndn::Buffer;
    using ResponsePayload = ndn::Buffer;
    using AckCandidate = AckSelectionCandidate;

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
        extern const std::shared_ptr<const AckSelectionPolicy> LoadBalancing;
        extern const std::shared_ptr<const AckSelectionPolicy> AllResponders;
    }

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
                AllResponders,
                CustomSelectionStrategy,
            };

            using ResponseHandler =
                std::function<void(const ndn_service_framework::ResponseMessage&)>;

            using TimeoutHandler =
                std::function<void(const ndn::Name&)>;

            enum class RequestLifecycleState
            {
                QUEUED_LOCAL,
                ADMISSION_DELAYED,
                ADMITTED,
                REQUEST_PUBLISHED,
                ACK_MATCHED,
                PROVIDER_SELECTED,
                COORDINATION_PUBLISHED,
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
                uint64_t coordinationPublishTimestampUs = 0;
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

            struct LocalMockTag
            {
            };

            ServiceUser(ndn::Face& face,ndn::Name group_prefix, ndn::security::Certificate identityCert,ndn::security::Certificate attrAuthorityCertificate,std::string trustSchemaPath);
            ServiceUser(LocalMockTag,
                        ndn::Face& face,
                        ndn::Name group_prefix,
                        ndn::security::Certificate identityCert,
                        ndn::security::Certificate attrAuthorityCertificate,
                        std::string trustSchemaPath);

            virtual ~ServiceUser();
            // void addServiceStub(ServiceStub serviceStub);

            void init();

            ndn::Name getName();

            void fetchPermissionsFromController(const ndn::Name& controllerPrefix);
            void applyPermissionResponse(const PermissionResponse& response);
            static bool handlePermissionResponseData(const ndn::Data& data,
                                                     const ndn::Name& identity,
                                                     ndn::KeyChain& keyChain,
                                                     UserPermissionTable& permissionTable);
            void setRequestPublisher(RequestPublisher publisher);
            void setRequestLifecycleCallback(RequestLifecycleCallback callback);
            void setAdmissionControlWarningHandler(AdmissionControlWarningHandler handler);
            void setAdmissionControlRejectHandler(AdmissionControlRejectHandler handler);
            std::optional<RequestLifecycleStatus>
            getRequestStatus(const ndn::Name& requestId) const;
            std::vector<RequestLifecycleStatus> getActiveRequestStatuses() const;
            static const char* requestLifecycleStateToString(RequestLifecycleState state);
            size_t getPendingCallCount() const;
            void setPendingCallTimeoutGrace(ndn::time::milliseconds grace);
            void setPerformanceMode(bool enabled);
            void setHandlerThreads(size_t n);
            size_t getHandlerThreads() const;
            size_t getHandlerQueueDepth() const;
            void setAckProcessingThreads(size_t n);
            size_t getAckProcessingThreads() const;
            size_t getAckProcessingQueueDepth() const;
            void setUseTokens(bool enabled);
            bool getUseTokens() const;
            void setUseHybridMessageCrypto(bool enabled);
            bool getUseHybridMessageCrypto() const;
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
                bool enabled = false;
                size_t minWindow = 1;
                size_t maxWindow = 512;
                size_t initialWindow = 16;
                size_t hardInflightLimit = 512;
                size_t aiStep = 4;
                double mdFactor = 0.85;
                double severeMdFactor = 0.75;
                int controlIntervalMs = 500;
                int targetLatencyMs = 350;
                int hardTargetLatencyMs = 500;
                size_t softQueueLimit = 32;
                size_t hardQueueLimit = 128;
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

            void PublishRequest(const std::vector<ndn::Name>& serviceProviderNames,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &payload, const size_t& strategy=ndn_service_framework::tlv::FirstResponding);
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
                const ndn::time::milliseconds& freshness = ndn::DEFAULT_FRESHNESS_PERIOD);

            ndn::Name async_call(const PreparedServiceRequest& ctx,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const PreparedServiceRequest& ctx,
                                 const std::vector<ndn::Name>& providers,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& functionName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t strategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int ackTimeoutMs,
                                 AcksHandler onAcksHandler,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler);

            ndn::Name async_call(const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int ackTimeoutMs,
                                 AckCandidatesHandler onAcksHandler,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler);

            ndn::Name async_call(const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int ackTimeoutMs,
                                 AckCandidatesHandler onAcksHandler,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler,
                                 size_t requestStrategy = ndn_service_framework::tlv::FirstResponding);

            ndn::Name async_call(const std::vector<ndn::Name>& providers,
                                 const ndn::Name& serviceName,
                                 ndn_service_framework::RequestMessage requestMessage,
                                 int ackTimeoutMs,
                                 AckSelectionStrategy selectionStrategy,
                                 int timeoutMs,
                                 TimeoutHandler onTimeout,
                                 ResponseHandler onResponseHandler);

            ndn::Name AsyncCall(const ServiceName& service,
                                const RequestPayload& request,
                                int ackCollectionTimeMs,
                                std::shared_ptr<const AckSelectionPolicy> selectionPolicy,
                                int timeoutMs,
                                ResponseHandler onResponse,
                                TimeoutHandler onTimeout);

            template<typename RequestT, typename ResponseT>
            ndn::Name AsyncCall(const ServiceName& service,
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

                return AsyncCall(
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
            ndn::Name asyncCall(const std::vector<ndn::Name>& providers,
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

                return async_call(providers,
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
            ndn::Name asyncCall(const std::vector<ndn::Name>& providers,
                                const ndn::Name& serviceName,
                                const ndn::Name& functionName,
                                const RequestT& request,
                                std::function<void(const ResponseT&)> onResponse,
                                std::function<void()> onTimeout,
                                int timeoutMs,
                                size_t strategy = ndn_service_framework::tlv::FirstResponding)
            {
                return asyncCall<RequestT, ResponseT>(providers,
                                                      makeUnifiedServiceName(serviceName, functionName),
                                                      request,
                                                      std::move(onResponse),
                                                      std::move(onTimeout),
                                                      timeoutMs,
                                                      strategy);
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

            bool handleRequestAckByName(const ndn::Name& ackName,
                                        const ndn_service_framework::RequestAckMessage& ackMessage);

            bool handleRequestAckByName(const ndn::Name& ackName,
                                        const ndn::Block& ackBlock);
            void dispatchDecryptedResponseByName(const ndn::Name& responseName,
                                                 const ndn::Name& requestId,
                                                 const ndn::Buffer& buffer);
            void finishDecryptedResponseByName(const ndn::Name& responseName,
                                               const ndn::Name& requestId,
                                               ndn_service_framework::ResponseMessage responseMessage);
            void finishRequestAckOnEventLoop(const ndn::Name& providerName,
                                             const ndn::Name& ServiceName,
                                             const ndn::Name& FunctionName,
                                             const ndn::Name& requestID,
                                             ndn_service_framework::RequestAckMessage AckMessage);

            virtual void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            // ndnsd serviceinfo discovery callback
            void processNDNSDServiceInfoCallback(const ndnsd::discovery::Details& callback);

            void onPermissionResponseData(const ndn::Interest& interest,
                                          const ndn::Data& data);
            void onPermissionResponseTimeout(const ndn::Interest& interest);

            void OnRequestAck(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            void OnRequestAckDecryptionSuccessCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const ndn::Buffer &buffer) ;

            void OnRequestAckDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID, const std::string &error) ;

            void PublishServiceCoordinationMessage(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &requestID) ;
            void PublishServiceCoordinationMessageV2(const ndn::Name& providerName,
                                                     const ndn::Name& serviceName,
                                                     const ndn::Name& requestId);

            void OnResponseDecryptionErrorCallback(const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const std::string &);

            void OnPermissionTokenDecryptionSuccessCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum, const ndn::Buffer &buffer) ;

            void OnPermissionTokenDecryptionErrorCallback(const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum, const std::string &error) ;

            bool replyFromIMS(const ndn::Interest &interest);

            void onPrefixRegisterFailure(const ndn::Name& prefix, const std::string& reason);

            void onInterest(const ndn::InterestFilter &, const ndn::Interest &interest);

            void serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data>& contentData, ndn::nacabe::SPtrVector<ndn::Data>& ckData);

            void PublishMessage(const ndn::Name& messageName, const ndn::Name &messageNameWithoutPrefix, AbstractMessage& message);
            void publishHybridMessage(const ndn::Name& messageName,
                                      const ndn::Name& messageNameWithoutPrefix,
                                      AbstractMessage& message);
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

            struct PendingCall
            {
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
                uint64_t coordinationScheduledAtUs = 0;
                uint64_t coordinationPublishedAtUs = 0;
                uint64_t responseObservedAtUs = 0;
                uint64_t responseDecryptedAtUs = 0;
                uint64_t responseValidatedAtUs = 0;
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
                bool timedOut = false;
                bool timeoutGraceActive = false;
                size_t ackDecryptsInFlight = 0;
                size_t ackSelectionDeferrals = 0;
                size_t learnedAckProviderCountAtPublish = 0;
                std::vector<StoredAck> requestAcks;
                std::vector<StoredAck> customSelectedAcks;
                std::vector<ndn::Name> successfulAckProviders;
                std::vector<ndn::Name> coordinatedProviders;
                std::vector<ndn::Name> expectedResponseProviders;
                std::vector<ndn::Name> responseProviders;
                ndn::Name selectedProvider;
                std::map<std::string, std::string> providerTokens;
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

            static ndn::Name makeUnifiedServiceName(const ndn::Name& serviceName,
                                                    const ndn::Name& functionName);

            static std::string sanitizeLargeDataObjectId(const std::string& objectLabel);

            bool evaluateAckSelection(const ndn::Name& requestId);

            bool handleAckCollectionTimeout(const ndn::Name& requestId);

            bool selectLateAckAfterAckTimeout(PendingCall& pendingCall,
                                              const StoredAck& storedAck);

            bool evaluateCustomAckSelection(PendingCall& pendingCall);

            bool evaluateBuiltInAckSelection(PendingCall& pendingCall);
            bool hasReachedLatePipelineStage(const PendingCall& pendingCall) const;
            void recordObservedAckProvider(const ndn::Name& serviceName,
                                           const ndn::Name& providerName,
                                           uint64_t timestampUs);
            size_t getRecentAckProviderCount(const ndn::Name& serviceName,
                                             uint64_t nowUs);
            void scheduleRequestTimeout(const ndn::Name& requestId, int timeoutMs);
            void finalizeTimedOutPendingCall(const ndn::Name& requestId);
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

            static ndn::Name selectLoadBalancingProvider(const std::vector<ndn::Name>& providers);

            static const StoredAck* findStoredAck(
                const PendingCall& pendingCall,
                const ndn_service_framework::RequestAckMessage& ackMessage);

            ndn::Name startAsyncCallWithRequestId(const ndn::Name& requestId,
                                                  const std::vector<ndn::Name>& providers,
                                                  const ndn::Name& serviceName,
                                                  ndn_service_framework::RequestMessage requestMessage,
                                                  int timeoutMs,
                                                  TimeoutHandler onTimeout,
                                                  ResponseHandler onResponseHandler,
                                                  size_t strategy);

            void cleanupPendingCallState(const ndn::Name& requestId);
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
            
            ndn::nacabe::Consumer nacConsumer;
            //ndn::nacabe::Producer nacProducer;
            ndn::nacabe::CacheProducer nacProducer;
            ndn::security::SigningInfo m_signingInfo;
            bool m_useTokens = true;
            bool m_useHybridMessageCrypto = true;
            bool m_timelineTrace = false;
            HybridMessageCrypto m_hybridMessageCrypto;
            HybridCryptoCounters m_hybridCryptoCounters;
            SerializedWorkerQueue m_cryptoProduceQueue{"ServiceUser NAC-ABE produce"};
            BoundedWorkerPool m_handlerPool{"ServiceUser response callbacks"};
            BoundedWorkerPool m_ackProcessingPool{"ServiceUser ACK processing"};

            ndn::InMemoryStorageFifo m_IMS;
            std::mutex _cache_mutex;

            OptionalServiceDiscovery m_ServiceDiscovery;
            UserPermissionTable UPT;

            std::map<ndn::Name, size_t> m_strategyMap;

            // a map used for load balancing requestID 
            std::map<ndn::Name, std::vector<AckInfo>> m_AckInfoMap;

            ConfigManager m_configManager;

            std::map<ndn::Name, int> m_sessionIDMap;

            std::mutex svs_mutex;

            std::map<ndn::Name, PendingCall> m_pendingCalls;
            std::map<ndn::Name, std::map<std::string, uint64_t>>
                m_recentAckProvidersByService;
            std::map<ndn::Name, PendingCallTraceRecord> m_pendingCallTraceHistory;
            std::map<ndn::Name, RequestLifecycleStatus> m_requestLifecycleStatuses;
            RequestLifecycleCallback m_requestLifecycleCallback;
            AdmissionControlWarningHandler m_admissionControlWarningHandler;
            AdmissionControlRejectHandler m_admissionControlRejectHandler;
            RequestPublisher m_requestPublisher;
            ndn::time::milliseconds m_pendingCallTimeoutGrace{500};
            bool m_performanceMode = false;
            RuntimeDiagnostics m_runtimeDiagnostics;
            AdaptiveAdmissionOptions m_adaptiveAdmissionOptions;
            size_t m_adaptiveAdmissionWindow = 512;
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
            LoadBalancing;
        extern const std::shared_ptr<const ndn_service_framework::AckSelectionPolicy>
            AllResponders;
    }
}

#endif
