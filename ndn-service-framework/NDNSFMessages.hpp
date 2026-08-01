#ifndef NDN_SERVICE_FRAMEWORK_MESSAGES_HPP
#define NDN_SERVICE_FRAMEWORK_MESSAGES_HPP

#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common.hpp"

namespace ndn_service_framework {

namespace tlv {
    // Message types
    enum {
        RequestMessageType = 128,
        ResponseMessageType = 129,
        RequestAckMessageType = 130,
        ServiceSelectionMessageType = 131,
        ServiceAccessMessageType = 132,
        ServiceProvisionMessageType = 133,
        TokenType = 150,
        PayloadType = 151,
        StatusType = 152,
        ErrorInfoType = 153,
        RequestIDType = 154,
        StrategyType = 155,
        PermissionEntryType = 156,
        PermissionResponseType = 157,
        ProviderNameType = 158,
        ServiceNameType = 159,
        PermissionKindType = 160,
        TargetIdentityType = 161,
        TtlType = 162,
        VersionType = 163,
        EncryptedPermissionResponseType = 164,
        RecipientCertNameType = 165,
        AlgorithmType = 166,
        EncryptedAesKeyType = 167,
        IvType = 168,
        CipherTextType = 169,
        UserTokenType = 170,
        ProviderTokenType = 171,
        HybridMessageEnvelopeType = 172,
        KeyIdType = 173,
        EpochIdType = 174,
        NonceType = 175,
        AuthTagType = 176,
        WrappedMessageKeyType = 177,
        MessageTypeType = 178,
        CollaborationDataMessageType = 179,
        KeyScopeType = 180,
        TopicType = 181,
        ProducerRoleType = 182,
        SequenceType = 183,
        AssignmentPayloadType = 184,
        SelectionProviderEntryType = 0xF503,
        PolicyManifestType = 185,
        ValidFromType = 186,
        GracePeriodMsType = 187,
        RequiredKeyEpochType = 188,
        RequestModeType = 189,
        AllowedServiceListType = 0xF501,
        AllowedServiceType = 0xF502,
        DeploymentIntentType = 0xF610,
        ProviderCapabilityOfferType = 0xF611,
        DeploymentPlanType = 0xF612,
        ProviderReadyMessageType = 0xF613,
        ReadyAcknowledgementType = 0xF614,
        ExecutionActivateMessageType = 0xF615,
        SecureStatusQueryType = 0xF616,
        SecureStatusSnapshotType = 0xF617,
        DeploymentControlFieldType = 0xF618,
        DeploymentControlFieldNameType = 0xF619,
        DeploymentControlFieldValueType = 0xF61A,
        RequestCapabilitiesType = 0xF61B,
        EncryptedRequestInputType = 0xF61C,
        SelectionInputKeyOfferType = 0xF61D,
        SelectionInputKeyGrantType = 0xF61E,
        ReservationLeaseType = 0xF61F,
        SelectionDecisionType = 0xF620,
        SelectionDecisionReceiptType = 0xF621,
        RecipientEncryptedAssignmentType = 0xF622,
        StageInputEvidenceType = 0xF623,
        StageAbortType = 0xF624,
        SelectionDecisionTombstoneType = 0xF625,
        AttemptType = 0xF626,
        OpaqueAssignmentSetType = 0xF627,
        OpaqueAssignmentItemType = 0xF628,
        CollaborationAssignmentEnvelopeType = 0xF629,
        CollaborationRoleType = 0xF62A,
        CollaborationArtifactType = 0xF62B,
        CollaborationProvisioningType = 0xF62C,
        CollaborationProvisioningTimeoutType = 0xF62D,
        CollaborationOpaquePayloadType = 0xF62E,
        CollaborationScopeKeysType = 0xF62F,
        CollaborationScopeKeyType = 0xF630,
        CollaborationScopeKeyNameType = 0xF631,
        CollaborationScopeKeyValueType = 0xF632,
    };

    // Selection strategies.
    enum {
        FirstResponding = 0,
        RandomSelection = 1,
        AllSelected = 2,
    };

    enum {
        NormalRequest = 0,
        // Targeted invocation still uses NDNSF Request/Response, but skips the
        // normal ACK/Selection phase because the requester already names the
        // intended provider.
        TargetedRequest = 1,
        TargetedBootstrapRequest = 2,
    };

    enum {
        UserPermission = 0,
        ProviderPermission = 1,
    };
}

class AbstractMessage {
public:
    virtual ~AbstractMessage() {}

    virtual ndn::Block WireEncode() const = 0;
    virtual bool WireDecode(const ndn::Block& block) = 0;
    virtual void Clear() = 0;
};

/** Canonical bounded container shared by Spec 129 deployment-control messages.
 *
 * Values are opaque UTF-8/byte strings at Core level. DI interprets model and
 * runtime fields; Core owns version, bounds, canonical wire, and digest.
 */
class DeploymentControlMessage : public AbstractMessage
{
public:
    static constexpr uint64_t VERSION = 1;
    static constexpr size_t MAX_FIELDS = 64;
    static constexpr size_t MAX_FIELD_NAME = 64;
    static constexpr size_t MAX_FIELD_VALUE = 4096;
    static constexpr size_t MAX_WIRE_SIZE = 65536;

    explicit DeploymentControlMessage(uint32_t messageType = tlv::DeploymentIntentType);
    void setVersion(uint64_t version);
    uint64_t getVersion() const;
    void setField(const std::string& name, const std::string& value);
    bool hasField(const std::string& name) const;
    const std::string& getField(const std::string& name) const;
    const std::map<std::string, std::string>& getFields() const;
    std::string computeDigest() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

protected:
    uint32_t getMessageType() const;

private:
    uint32_t messageType_;
    uint64_t version_ = VERSION;
    std::map<std::string, std::string> fields_;
    mutable ndn::Block wire_;
};

class DeploymentIntent : public DeploymentControlMessage
{ public: DeploymentIntent(); };
class ProviderCapabilityOffer : public DeploymentControlMessage
{ public: ProviderCapabilityOffer(); };
class DeploymentPlan : public DeploymentControlMessage
{ public: DeploymentPlan(); };
class ProviderReadyMessage : public DeploymentControlMessage
{ public: ProviderReadyMessage(); };
class ReadyAcknowledgement : public DeploymentControlMessage
{ public: ReadyAcknowledgement(); };
class ExecutionActivateMessage : public DeploymentControlMessage
{ public: ExecutionActivateMessage(); };
class SecureStatusQuery : public DeploymentControlMessage
{ public: SecureStatusQuery(); };
class SecureStatusSnapshot : public DeploymentControlMessage
{ public: SecureStatusSnapshot(); };
class RequestCapabilities : public DeploymentControlMessage
{ public: RequestCapabilities(); };
class EncryptedRequestInput : public DeploymentControlMessage
{ public: EncryptedRequestInput(); };
class SelectionInputKeyOffer : public DeploymentControlMessage
{ public: SelectionInputKeyOffer(); };
class SelectionInputKeyGrant : public DeploymentControlMessage
{ public: SelectionInputKeyGrant(); };
class ReservationLease : public DeploymentControlMessage
{ public: ReservationLease(); };
class SelectionDecision : public DeploymentControlMessage
{ public: SelectionDecision(); };
class SelectionDecisionReceipt : public DeploymentControlMessage
{ public: SelectionDecisionReceipt(); };
class RecipientEncryptedAssignment : public DeploymentControlMessage
{ public: RecipientEncryptedAssignment(); };
class StageInputEvidence : public DeploymentControlMessage
{ public: StageInputEvidence(); };
class StageAbort : public DeploymentControlMessage
{ public: StageAbort(); };
class SelectionDecisionTombstone : public DeploymentControlMessage
{ public: SelectionDecisionTombstone(); };

class RequestMessage : public AbstractMessage {
public:
    RequestMessage();
    RequestMessage(const RequestMessage& other);
    RequestMessage& operator=(const RequestMessage& other);

    void setTokens(const std::map<std::string, std::string>& tokens);
    void setUserToken(const std::string& userToken);
    void setProviderToken(const std::string& providerToken);
    void setPayload(ndn::Buffer& payload, size_t size);
    void setPayloadBlock(const ndn::Block& payloadBlock);
    // FirstResponding = 0, RandomSelection = 1, AllSelected = 2.
    void setStrategy(size_t strategy);
    void setRequestMode(size_t requestMode);
    void setTargetProvider(const ndn::Name& targetProvider);
    void setPolicyEpoch(size_t policyEpoch);
    void setDeploymentIntent(const DeploymentIntent& intent);
    void setRequestCapabilities(const RequestCapabilities& capabilities);
    void setEncryptedRequestInput(const EncryptedRequestInput& input);
    bool hasDeploymentIntent() const;
    bool hasRequestCapabilities() const;
    bool hasEncryptedRequestInput() const;
    const DeploymentIntent& getDeploymentIntent() const;
    const RequestCapabilities& getRequestCapabilities() const;
    const EncryptedRequestInput& getEncryptedRequestInput() const;
    const std::map<std::string, std::string>& getTokens() const;
    const std::string& getUserToken() const;
    const std::string& getProviderToken() const;
    ndn::Buffer getPayload() const;
    const ndn::Block& getPayloadBlock() const;
    size_t getPayloadSize() const;
    size_t getStrategy() const;
    size_t getRequestMode() const;
    const ndn::Name& getTargetProvider() const;
    size_t getPolicyEpoch() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::map<std::string, std::string> tokens_;
    std::string userToken_;
    std::string providerToken_;
    std::shared_ptr<const ndn::Block> payloadBlock_;
    size_t payloadSize_ = 0;
    size_t strategy_ = tlv::FirstResponding;
    size_t requestMode_ = tlv::NormalRequest;
    ndn::Name targetProvider_;
    size_t policyEpoch_ = 0;
    std::optional<DeploymentIntent> deploymentIntent_;
    std::optional<RequestCapabilities> requestCapabilities_;
    std::optional<EncryptedRequestInput> encryptedRequestInput_;
    mutable std::shared_ptr<const ndn::Block> m_wire;
};

class ResponseMessage : public AbstractMessage {
public:
    ResponseMessage();
    ResponseMessage(const ResponseMessage& other);
    ResponseMessage& operator=(const ResponseMessage& other);

    void setStatus(bool status);
    void setErrorInfo(const std::string& errorInfo);
    void setTokens(const std::map<std::string, std::string>& tokens);
    void setUserToken(const std::string& userToken);
    void setPayload(ndn::Buffer& payload, size_t size);
    void setPayloadBlock(const ndn::Block& payloadBlock);
    void setPolicyEpoch(size_t policyEpoch);
    void setAuthenticatedTransportEvidence(const std::string& dataName,
                                           const std::string& signerCertificate,
                                           const std::string& wireDigest);
    bool getStatus() const;
    const std::string& getErrorInfo() const;
    const std::map<std::string, std::string>& getTokens() const;
    const std::string& getUserToken() const;
    ndn::Buffer getPayload() const;
    const ndn::Block& getPayloadBlock() const;
    size_t getPayloadSize() const;
    size_t getPolicyEpoch() const;
    const std::string& getDataName() const;
    const std::string& getSignerCertificate() const;
    const std::string& getWireDigest() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    bool status_ = false;
    std::string errorInfo_;
    std::map<std::string, std::string> tokens_;
    std::string userToken_;
    std::shared_ptr<const ndn::Block> payloadBlock_;
    size_t payloadSize_ = 0;
    size_t policyEpoch_ = 0;
    // Local metadata from the authenticated SVS Data packet. These fields are
    // deliberately excluded from ResponseMessage wire encoding.
    std::string dataName_;
    std::string signerCertificate_;
    std::string wireDigest_;
    mutable std::shared_ptr<const ndn::Block> m_wire;
};

class RequestAckMessage : public AbstractMessage {
public:
    RequestAckMessage();
    RequestAckMessage(const RequestAckMessage& other);
    RequestAckMessage& operator=(const RequestAckMessage& other);

    void setStatus(bool status);
    void setMessage(const std::string& message);
    void setUserToken(const std::string& userToken);
    void setProviderToken(const std::string& providerToken);
    void setPayload(ndn::Buffer& payload, size_t size);
    void setPayloadBlock(const ndn::Block& payloadBlock);
    void setPolicyEpoch(size_t policyEpoch);
    void setProviderCapabilityOffer(const ProviderCapabilityOffer& offer);
    void setSelectionInputKeyOffer(const SelectionInputKeyOffer& offer);
    void setReservationLease(const ReservationLease& lease);
    bool hasProviderCapabilityOffer() const;
    bool hasSelectionInputKeyOffer() const;
    bool hasReservationLease() const;
    const ProviderCapabilityOffer& getProviderCapabilityOffer() const;
    const SelectionInputKeyOffer& getSelectionInputKeyOffer() const;
    const ReservationLease& getReservationLease() const;
    bool getStatus() const;
    const std::string& getMessage() const;
    const std::string& getUserToken() const;
    const std::string& getProviderToken() const;
    ndn::Buffer getPayload() const;
    const ndn::Block& getPayloadBlock() const;
    size_t getPayloadSize() const;
    size_t getPolicyEpoch() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    bool status_ = false;
    std::string message_;
    std::string userToken_;
    std::string providerToken_;
    std::shared_ptr<const ndn::Block> payloadBlock_;
    size_t payloadSize_ = 0;
    size_t policyEpoch_ = 0;
    std::optional<ProviderCapabilityOffer> providerCapabilityOffer_;
    std::optional<SelectionInputKeyOffer> selectionInputKeyOffer_;
    std::optional<ReservationLease> reservationLease_;
    mutable std::shared_ptr<const ndn::Block> m_wire;
};

/** Encode multiple generic collaboration assignments for one provider.
 *
 * Core preserves item order and byte identity but never interprets an item.
 * A single assignment remains its original bytes for wire compatibility.
 */
ndn::Buffer
encodeOpaqueAssignmentSet(const std::vector<ndn::Buffer>& assignments);

/** Decode a Core assignment set; a non-container is one opaque item. */
std::vector<ndn::Buffer>
decodeOpaqueAssignmentSet(const ndn::Buffer& payload);

/** Framework-owned metadata around one application-owned opaque assignment.
 *
 * The envelope keeps role/authorization and optional provisioning metadata
 * separate from the application bytes. Decoding returns false for legacy
 * semicolon assignments and arbitrary opaque payloads.
 */
struct CollaborationAssignmentEnvelope
{
    std::string role;
    ndn::Name assignedArtifact;
    bool requiresProvisioning = false;
    uint64_t provisioningTimeoutMs = 0;
    std::map<std::string, ndn::Buffer> scopeKeys;
    ndn::Buffer opaquePayload;
};

ndn::Buffer
encodeCollaborationAssignmentEnvelope(
    const CollaborationAssignmentEnvelope& assignment);

bool
decodeCollaborationAssignmentEnvelope(
    const ndn::Buffer& payload,
    CollaborationAssignmentEnvelope& assignment);

struct SelectionProviderEntry
{
    ndn::Name providerName;
    std::string providerTokenHash;
    ndn::Buffer assignmentPayload;
};

class ServiceSelectionMessage : public AbstractMessage {
public:
    ServiceSelectionMessage();
    ServiceSelectionMessage(const ServiceSelectionMessage& other);
    ServiceSelectionMessage& operator=(const ServiceSelectionMessage& other);

    void setRequestIDs(const std::vector<std::string>& requestIDs);
    void setProviderToken(const std::string& providerToken);
    void setAssignmentPayload(const ndn::Buffer& payload);
    void setPolicyEpoch(size_t policyEpoch);
    void setAttempt(uint64_t attempt);
    void addProviderEntry(const SelectionProviderEntry& entry);
    void setDeploymentPlan(const DeploymentPlan& plan);
    void setSelectionDecision(const SelectionDecision& decision);
    void setSelectionInputKeyGrant(const SelectionInputKeyGrant& grant);
    void setRecipientEncryptedAssignment(const RecipientEncryptedAssignment& assignment);
    bool hasDeploymentPlan() const;
    bool hasSelectionDecision() const;
    bool hasSelectionInputKeyGrant() const;
    bool hasRecipientEncryptedAssignment() const;
    const DeploymentPlan& getDeploymentPlan() const;
    const SelectionDecision& getSelectionDecision() const;
    const SelectionInputKeyGrant& getSelectionInputKeyGrant() const;
    const RecipientEncryptedAssignment& getRecipientEncryptedAssignment() const;
    const std::vector<std::string>& getRequestIDs() const;
    const std::string& getProviderToken() const;
    const ndn::Buffer& getAssignmentPayload() const;
    size_t getPolicyEpoch() const;
    uint64_t getAttempt() const;
    const std::vector<SelectionProviderEntry>& getProviderEntries() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::vector<std::string> requestIDs_;
    std::string providerToken_;
    ndn::Buffer assignmentPayload_;
    size_t policyEpoch_ = 0;
    uint64_t attempt_ = 1;
    std::vector<SelectionProviderEntry> providerEntries_;
    std::optional<DeploymentPlan> deploymentPlan_;
    std::optional<SelectionDecision> selectionDecision_;
    std::optional<SelectionInputKeyGrant> selectionInputKeyGrant_;
    std::optional<RecipientEncryptedAssignment> recipientEncryptedAssignment_;
    mutable ndn::Block m_wire;
};

class CollaborationDataMessage : public AbstractMessage {
public:
    CollaborationDataMessage();

    void setKeyScope(const std::string& keyScope);
    void setTopic(const ndn::Name& topic);
    void setProducerRole(const std::string& role);
    void setSequence(uint64_t sequence);
    void setPayload(const ndn::Buffer& payload);

    const std::string& getKeyScope() const;
    const ndn::Name& getTopic() const;
    const std::string& getProducerRole() const;
    uint64_t getSequence() const;
    const ndn::Buffer& getPayload() const;

    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::string keyScope_;
    ndn::Name topic_;
    std::string producerRole_;
    uint64_t sequence_ = 0;
    ndn::Buffer payload_;
    mutable ndn::Block m_wire;
};

class HybridMessageEnvelope : public AbstractMessage {
public:
    HybridMessageEnvelope();

    void setVersion(size_t version);
    void setAlgorithm(const std::string& algorithm);
    void setKeyId(const std::string& keyId);
    void setEpochId(const std::string& epochId);
    void setMessageType(const std::string& messageType);
    void setNonce(const ndn::Buffer& nonce);
    void setCipherText(const ndn::Buffer& cipherText);
    void setAuthTag(const ndn::Buffer& authTag);
    void setWrappedMessageKey(const ndn::Buffer& wrappedMessageKey);

    size_t getVersion() const;
    const std::string& getAlgorithm() const;
    const std::string& getKeyId() const;
    const std::string& getEpochId() const;
    const std::string& getMessageType() const;
    const ndn::Buffer& getNonce() const;
    const ndn::Buffer& getCipherText() const;
    const ndn::Buffer& getAuthTag() const;
    const ndn::Buffer& getWrappedMessageKey() const;
    bool hasWrappedMessageKey() const;

    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    size_t version_ = 1;
    std::string algorithm_ = "AES-256-GCM";
    std::string keyId_;
    std::string epochId_;
    std::string messageType_;
    ndn::Buffer nonce_;
    ndn::Buffer cipherText_;
    ndn::Buffer authTag_;
    ndn::Buffer wrappedMessageKey_;
    mutable ndn::Block m_wire;
};

class PermissionEntry : public AbstractMessage {
public:
    PermissionEntry();

    void setProviderName(const std::string& providerName);
    void setServiceName(const std::string& serviceName);
    void setToken(const std::string& token);
    void setTtl(size_t ttl);
    void setVersion(size_t version);

    const std::string& getProviderName() const;
    const std::string& getServiceName() const;
    const std::string& getToken() const;
    size_t getTtl() const;
    size_t getVersion() const;
    std::string toString() const;

    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::string providerName_;
    std::string serviceName_;
    std::string token_;
    size_t ttl_ = 0;
    size_t version_ = 1;
    mutable ndn::Block m_wire;
};

class PermissionResponse : public AbstractMessage {
public:
    PermissionResponse();

    void setTargetIdentity(const std::string& targetIdentity);
    void setPermissionKind(size_t permissionKind);
    void setPolicyEpoch(size_t policyEpoch);
    void setEntries(const std::vector<PermissionEntry>& entries);
    void addEntry(const PermissionEntry& entry);

    const std::string& getTargetIdentity() const;
    size_t getPermissionKind() const;
    size_t getPolicyEpoch() const;
    const std::vector<PermissionEntry>& getEntries() const;
    std::string toString() const;

    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::string targetIdentity_;
    size_t permissionKind_ = tlv::UserPermission;
    size_t policyEpoch_ = 1;
    std::vector<PermissionEntry> entries_;
    mutable ndn::Block m_wire;
};

class PolicyManifest : public AbstractMessage {
public:
    PolicyManifest();

    void setPolicyEpoch(size_t policyEpoch);
    void setValidFromMs(uint64_t validFromMs);
    void setGracePeriodMs(uint64_t gracePeriodMs);
    void setRequiredKeyEpoch(size_t requiredKeyEpoch);

    size_t getPolicyEpoch() const;
    uint64_t getValidFromMs() const;
    uint64_t getGracePeriodMs() const;
    size_t getRequiredKeyEpoch() const;
    std::string toString() const;

    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    size_t policyEpoch_ = 1;
    uint64_t validFromMs_ = 0;
    uint64_t gracePeriodMs_ = 0;
    size_t requiredKeyEpoch_ = 1;
    mutable ndn::Block m_wire;
};

// Used only for PermissionResponse encryption.
// This is not NAC-ABE and must not be used for NDNSF service message encryption.
// Intended algorithm: RSA-wrapped AES-CBC.
class EncryptedPermissionResponse : public AbstractMessage {
public:
    EncryptedPermissionResponse();

    void setRecipientCertName(const std::string& recipientCertName);
    void setAlgorithm(const std::string& algorithm);
    void setEncryptedAesKey(const ndn::Buffer& encryptedAesKey);
    void setIv(const ndn::Buffer& iv);
    void setCipherText(const ndn::Buffer& cipherText);

    const std::string& getRecipientCertName() const;
    const std::string& getAlgorithm() const;
    const ndn::Buffer& getEncryptedAesKey() const;
    const ndn::Buffer& getIv() const;
    const ndn::Buffer& getCipherText() const;
    std::string toString() const;

    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::string recipientCertName_;
    std::string algorithm_;
    ndn::Buffer encryptedAesKey_;
    ndn::Buffer iv_;
    ndn::Buffer cipherText_;
    mutable ndn::Block m_wire;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_MESSAGES_HPP
