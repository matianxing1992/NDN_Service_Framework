#ifndef NDN_SERVICE_FRAMEWORK_UTILS_HPP
#define NDN_SERVICE_FRAMEWORK_UTILS_HPP

#include <string>
#include <regex>
#include <tuple>
#include <optional>
#include <set>
#include <cstddef>
#include <memory>
#include <vector>

#include <boost/format.hpp>
#include <ndn-cxx/encoding/buffer-stream.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <common.hpp>
#include <NDNSFMessages.hpp>

namespace ndn_service_framework
{
    struct ControlQueryParameters {
        ndn::Name subject;
        ndn::Name service;
        uint64_t generation = 0;
        uint64_t epoch = 0;
    };
    ndn::Block encodeControlQueryParameters(const ControlQueryParameters& query);
    std::optional<ControlQueryParameters> decodeControlQueryParameters(const ndn::Block& parameters);
    struct ControlRpcName {
        ndn::Name producer;
        std::string operation;
        std::string callIdentityDigest;
        std::string queryNonce;
    };
    /** Complete query parameters remain in the Interest; reply echoes its final Name. */
    ndn::Name makeControlRpcName(const ndn::Name& producer, const std::string& operation,
                                const ndn::Block& parameters, const std::string& queryNonce);
    std::optional<ControlRpcName> parseControlRpcName(
        const ndn::Interest& interest, const ndn::Name& expectedProducer,
        const std::string& expectedOperation);
    std::string makeControlQueryNonce();
    std::optional<uint64_t> controlNonceExpiry(const ndn::Name& producer, const ndn::Interest& interest);
    /** Record commitment excludes its own Name; revision is the full signed plan digest. */
    ndn::Name makeDeploymentRecordName(const ndn::Name& producer,
        const ndn::Name& application, const std::string& deploymentId,
        const std::string& kind, const std::string& revisionDigest,
        uint64_t epoch, const std::string& payloadDigest);


    /** Canonical Controller policy-status name.  A bare service suffix is
     * retained as a bootstrap prefix; Controller replies use the versioned
     * form so a (service, generation, epoch) pair is directly addressable. */
    struct PolicyStatusName
    {
        ndn::Name serviceName;
        std::optional<ControllerVersion> version;
    };

    ndn::Name makePolicyStatusName(const ndn::Name& controllerPrefix,
                                   const ndn::Name& serviceName,
                                   const ControllerVersion& version);
    std::optional<PolicyStatusName>
    parsePolicyStatusName(const ndn::Name& controllerPrefix,
                          const ndn::Name& name);

    std::optional<PolicyStatusName> parsePolicyStatusName(
        const ndn::Name& controllerPrefix, const ndn::Interest& interest);

    struct RequestNameV2
    {
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    struct ResponseNameV2
    {
        ndn::Name providerName;
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    struct RequestAckNameV2
    {
        ndn::Name providerName;
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    struct ServiceSelectionNameV2
    {
        ndn::Name requesterName;
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    struct ServiceSelectionDecisionNameV2
    {
        ndn::Name requesterName;
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestId;
        uint64_t attempt = 0;
    };

    struct CompactServiceSelectionNameV2
    {
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    struct SelectionStatusQueryName
    {
        ndn::Name providerName;
        ndn::Name serviceName;
        std::string selectionDigest;
    };

    struct SecureSelectionStatusName
    {
        ndn::Name providerName;
        uint64_t version = 0;
        std::string statusHandle;
    };

    struct ProviderReadyName
    {
        ndn::Name requesterName;
        uint64_t version = 0;
        std::string controlHandle;
    };

    struct ExecutionActivateName
    {
        ndn::Name providerName;
        uint64_t version = 0;
        std::string controlHandle;
    };

    struct CollaborationDataName
    {
        ndn::Name producerName;
        ndn::Name requesterName;
        ndn::Name requestId;
        std::string keyScope;
        ndn::Name topic;
        uint64_t sequence = 0;
    };

    struct LargeDataReference
    {
        ndn::Name dataName;
        std::string objectType;
        std::string objectId;
        // Empty preserves the legacy service-wide MessageKey path.  The
        // request-scoped response path sets this to "request" so the User
        // resolves the object with the invocation's K_response instead of a
        // service-wide ABE key.
        std::string keyScope;
        size_t plaintextSize = 0;
        bool encrypted = true;
        std::string digest;
    };

    // C03 invocation names preserve the complete actor and service Names.
    // REQUEST: <requester>/NDNSF/REQUEST/<service...>/<requestId...>
    // ACK/RESPONSE: <provider>/NDNSF/<category>/<requester URI>/<service...>/<requestId...>
    // SELECTION: <requester>/NDNSF/SELECTION/<provider URI>/<service...>/<requestId...>
    // Embedded actor URIs use explicit CANONICAL rendering. Contextual readers
    // fix actor/service boundaries before accepting an invocation. Compact
    // helpers below decode historical records and are not publication paths.
    std::optional<ndn::Name> parseSvsProducerActor(const ndn::Name& node,
                                                   const std::string& role);
    ndn::Name makeRequestNameV2(const ndn::Name& requesterName,
                                const ndn::Name& serviceName,
                                const ndn::Name& requestId);
    ndn::Name makeRequestNameWithoutPrefixV2(const ndn::Name& serviceName,
                                             const ndn::Name& requestId);
    std::optional<RequestNameV2> parseRequestNameV2(const ndn::Name& requestName);
    std::optional<RequestNameV2> parseRequestNameV2(
        const ndn::Name& name, const ndn::Name& expectedRequester,
        const ndn::Name& expectedService);

    ndn::Name makeResponseNameV2(const ndn::Name& providerName,
                                 const ndn::Name& requesterName,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& requestId);
    ndn::Name makeResponseNameWithoutPrefixV2(const ndn::Name& requesterName,
                                              const ndn::Name& serviceName,
                                              const ndn::Name& requestId);
    // SegmentFetcher may append version/segment components to a response Data
    // name. Logical response matching and hybrid-message AAD use the
    // unsegmented V2 name.
    ndn::Name stripTrailingResponseSegments(const ndn::Name& responseName);
    std::optional<ResponseNameV2> parseResponseNameV2(const ndn::Name& responseName);
    std::optional<ResponseNameV2> parseResponseNameV2(
        const ndn::Name& name, const ndn::Name& expectedProvider,
        const ndn::Name& expectedService);

    ndn::Name makeRequestAckNameV2(const ndn::Name& providerName,
                                   const ndn::Name& requesterName,
                                   const ndn::Name& serviceName,
                                   const ndn::Name& requestId);
    ndn::Name makeRequestAckNameWithoutPrefixV2(const ndn::Name& requesterName,
                                                const ndn::Name& serviceName,
                                                const ndn::Name& requestId);
    std::optional<RequestAckNameV2> parseRequestAckNameV2(const ndn::Name& requestAckName);
    std::optional<RequestAckNameV2> parseRequestAckNameV2(
        const ndn::Name& name, const ndn::Name& expectedProvider,
        const ndn::Name& expectedService);

    ndn::Name makeServiceSelectionNameV2(const ndn::Name& requesterName,
                                            const ndn::Name& providerName,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId);
    ndn::Name makeServiceSelectionNameWithoutPrefixV2(const ndn::Name& providerName,
                                                         const ndn::Name& serviceName,
                                                         const ndn::Name& requestId);
    std::optional<ServiceSelectionNameV2>
    parseServiceSelectionNameV2(const ndn::Name& serviceSelectionName);
    std::optional<ServiceSelectionNameV2> parseServiceSelectionNameV2(
        const ndn::Name& name, const ndn::Name& expectedRequester,
        const ndn::Name& expectedService);
    ndn::Name makeServiceSelectionDecisionNameV2(const ndn::Name& requesterName,
                                                 const ndn::Name& providerName,
                                                 const ndn::Name& serviceName,
                                                 const ndn::Name& requestId,
                                                 uint64_t attempt);
    ndn::Name makeServiceSelectionDecisionNameWithoutPrefixV2(
                                                 const ndn::Name& providerName,
                                                 const ndn::Name& serviceName,
                                                 const ndn::Name& requestId,
                                                 uint64_t attempt);
    std::optional<ServiceSelectionDecisionNameV2>
    parseServiceSelectionDecisionNameV2(const ndn::Name& name);
    std::optional<ServiceSelectionDecisionNameV2> parseServiceSelectionDecisionNameV2(
        const ndn::Name& name, const ndn::Name& expectedRequester,
        const ndn::Name& expectedService);
    ndn::Name makeCompactServiceSelectionNameV2(const ndn::Name& requesterName,
                                                const ndn::Name& serviceName,
                                                const ndn::Name& requestId);
    ndn::Name makeCompactServiceSelectionNameWithoutPrefixV2(const ndn::Name& serviceName,
                                                             const ndn::Name& requestId);
    std::optional<CompactServiceSelectionNameV2>
    parseCompactServiceSelectionNameV2(const ndn::Name& serviceSelectionName);
    std::string computeSelectionProviderTokenProofHash(const ndn::Name& requesterName,
                                                       const ndn::Name& providerName,
                                                       const ndn::Name& serviceName,
                                                       const std::string& providerToken);

    ndn::Block encodeSelectionStatusQueryParameters(const ndn::Name& service, const std::string& selectionDigest);
    std::optional<SelectionStatusQueryName>
    parseSelectionStatusQueryName(const ndn::Interest& interest, const ndn::Name& expectedProvider);
    // Explicit legacy Name-only grammar; current callers use the contextual Interest overload.
    ndn::Name makeSelectionStatusQueryName(const ndn::Name& providerName,
                                           const ndn::Name& serviceName,
                                           const std::string& selectionDigest);
    std::optional<SelectionStatusQueryName>
    parseSelectionStatusQueryName(const ndn::Name& statusQueryName);
    std::string computeSelectionDigest(const ServiceSelectionMessage& message);

    /** Compute the pre-envelope Selection digest used by request-scoped key
     * bindings.  The RSA envelope is excluded to avoid a circular digest;
     * the final Selection wire digest remains available from
     * computeSelectionDigest(). */
    std::string computeSelectionDigestWithoutKeyEnvelope(
        const ServiceSelectionMessage& message);

    std::string makeOpaqueControlHandle(size_t bytes = 24);
    bool isValidOpaqueControlHandle(const std::string& handle);
    ndn::Name makeSecureSelectionStatusName(const ndn::Name& providerName,
                                            uint64_t version,
                                            const std::string& statusHandle);
    std::optional<SecureSelectionStatusName>
    parseSecureSelectionStatusName(const ndn::Name& name);
    ndn::Name makeProviderReadyName(const ndn::Name& requesterName,
                                    uint64_t version,
                                    const std::string& controlHandle);
    std::optional<ProviderReadyName> parseProviderReadyName(const ndn::Name& name);
    std::optional<ProviderReadyName> parseProviderReadyName(const ndn::Name& name,
                                                         const ndn::Name& expectedRequester);
    ndn::Name makeExecutionActivateName(const ndn::Name& providerName,
                                        uint64_t version,
                                        const std::string& controlHandle);
    std::optional<ExecutionActivateName> parseExecutionActivateName(const ndn::Name& name);
    std::optional<ExecutionActivateName> parseExecutionActivateName(const ndn::Name& name,
                                                                 const ndn::Name& expectedProvider);

    ndn::Name makeCollaborationDataName(const ndn::Name& producerName,
                                        const ndn::Name& requesterName,
                                        const ndn::Name& requestId,
                                        const std::string& keyScope,
                                        const ndn::Name& topic,
                                        uint64_t sequence);
    std::optional<CollaborationDataName>
    parseCollaborationDataName(const ndn::Name& collaborationDataName);
    /** Strict current reader; Producer comes from the validated SVS actor. */
    std::optional<CollaborationDataName>
    parseCollaborationDataName(const ndn::Name& collaborationDataName,
                               const ndn::Name& expectedProducer);

    ndn::Buffer encodeLargeDataReferencePayload(const LargeDataReference& reference);
    std::optional<LargeDataReference> parseLargeDataReferencePayload(const ndn::Buffer& payload);
    bool isLargeDataReferencePayload(const ndn::Buffer& payload);

    // Register one Interest filter with a bounded retry on transient RIB
    // authorization rejections.  NFD's offline command authenticator can
    // miss the freshly created signer certificate while the node PIB is
    // briefly locked by a concurrent keychain operation; a short bounded
    // retry avoids turning that race into a fatal startup failure.
    void registerInterestFilterWithRetry(
        ndn::Face& face,
        const ndn::Name& prefix,
        std::function<void(const ndn::InterestFilter&, const ndn::Interest&)> onInterest,
        std::function<void(const ndn::Name&)> onSuccess,
        std::function<void(const ndn::Name&, const std::string&)> onFailure,
        size_t attempts,
        std::chrono::milliseconds delayBetweenAttempts,
        std::shared_ptr<ndn::ScopedRegisteredPrefixHandle> registrationHolder);

    // /muas/drone1/NDNSF/TOKEN/ObjectDetection/YOLOv8/0
    // <provider> <service> <function> <seqNum>
    std::shared_ptr<ndn::Buffer> CombineSegmentsIntoBuffer(ndn::nacabe::SPtrVector<ndn::Data> segments);

    std::string NameToRegexString(ndn::Name& name);
    std::string NameToRegexString(const ndn::Name& name);

    std::string RandomString(const int len);

    std::optional<std::vector<std::string>> GetAttributesByName(const ndn::Name& name);

    // ndn::Block to ndn::span<const uint8_t>
    ndn::span<const uint8_t> blockToSpan(const ndn::Block& block);

    // concatnate std::vector<std::string> using " and "
    std::string ConcatenateString(const std::vector<std::string>& strs);

    // Function to merge content values of multiple ndn::Data objects into a uint8_t array
    std::vector<uint8_t> mergeDataContents(const std::vector<std::shared_ptr<ndn::Data>>& dataPackets);

    // PermissionResponse-only encryption helpers. These do not use NAC-ABE.
    // Intended algorithm: RSA-wrapped AES-CBC.
    EncryptedPermissionResponse
    encryptPermissionResponseForCertificate(const PermissionResponse& response,
                                            const ndn::security::Certificate& recipientCert);

    PermissionResponse
    decryptPermissionResponseWithKeyChain(const EncryptedPermissionResponse& encryptedResponse,
                                          const ndn::security::KeyChain& keyChain);

} // namespace ndn_service_framework
#endif
