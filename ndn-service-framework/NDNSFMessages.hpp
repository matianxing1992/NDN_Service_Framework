#ifndef NDN_SERVICE_FRAMEWORK_MESSAGES_HPP
#define NDN_SERVICE_FRAMEWORK_MESSAGES_HPP

#include <iostream>
#include <map>
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
        AllowedServiceListType = 0xF501,
        AllowedServiceType = 0xF502,
    };

    // Coordination Strategies
    enum {
        FirstResponding = 0,
        LoadBalancing = 1,
        AllResponders = 2,
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

class RequestMessage : public AbstractMessage {
public:
    RequestMessage();

    void setTokens(const std::map<std::string, std::string>& tokens);
    void setUserToken(const std::string& userToken);
    void setPayload(ndn::Buffer& payload, size_t size);
    // FirstResponding = 0, LoadBalancing = 1, AllResponders = 2,
    void setStrategy(size_t strategy);
    const std::map<std::string, std::string>& getTokens() const;
    const std::string& getUserToken() const;
    ndn::Buffer getPayload() const;
    size_t getPayloadSize() const;
    size_t getStrategy() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::map<std::string, std::string> tokens_;
    std::string userToken_;
    ndn::Buffer payload_;
    size_t payloadSize_ = 0;
    size_t strategy_ = tlv::FirstResponding;
    mutable ndn::Block m_wire;
};

class ResponseMessage : public AbstractMessage {
public:
    ResponseMessage();

    void setStatus(bool status);
    void setErrorInfo(const std::string& errorInfo);
    void setUserToken(const std::string& userToken);
    void setPayload(ndn::Buffer& payload, size_t size);
    bool getStatus() const;
    const std::string& getErrorInfo() const;
    const std::string& getUserToken() const;
    ndn::Buffer getPayload() const;
    size_t getPayloadSize() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    bool status_;
    std::string errorInfo_;
    std::string userToken_;
    ndn::Buffer payload_;
    size_t payloadSize_ = 0;
    mutable ndn::Block m_wire;
};

class RequestAckMessage : public AbstractMessage {
public:
    RequestAckMessage();

    void setStatus(bool status);
    void setMessage(const std::string& message);
    void setUserToken(const std::string& userToken);
    void setProviderToken(const std::string& providerToken);
    void setPayload(ndn::Buffer& payload, size_t size);
    bool getStatus() const;
    const std::string& getMessage() const;
    const std::string& getUserToken() const;
    const std::string& getProviderToken() const;
    ndn::Buffer getPayload() const;
    size_t getPayloadSize() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    bool status_;
    std::string message_;
    std::string userToken_;
    std::string providerToken_;
    ndn::Buffer payload_;
    size_t payloadSize_ = 0;
    mutable ndn::Block m_wire;
};

class ServiceCoordinationMessage : public AbstractMessage {
public:
    ServiceCoordinationMessage();

    void setRequestIDs(const std::vector<std::string>& requestIDs);
    void setProviderToken(const std::string& providerToken);
    const std::vector<std::string>& getRequestIDs() const;
    const std::string& getProviderToken() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::vector<std::string> requestIDs_;
    std::string providerToken_;
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
    void setEntries(const std::vector<PermissionEntry>& entries);
    void addEntry(const PermissionEntry& entry);

    const std::string& getTargetIdentity() const;
    size_t getPermissionKind() const;
    const std::vector<PermissionEntry>& getEntries() const;
    std::string toString() const;

    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::string targetIdentity_;
    size_t permissionKind_ = tlv::UserPermission;
    std::vector<PermissionEntry> entries_;
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
