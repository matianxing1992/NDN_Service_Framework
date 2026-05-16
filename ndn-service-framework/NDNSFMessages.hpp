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
        AllowedServiceListType = 0xF501,
        AllowedServiceType = 0xF502,
    };

    // Coordination Strategies
    enum {
        FirstResponding = 0,
        LoadBalancing = 1,
        NoCoordination = 2,
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
    void setPayload(ndn::Buffer& payload, size_t size);
    // FirstResponding = 0, LoadBalancing = 1, All = 2,
    void setStrategy(size_t strategy);
    const std::map<std::string, std::string>& getTokens() const;
    ndn::Buffer getPayload() const;
    size_t getPayloadSize() const;
    size_t getStrategy() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::map<std::string, std::string> tokens_;
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
    void setPayload(ndn::Buffer& payload, size_t size);
    bool getStatus() const;
    const std::string& getErrorInfo() const;
    ndn::Buffer getPayload() const;
    size_t getPayloadSize() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    bool status_;
    std::string errorInfo_;
    ndn::Buffer payload_;
    size_t payloadSize_ = 0;
    mutable ndn::Block m_wire;
};

class RequestAckMessage : public AbstractMessage {
public:
    RequestAckMessage();

    void setStatus(bool status);
    void setMessage(const std::string& message);
    void setPayload(ndn::Buffer& payload, size_t size);
    bool getStatus() const;
    const std::string& getMessage() const;
    ndn::Buffer getPayload() const;
    size_t getPayloadSize() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    bool status_;
    std::string message_;
    ndn::Buffer payload_;
    size_t payloadSize_ = 0;
    mutable ndn::Block m_wire;
};

class ServiceCoordinationMessage : public AbstractMessage {
public:
    ServiceCoordinationMessage();

    void setRequestIDs(const std::vector<std::string>& requestIDs);
    const std::vector<std::string>& getRequestIDs() const;
    void Clear() override;
    ndn::Block WireEncode() const override;
    bool WireDecode(const ndn::Block& block) override;

private:
    std::vector<std::string> requestIDs_;
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
