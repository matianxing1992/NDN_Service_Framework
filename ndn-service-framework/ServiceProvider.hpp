#ifndef NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP
#define NDN_SERVICE_FRAMEWORK_SERVICE_PROVIDER_HPP

#include "common.hpp"

#include "Service.hpp"
#include "utils.hpp"

#include "BloomFilter.hpp"
#include "UserPermissionTable.hpp"
#include "NDNSFMessages.hpp"
#include "ConfigManager.hpp"
#include "HybridMessageCrypto.hpp"
#include "TimelineTrace.hpp"

#include <functional>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <condition_variable>
#include <tuple>
#include <utility>
#include <vector>



namespace ndn_service_framework{

    using RequestPayload = ndn::Buffer;
    using ResponsePayload = ndn::Buffer;
    using ServiceName = ndn::Name;
    using CollaborationRole = std::string;
    using KeyScope = std::string;
    using Topic = ndn::Name;
    using SessionId = std::string;

    struct LargeDataFetchResult
    {
        bool success = false;
        std::vector<uint8_t> plaintext;
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
            };

            using AckStrategyHandler =
                std::function<AckDecision(const RequestMessage&)>;

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
                ndn::Buffer artifactPayload;
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
                ndn::Name publishLarge(KeyScope keyScope,
                                       Topic topic,
                                       const ndn::Buffer& payload,
                                       size_t maxSegmentSize = 7000,
                                       int freshnessMs = 60000);
                std::optional<ndn::Buffer> fetchLarge(const ndn::Name& dataName,
                                                      KeyScope keyScope,
                                                      int timeoutMs);
                void subscribe(KeyScope keyScope,
                               Topic topicPrefix,
                               std::function<void(const CollaborationData&)> onData);
                void subscribe(KeyScope keyScope,
                               Topic topicPrefix,
                               std::function<void(CollaborationContext&,
                                                  const CollaborationData&)> onData);
                std::optional<CollaborationData> waitOne(KeyScope keyScope,
                                                         Topic topicPrefix,
                                                         int timeoutMs);
                std::vector<CollaborationData> waitFor(KeyScope keyScope,
                                                       Topic topicPrefix,
                                                       size_t minCount,
                                                       int timeoutMs);
                void publishFinalResponse(const ndn::Buffer& payload);

            private:
                ServiceProvider& m_provider;
                ndn::Name m_requesterName;
                ndn::Name m_requestId;
                RequestMessage m_requestMessage;
                CollaborationAssignment m_assignment;
            };

            using CollaborationHandler =
                std::function<void(CollaborationContext& ctx,
                                   const RequestMessage& initialRequest)>;

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
            ServiceProvider(LocalMockTag,
                            ndn::Face& face,
                            ndn::Name group_prefix,
                            ndn::security::Certificate identityCert,
                            ndn::security::Certificate attrAuthorityCertificate,
                            std::string trustSchemaPath);
            virtual ~ServiceProvider();

            void init();

            ndn::Name getName();

            void fetchPermissionsFromController(const ndn::Name& controllerPrefix);
            void applyPermissionResponse(const PermissionResponse& response);
            size_t getCurrentPolicyEpoch() const;
            static bool handlePermissionResponseData(const ndn::Data& data,
                                                     const ndn::Name& identity,
                                                     ndn::KeyChain& keyChain,
                                                     UserPermissionTable& permissionTable);

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

            void UpdateUPTWithServiceMetaInfo(ndnsd::discovery::Details serviceDetails);
            void publishServiceInfo(const ndn::Name& serviceName,
                                    int serviceLifetimeSeconds,
                                    std::map<std::string, std::string> serviceMetaInfo = {});
            
            void OnRequest(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

            // After receiving service selection message, this function is called to consumeRequest.
            // Generic dynamic providers can rely on this safe default; legacy generated providers
            // may still override it for service-specific dispatch.
            virtual void ConsumeRequest(const ndn::Name& RequesterName,const ndn::Name& providerName,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID, RequestMessage& requestMessage);

            void addService(const ndn::Name& serviceName,
                            AckStrategyHandler ackHandler,
                            RequestHandler requestHandler);

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

            void setLegacyAckStrategyHandler(const ndn::Name& serviceName,
                                             LegacyAckStrategyHandler ackHandler);

            void setSelectionStatusQueryable(const ndn::Name& serviceName,
                                             bool enabled = true);

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
                                                       const ndn::Name& bloomFilterName,
                                                       const ndn::Name& requestId,
                                                       const ndn::Buffer& buffer);

            void OnRequestDecryptionSuccessCallback(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &bloomFilterName,  const ndn::Name &RequestID, const ndn::Buffer & buffer);
    
            // virtual void OnRequestDecryptionSuccessCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const ndn::Buffer &);
            void OnRequestDecryptionErrorCallback(const ndn::Name& requesterIdentity,const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& RequestID,const std::string &);
            
            // ndnsd serviceinfo discovery callback
            void processNDNSDServiceInfoCallback(const ndnsd::discovery::Details& callback);

            void onPermissionResponseData(const ndn::Interest& interest,
                                           const ndn::Data& data);
            void onPermissionResponseTimeout(const ndn::Interest& interest);
            void fetchPolicyManifestFromController(const ndn::Name& controllerPrefix);
            void onPolicyManifestData(const ndn::Interest& interest,
                                      const ndn::Data& data);
            void onPolicyManifestTimeout(const ndn::Interest& interest);
            bool isAcceptablePolicyEpoch(size_t messageEpoch) const;

            // void PublishResponse(const ndn::Name &requesterIdentity, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID, const ndn::Buffer& buffer);

            bool replyFromIMS(const ndn::Interest &interest);

            void onPrefixRegisterFailure(const ndn::Name& prefix, const std::string& reason);

            void onInterest(const ndn::InterestFilter &, const ndn::Interest &interest);

            void serveDataWithIMS(ndn::nacabe::SPtrVector<ndn::Data>& contentData, ndn::nacabe::SPtrVector<ndn::Data>& ckData);

            void PublishRequestAckMessage(const ndn::Name & requesterIdentity, const ndn::Name & ServiceName, const ndn::Name & FunctionName, const ndn::Name & RequestID, bool status, const std::string& msg, const ndn::Buffer& payload = ndn::Buffer(), const std::string& userToken = "", const std::string& providerToken = "");
            void PublishRequestAckMessageV2(const ndn::Name& requesterIdentity,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId,
                                            bool status,
                                            const std::string& msg,
                                            const ndn::Buffer& payload = ndn::Buffer(),
                                            const std::string& userToken = "",
                                            const std::string& providerToken = "");
    
            void onServiceSelectionMessage(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

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

            void OnServiceSelectionMessageDecryptionSuccessCallback(const ndn::Name &requesterName, const ndn::Name &providerName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID, const ndn::Buffer & buffer);
            void OnServiceSelectionMessageDecryptionSuccessCallbackV2(const ndn::Name& requesterName,
                                                                          const ndn::Name& providerName,
                                                                          const ndn::Name& serviceName,
                                                                          const ndn::Name& msgId,
                                                                          const ndn::Buffer& buffer);

            void OnServiceSelectionMessageDecryptionErrorCallback(const ndn::Name& requesterName,const ndn::Name &providerName, const ndn::Name& ServiceName,const ndn::Name& FunctionName, const ndn::Name& msgID,const std::string & reason);
            
            // Register NDNSF Messages in the ndn-svs
            void registerNDNSFMessages();

            // Register service info using ndnsd(). Generic dynamic providers may use the no-op
            // default; legacy generated providers may still override it.
            virtual void registerServiceInfo();

            bool isFresh(const ndn::svs::SVSPubSub::SubscriptionData &subscription);

        protected:
            void
            onMissingData(const std::vector<ndn::svs::MissingDataInfo> &);

        protected:
            struct RegisteredService
            {
                AckStrategyHandler ackHandler;
                RequestHandler requestHandler;
                RequestHandler targetedRequestHandler;
                ServiceMode mode = ServiceMode::Normal;
                bool selectionStatusQueryable = false;
            };

            struct RegisteredCollaborationService
            {
                AckStrategyHandler ackHandler;
                CollaborationHandler handler;
                std::vector<CollaborationRole> allowedRoles;
            };

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
            };

            struct ParsedRequestName
            {
                ndn::Name requesterIdentity;
                ndn::Name serviceName;
                ndn::Name requestId;
            };

            struct TargetedProviderTokenState
            {
                ndn::Name requesterIdentity;
                ndn::Name serviceName;
                std::string userToken;
            };

            static ResponseMessage makeErrorResponse(const std::string& errorInfo);

            static AckDecision makeDefaultAckDecision();

            static ndn::Name makeUnifiedServiceName(const ndn::Name& serviceName,
                                                    const ndn::Name& functionName);

            static std::optional<ParsedRequestName>
            parseRequestNameForUnifiedService(const ndn::Name& requestName);

            void schedulePendingRequestCleanup(const ndn::Name& pendingKey,
                                               ndn::time::milliseconds ttl = ndn::time::seconds(30));

            void cleanupPendingRequestState(const ndn::Name& pendingKey);

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
            bool dispatchAckDecisionAsync(
                const ndn::Name& requesterIdentity,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                AckStrategyHandler ackHandler);
            void finishAckDecisionOnEventLoop(
                const ndn::Name& requesterIdentity,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                AckDecision decision);
            void finishDecodedRequestOnEventLoop(
                const ndn::Name& requesterIdentity,
                const ndn::Name& serviceName,
                const ndn::Name& bloomFilterName,
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
                                          ResponseMessage& response) const;
            bool dispatchRequestExecutionAsync(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                std::string selectionDigest = "");
            bool dispatchCollaborationExecutionAsync(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                RequestMessage requestMessage,
                CollaborationAssignment assignment,
                std::string selectionDigest = "");
            void prepareCollaborationAssignmentAsync(
                const ndn::Name& requestId,
                CollaborationAssignment assignment,
                std::function<void(bool, std::string)> onReady);
            void finishRequestExecutionOnEventLoop(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& requestMessage,
                ResponseMessage response,
                std::string selectionDigest = "");
            void publishExecutionFailureOnEventLoop(
                const ndn::Name& requesterName,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& requestMessage,
                const std::string& error,
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
            std::optional<ndn::Buffer> fetchCollaborationLargeData(
                const ndn::Name& requestId,
                const std::string& keyScope,
                const ndn::Name& dataName,
                int timeoutMs);
            void publishCollaborationFinalResponse(
                const ndn::Name& requesterName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const RequestMessage& requestMessage,
                const ndn::Buffer& payload);
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
            ndn::security::Certificate attrAuthorityCertificate;
            ndn::nacabe::Consumer nacConsumer;
            //ndn::nacabe::Producer nacProducer;
            ndn::nacabe::CacheProducer nacProducer;
            ndn::security::SigningInfo m_signingInfo;
            bool m_timelineTrace = false;
            size_t m_currentPolicyEpoch = 0;
            size_t m_requiredKeyEpoch = 0;
            uint64_t m_policyGracePeriodMs = 0;
            HybridMessageCrypto m_hybridMessageCrypto;
            HybridCryptoCounters m_hybridCryptoCounters;
            SerializedWorkerQueue m_cryptoProduceQueue{"ServiceProvider NAC-ABE produce"};
            BoundedWorkerPool m_handlerPool{"ServiceProvider application handlers"};
            BoundedWorkerPool m_ackPool{"ServiceProvider ACK handlers"};

            // ChanllengeID->(Token->RequestNameWithoutRequestID)
            std::map<ndn::Name,std::pair<std::string, ndn::Name>> chanllengeRecords;
            // RequestPrefix is a Request Name Without RequestID
            std::set<ndn::Name> authorizedRequestPrefixSet;
            // Requests that are authorized request -> requestPrefix
            std::map<ndn::Name,ndn::Name> unauthorizedRequestMap;

            /*
                pending requests waiting for Service Selection Message;
                (/<requesterName>/<ServiceName>/<FunctionName>/<RequestID> -> RequestMessage)
            */
            std::map<ndn::Name,std::shared_ptr<RequestMessage>> pendingRequests;
            std::map<ndn::Name,std::string> pendingProviderTokens;
            std::set<ndn::Name> m_recentProviderRequests;
            std::set<ndn::Name> m_selectedProviderRequests;
            std::set<ndn::Name> m_selectionDecryptsInFlight;
            std::map<ndn::Name, std::string> m_pendingRequestTokenHashes;
            std::map<ndn::Name, std::string> m_selectedProviderTokenHashes;
            std::set<std::string> m_recentProviderRequestTokenHashes;
            std::set<std::string> m_consumedProviderTokenHashes;
            mutable std::map<std::string, TargetedProviderTokenState>
                m_targetedProviderTokens;
            mutable std::set<std::string> m_consumedTargetedProviderTokenHashes;
            mutable std::mutex m_pendingRequestMutex;
            std::map<ndn::Name, RegisteredCollaborationService> m_collaborationServices;
            std::map<ndn::Name, std::vector<CollaborationData>> m_collaborationDataByRequest;
            std::map<ndn::Name, std::map<KeyScope, ndn::Buffer>> m_collaborationScopeKeysByRequest;
            std::map<ndn::Name, std::map<KeyScope, ndn::Name>> m_collaborationScopeKeyDataNamesByRequest;
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
            std::map<ndn::Name, ProviderRequestLifecycleStatus>
                m_providerRequestLifecycleStatuses;
            std::map<std::string, SelectionExecutionStatus>
                m_selectionExecutionStatuses;
            ProviderRequestLifecycleCallback m_providerRequestLifecycleCallback;
            std::map<std::string, uint64_t> m_providerAdmissionCounters;

            ndn::random::RandomNumberEngine random;

            ndn::InMemoryStorageFifo m_IMS;
            std::mutex _cache_mutex;

            OptionalServiceDiscovery m_ServiceDiscovery;

            UserPermissionTable UPT;

            ConfigManager m_configManager;

            std::map<ndn::Name, int> m_sessionIDMap;

            std::mutex svs_mutex;

            std::map<ServiceKey, RegisteredService> m_services;
    };
}

#endif
