#include "NDNSFMessages.hpp"

namespace ndn_service_framework {

RequestMessage::RequestMessage() {}

void RequestMessage::setTokens(const std::map<std::string, std::string>& tokens) {
    tokens_ = tokens;
}

void RequestMessage::setPayload(ndn::Buffer& payload, size_t size) {
    payload_ = payload;
    payloadSize_ = size;
}

void RequestMessage::setStrategy(size_t strategy) {
    strategy_ = strategy;
}

const std::map<std::string, std::string>& RequestMessage::getTokens() const {
    return tokens_;
}

ndn::Buffer RequestMessage::getPayload() const {
    return payload_;
}

size_t RequestMessage::getPayloadSize() const {
    return payloadSize_;
}

size_t RequestMessage::getStrategy() const {
    return strategy_;
}

void RequestMessage::Clear() {
    tokens_.clear();
    payload_.clear();
    payloadSize_ = 0;
    m_wire.reset();
}

ndn::Block RequestMessage::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::RequestMessageType);
    for (const auto& token : tokens_) {
        block.push_back(ndn::makeStringBlock(tlv::TokenType, token.first + "=" + token.second));
    }
    // payload
    ndn::Block payloadBlock = ndn::makeBinaryBlock(tlv::PayloadType, payload_.begin(), payload_.end());
    block.push_back(payloadBlock);
    // strategy
    ndn::Block strategyloadBlock = ndn::makeNonNegativeIntegerBlock(tlv::StrategyType, strategy_);
    block.push_back(strategyloadBlock);
    block.encode();
    m_wire = block;
    return m_wire;
}

bool RequestMessage::WireDecode(const ndn::Block& block) {
    Clear(); // 清除已初始化的值

    if (block.type() != tlv::RequestMessageType) {
        return false; // 消息类型不匹配
    }

    block.parse();
    for(auto b : block.elements()){
        if (b.type() == tlv::TokenType) {
            std::string tokenStr = ndn::readString(b);
            size_t pos = tokenStr.find('=');
            if (pos != std::string::npos) {
                std::string key = tokenStr.substr(0, pos);
                std::string value = tokenStr.substr(pos + 1);
                tokens_[key] = value;
            }
        }
        if (b.type() == tlv::PayloadType) {
            payload_ = ndn::Buffer(b.value(),b.value_size());
            payloadSize_ = b.value_size();
        }
        if (b.type() == tlv::StrategyType) {
            strategy_ = ndn::readNonNegativeInteger(b);
        }
    }

    return true;
}

ResponseMessage::ResponseMessage() {}

void ResponseMessage::setStatus(bool status) {
    status_ = status;
}

void ResponseMessage::setErrorInfo(const std::string& errorInfo) {
    errorInfo_ = errorInfo;
}

void ResponseMessage::setPayload(ndn::Buffer& payload, size_t size) {
    payload_ = payload;
    payloadSize_ = size;
}

bool ResponseMessage::getStatus() const {
    return status_;
}

const std::string& ResponseMessage::getErrorInfo() const {
    return errorInfo_;
}

ndn::Buffer ResponseMessage::getPayload() const {
    return payload_;
}

size_t ResponseMessage::getPayloadSize() const {
    return payloadSize_;
}

void ResponseMessage::Clear() {
    status_ = false;
    errorInfo_.clear();
    payload_.clear();
    payloadSize_ = 0;
    m_wire.reset();
}

ndn::Block ResponseMessage::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::ResponseMessageType);
    // 编码 status
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StatusType, static_cast<int>(status_)));
    // 编码 errorInfo
    block.push_back(ndn::makeStringBlock(tlv::ErrorInfoType, errorInfo_));
    // 编码 payload
    ndn::Block payloadBlock = ndn::makeBinaryBlock(tlv::PayloadType, payload_.begin(), payload_.end());
    block.push_back(payloadBlock);
    block.encode();
    m_wire = block;
    return m_wire;
}

bool ResponseMessage::WireDecode(const ndn::Block& block) {
    Clear(); // 清除已初始化的值

    if (block.type() != tlv::ResponseMessageType) {
        return false; // 消息类型不匹配
    }
    block.parse();
    for(auto b : block.elements()){
        if (b.type() == tlv::StatusType) {
            status_ = ndn::readNonNegativeInteger(b) > 0 ? true : false;
        }
        else if (b.type() == tlv::ErrorInfoType) {
            errorInfo_ = ndn::readString(b);
        }
        else if (b.type() == tlv::PayloadType) {
            payload_ = ndn::Buffer(b.value(),b.value_size());
            payloadSize_ = b.value_size();
        }
    }

    return true;
}

RequestAckMessage::RequestAckMessage() {}

void RequestAckMessage::setStatus(bool status) {
    status_ = status;
}

void RequestAckMessage::setMessage(const std::string& message) {
    message_ = message;
}

bool RequestAckMessage::getStatus() const {
    return status_;
}

const std::string& RequestAckMessage::getMessage() const {
    return message_;
}

void RequestAckMessage::Clear() {
    status_ = false;
    message_.clear();
    m_wire.reset();
}

ndn::Block RequestAckMessage::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::RequestAckMessageType);
    // 编码 status
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StatusType, static_cast<int>(status_)));
    // 编码 message
    block.push_back(ndn::makeStringBlock(tlv::ErrorInfoType, message_));
    block.encode();
    m_wire = block;
    return m_wire;
}

bool RequestAckMessage::WireDecode(const ndn::Block& block) {
    Clear(); // 清除已初始化的值

    if (block.type() != tlv::RequestAckMessageType) {
        return false; // 消息类型不匹配
    }

    block.parse();
    for(auto b : block.elements()){
        if (b.type() == tlv::StatusType) {
            status_ = ndn::readNonNegativeInteger(b) > 0 ? true : false;
        }
        else if (b.type() == tlv::ErrorInfoType) {
            message_ = ndn::readString(b);
        }
    }

    return true;
}

ServiceCoordinationMessage::ServiceCoordinationMessage() {}

void ServiceCoordinationMessage::setRequestIDs(const std::vector<std::string>& requestIDs) {
    requestIDs_ = requestIDs;
}

const std::vector<std::string>& ServiceCoordinationMessage::getRequestIDs() const {
    return requestIDs_;
}

void ServiceCoordinationMessage::Clear() {
    requestIDs_.clear();
    m_wire.reset();
}

ndn::Block ServiceCoordinationMessage::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::ServiceSelectionMessageType);
    for (const auto& id : requestIDs_) {
        block.push_back(ndn::makeStringBlock(tlv::RequestIDType, id));
    }
    block.encode();
    m_wire = block;
    return m_wire;
}

bool ServiceCoordinationMessage::WireDecode(const ndn::Block& block) {
    Clear(); // 清除已初始化的值

    if (block.type() != tlv::ServiceSelectionMessageType) {
        return false; // 消息类型不匹配
    }

    block.parse();
    for(auto b : block.elements()){
        if (b.type() == tlv::RequestIDType) {
            requestIDs_.push_back(ndn::readString(b));
        }
    }

    return true;
}

PermissionEntry::PermissionEntry() {}

void PermissionEntry::setProviderName(const std::string& providerName) {
    providerName_ = providerName;
}

void PermissionEntry::setServiceName(const std::string& serviceName) {
    serviceName_ = serviceName;
}

void PermissionEntry::setToken(const std::string& token) {
    token_ = token;
}

void PermissionEntry::setTtl(size_t ttl) {
    ttl_ = ttl;
}

void PermissionEntry::setVersion(size_t version) {
    version_ = version;
}

const std::string& PermissionEntry::getProviderName() const {
    return providerName_;
}

const std::string& PermissionEntry::getServiceName() const {
    return serviceName_;
}

const std::string& PermissionEntry::getToken() const {
    return token_;
}

size_t PermissionEntry::getTtl() const {
    return ttl_;
}

size_t PermissionEntry::getVersion() const {
    return version_;
}

std::string PermissionEntry::toString() const {
    return "PermissionEntry{providerName=" + providerName_ +
           ", serviceName=" + serviceName_ +
           ", token=" + token_ +
           ", ttl=" + std::to_string(ttl_) +
           ", version=" + std::to_string(version_) + "}";
}

void PermissionEntry::Clear() {
    providerName_.clear();
    serviceName_.clear();
    token_.clear();
    ttl_ = 0;
    version_ = 1;
    m_wire.reset();
}

ndn::Block PermissionEntry::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }

    ndn::Block block(tlv::PermissionEntryType);
    block.push_back(ndn::makeStringBlock(tlv::ProviderNameType, providerName_));
    block.push_back(ndn::makeStringBlock(tlv::ServiceNameType, serviceName_));
    block.push_back(ndn::makeStringBlock(tlv::TokenType, token_));
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::TtlType, ttl_));
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, version_));
    block.encode();
    m_wire = block;
    return m_wire;
}

bool PermissionEntry::WireDecode(const ndn::Block& block) {
    Clear();

    if (block.type() != tlv::PermissionEntryType) {
        return false;
    }

    block.parse();
    for (auto b : block.elements()) {
        if (b.type() == tlv::ProviderNameType) {
            providerName_ = ndn::readString(b);
        }
        else if (b.type() == tlv::ServiceNameType) {
            serviceName_ = ndn::readString(b);
        }
        else if (b.type() == tlv::TokenType) {
            token_ = ndn::readString(b);
        }
        else if (b.type() == tlv::TtlType) {
            ttl_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::VersionType) {
            version_ = ndn::readNonNegativeInteger(b);
        }
    }

    return true;
}

PermissionResponse::PermissionResponse() {}

void PermissionResponse::setTargetIdentity(const std::string& targetIdentity) {
    targetIdentity_ = targetIdentity;
}

void PermissionResponse::setPermissionKind(size_t permissionKind) {
    permissionKind_ = permissionKind;
}

void PermissionResponse::setEntries(const std::vector<PermissionEntry>& entries) {
    entries_ = entries;
}

void PermissionResponse::addEntry(const PermissionEntry& entry) {
    entries_.push_back(entry);
}

const std::string& PermissionResponse::getTargetIdentity() const {
    return targetIdentity_;
}

size_t PermissionResponse::getPermissionKind() const {
    return permissionKind_;
}

const std::vector<PermissionEntry>& PermissionResponse::getEntries() const {
    return entries_;
}

std::string PermissionResponse::toString() const {
    std::string result = "PermissionResponse{targetIdentity=" + targetIdentity_ +
                         ", permissionKind=" + std::to_string(permissionKind_) +
                         ", entries=[";
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (i > 0) {
            result += ", ";
        }
        result += entries_[i].toString();
    }
    result += "]}";
    return result;
}

void PermissionResponse::Clear() {
    targetIdentity_.clear();
    permissionKind_ = tlv::UserPermission;
    entries_.clear();
    m_wire.reset();
}

ndn::Block PermissionResponse::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }

    ndn::Block block(tlv::PermissionResponseType);
    block.push_back(ndn::makeStringBlock(tlv::TargetIdentityType, targetIdentity_));
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::PermissionKindType, permissionKind_));
    for (const auto& entry : entries_) {
        block.push_back(entry.WireEncode());
    }
    block.encode();
    m_wire = block;
    return m_wire;
}

bool PermissionResponse::WireDecode(const ndn::Block& block) {
    Clear();

    if (block.type() != tlv::PermissionResponseType) {
        return false;
    }

    block.parse();
    for (auto b : block.elements()) {
        if (b.type() == tlv::TargetIdentityType) {
            targetIdentity_ = ndn::readString(b);
        }
        else if (b.type() == tlv::PermissionKindType) {
            permissionKind_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::PermissionEntryType) {
            PermissionEntry entry;
            if (entry.WireDecode(b)) {
                entries_.push_back(entry);
            }
        }
    }

    return true;
}

EncryptedPermissionResponse::EncryptedPermissionResponse() {}

void EncryptedPermissionResponse::setRecipientCertName(const std::string& recipientCertName) {
    recipientCertName_ = recipientCertName;
}

void EncryptedPermissionResponse::setAlgorithm(const std::string& algorithm) {
    algorithm_ = algorithm;
}

void EncryptedPermissionResponse::setEncryptedAesKey(const ndn::Buffer& encryptedAesKey) {
    encryptedAesKey_ = encryptedAesKey;
}

void EncryptedPermissionResponse::setIv(const ndn::Buffer& iv) {
    iv_ = iv;
}

void EncryptedPermissionResponse::setCipherText(const ndn::Buffer& cipherText) {
    cipherText_ = cipherText;
}

const std::string& EncryptedPermissionResponse::getRecipientCertName() const {
    return recipientCertName_;
}

const std::string& EncryptedPermissionResponse::getAlgorithm() const {
    return algorithm_;
}

const ndn::Buffer& EncryptedPermissionResponse::getEncryptedAesKey() const {
    return encryptedAesKey_;
}

const ndn::Buffer& EncryptedPermissionResponse::getIv() const {
    return iv_;
}

const ndn::Buffer& EncryptedPermissionResponse::getCipherText() const {
    return cipherText_;
}

std::string EncryptedPermissionResponse::toString() const {
    return "EncryptedPermissionResponse{recipientCertName=" + recipientCertName_ +
           ", algorithm=" + algorithm_ +
           ", encryptedAesKeySize=" + std::to_string(encryptedAesKey_.size()) +
           ", ivSize=" + std::to_string(iv_.size()) +
           ", cipherTextSize=" + std::to_string(cipherText_.size()) + "}";
}

void EncryptedPermissionResponse::Clear() {
    recipientCertName_.clear();
    algorithm_.clear();
    encryptedAesKey_.clear();
    iv_.clear();
    cipherText_.clear();
    m_wire.reset();
}

ndn::Block EncryptedPermissionResponse::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }

    ndn::Block block(tlv::EncryptedPermissionResponseType);
    block.push_back(ndn::makeStringBlock(tlv::RecipientCertNameType, recipientCertName_));
    block.push_back(ndn::makeStringBlock(tlv::AlgorithmType, algorithm_));
    block.push_back(ndn::makeBinaryBlock(tlv::EncryptedAesKeyType,
                                         encryptedAesKey_.begin(),
                                         encryptedAesKey_.end()));
    block.push_back(ndn::makeBinaryBlock(tlv::IvType, iv_.begin(), iv_.end()));
    block.push_back(ndn::makeBinaryBlock(tlv::CipherTextType,
                                         cipherText_.begin(),
                                         cipherText_.end()));
    block.encode();
    m_wire = block;
    return m_wire;
}

bool EncryptedPermissionResponse::WireDecode(const ndn::Block& block) {
    Clear();

    if (block.type() != tlv::EncryptedPermissionResponseType) {
        return false;
    }

    block.parse();
    for (auto b : block.elements()) {
        if (b.type() == tlv::RecipientCertNameType) {
            recipientCertName_ = ndn::readString(b);
        }
        else if (b.type() == tlv::AlgorithmType) {
            algorithm_ = ndn::readString(b);
        }
        else if (b.type() == tlv::EncryptedAesKeyType) {
            encryptedAesKey_ = ndn::Buffer(b.value(), b.value_size());
        }
        else if (b.type() == tlv::IvType) {
            iv_ = ndn::Buffer(b.value(), b.value_size());
        }
        else if (b.type() == tlv::CipherTextType) {
            cipherText_ = ndn::Buffer(b.value(), b.value_size());
        }
    }

    return true;
}

} // namespace ndn_service_framework
