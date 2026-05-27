#include "NDNSFMessages.hpp"

namespace ndn_service_framework {

RequestMessage::RequestMessage() {}

void RequestMessage::setTokens(const std::map<std::string, std::string>& tokens) {
    tokens_ = tokens;
}

void RequestMessage::setUserToken(const std::string& userToken) {
    userToken_ = userToken;
}

void RequestMessage::setPayload(ndn::Buffer& payload, size_t size) {
    payload_ = payload;
    payloadSize_ = size;
}

void RequestMessage::setStrategy(size_t strategy) {
    strategy_ = strategy;
}

void RequestMessage::setPolicyEpoch(size_t policyEpoch) {
    policyEpoch_ = policyEpoch;
}

const std::map<std::string, std::string>& RequestMessage::getTokens() const {
    return tokens_;
}

const std::string& RequestMessage::getUserToken() const {
    return userToken_;
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

size_t RequestMessage::getPolicyEpoch() const {
    return policyEpoch_;
}

void RequestMessage::Clear() {
    tokens_.clear();
    userToken_.clear();
    payload_.clear();
    payloadSize_ = 0;
    m_wire.reset();
    policyEpoch_ = 0;
}

ndn::Block RequestMessage::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::RequestMessageType);
    for (const auto& token : tokens_) {
        block.push_back(ndn::makeStringBlock(tlv::TokenType, token.first + "=" + token.second));
    }
    if (!userToken_.empty()) {
        block.push_back(ndn::makeStringBlock(tlv::UserTokenType, userToken_));
    }
    // payload
    ndn::Block payloadBlock = ndn::makeBinaryBlock(tlv::PayloadType, payload_.begin(), payload_.end());
    block.push_back(payloadBlock);
    // strategy
    ndn::Block strategyloadBlock = ndn::makeNonNegativeIntegerBlock(tlv::StrategyType, strategy_);
    block.push_back(strategyloadBlock);
    if (policyEpoch_ > 0) {
        block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, policyEpoch_));
    }
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
        else if (b.type() == tlv::UserTokenType) {
            userToken_ = ndn::readString(b);
        }
        else if (b.type() == tlv::PayloadType) {
            payload_ = ndn::Buffer(b.value(),b.value_size());
            payloadSize_ = b.value_size();
        }
        else if (b.type() == tlv::StrategyType) {
            strategy_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::VersionType) {
            policyEpoch_ = ndn::readNonNegativeInteger(b);
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

void ResponseMessage::setUserToken(const std::string& userToken) {
    userToken_ = userToken;
}

void ResponseMessage::setPayload(ndn::Buffer& payload, size_t size) {
    payload_ = payload;
    payloadSize_ = size;
}

void ResponseMessage::setPolicyEpoch(size_t policyEpoch) {
    policyEpoch_ = policyEpoch;
}

bool ResponseMessage::getStatus() const {
    return status_;
}

const std::string& ResponseMessage::getErrorInfo() const {
    return errorInfo_;
}

const std::string& ResponseMessage::getUserToken() const {
    return userToken_;
}

ndn::Buffer ResponseMessage::getPayload() const {
    return payload_;
}

size_t ResponseMessage::getPayloadSize() const {
    return payloadSize_;
}

size_t ResponseMessage::getPolicyEpoch() const {
    return policyEpoch_;
}

void ResponseMessage::Clear() {
    status_ = false;
    errorInfo_.clear();
    userToken_.clear();
    payload_.clear();
    payloadSize_ = 0;
    policyEpoch_ = 0;
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
    if (!userToken_.empty()) {
        block.push_back(ndn::makeStringBlock(tlv::UserTokenType, userToken_));
    }
    // 编码 payload
    ndn::Block payloadBlock = ndn::makeBinaryBlock(tlv::PayloadType, payload_.begin(), payload_.end());
    block.push_back(payloadBlock);
    if (policyEpoch_ > 0) {
        block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, policyEpoch_));
    }
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
        else if (b.type() == tlv::UserTokenType) {
            userToken_ = ndn::readString(b);
        }
        else if (b.type() == tlv::PayloadType) {
            payload_ = ndn::Buffer(b.value(),b.value_size());
            payloadSize_ = b.value_size();
        }
        else if (b.type() == tlv::VersionType) {
            policyEpoch_ = ndn::readNonNegativeInteger(b);
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

void RequestAckMessage::setUserToken(const std::string& userToken) {
    userToken_ = userToken;
}

void RequestAckMessage::setProviderToken(const std::string& providerToken) {
    providerToken_ = providerToken;
}

void RequestAckMessage::setPayload(ndn::Buffer& payload, size_t size) {
    payload_ = payload;
    payloadSize_ = size;
}

void RequestAckMessage::setPolicyEpoch(size_t policyEpoch) {
    policyEpoch_ = policyEpoch;
}

bool RequestAckMessage::getStatus() const {
    return status_;
}

const std::string& RequestAckMessage::getMessage() const {
    return message_;
}

const std::string& RequestAckMessage::getUserToken() const {
    return userToken_;
}

const std::string& RequestAckMessage::getProviderToken() const {
    return providerToken_;
}

ndn::Buffer RequestAckMessage::getPayload() const {
    return payload_;
}

size_t RequestAckMessage::getPayloadSize() const {
    return payloadSize_;
}

size_t RequestAckMessage::getPolicyEpoch() const {
    return policyEpoch_;
}

void RequestAckMessage::Clear() {
    status_ = false;
    message_.clear();
    userToken_.clear();
    providerToken_.clear();
    payload_.clear();
    payloadSize_ = 0;
    policyEpoch_ = 0;
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
    if (!userToken_.empty()) {
        block.push_back(ndn::makeStringBlock(tlv::UserTokenType, userToken_));
    }
    if (!providerToken_.empty()) {
        block.push_back(ndn::makeStringBlock(tlv::ProviderTokenType, providerToken_));
    }
    // 编码 payload
    ndn::Block payloadBlock = ndn::makeBinaryBlock(tlv::PayloadType, payload_.begin(), payload_.end());
    block.push_back(payloadBlock);
    if (policyEpoch_ > 0) {
        block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, policyEpoch_));
    }
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
        else if (b.type() == tlv::UserTokenType) {
            userToken_ = ndn::readString(b);
        }
        else if (b.type() == tlv::ProviderTokenType) {
            providerToken_ = ndn::readString(b);
        }
        else if (b.type() == tlv::PayloadType) {
            payload_ = ndn::Buffer(b.value(),b.value_size());
            payloadSize_ = b.value_size();
        }
        else if (b.type() == tlv::VersionType) {
            policyEpoch_ = ndn::readNonNegativeInteger(b);
        }
    }

    return true;
}

ServiceCoordinationMessage::ServiceCoordinationMessage() {}

void ServiceCoordinationMessage::setRequestIDs(const std::vector<std::string>& requestIDs) {
    requestIDs_ = requestIDs;
}

void ServiceCoordinationMessage::setProviderToken(const std::string& providerToken) {
    providerToken_ = providerToken;
}

void ServiceCoordinationMessage::setAssignmentPayload(const ndn::Buffer& payload) {
    assignmentPayload_ = payload;
}

void ServiceCoordinationMessage::setPolicyEpoch(size_t policyEpoch) {
    policyEpoch_ = policyEpoch;
}

const std::vector<std::string>& ServiceCoordinationMessage::getRequestIDs() const {
    return requestIDs_;
}

const std::string& ServiceCoordinationMessage::getProviderToken() const {
    return providerToken_;
}

const ndn::Buffer& ServiceCoordinationMessage::getAssignmentPayload() const {
    return assignmentPayload_;
}

size_t ServiceCoordinationMessage::getPolicyEpoch() const {
    return policyEpoch_;
}

void ServiceCoordinationMessage::Clear() {
    requestIDs_.clear();
    providerToken_.clear();
    assignmentPayload_.clear();
    policyEpoch_ = 0;
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
    if (!providerToken_.empty()) {
        block.push_back(ndn::makeStringBlock(tlv::ProviderTokenType, providerToken_));
    }
    if (!assignmentPayload_.empty()) {
        block.push_back(ndn::makeBinaryBlock(tlv::AssignmentPayloadType,
                                             assignmentPayload_.begin(),
                                             assignmentPayload_.end()));
    }
    if (policyEpoch_ > 0) {
        block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, policyEpoch_));
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
        else if (b.type() == tlv::ProviderTokenType) {
            providerToken_ = ndn::readString(b);
        }
        else if (b.type() == tlv::AssignmentPayloadType) {
            assignmentPayload_ = ndn::Buffer(b.value(), b.value_size());
        }
        else if (b.type() == tlv::VersionType) {
            policyEpoch_ = ndn::readNonNegativeInteger(b);
        }
    }

    return true;
}

CollaborationDataMessage::CollaborationDataMessage() {}

void CollaborationDataMessage::setKeyScope(const std::string& keyScope) {
    keyScope_ = keyScope;
}

void CollaborationDataMessage::setTopic(const ndn::Name& topic) {
    topic_ = topic;
}

void CollaborationDataMessage::setProducerRole(const std::string& role) {
    producerRole_ = role;
}

void CollaborationDataMessage::setSequence(uint64_t sequence) {
    sequence_ = sequence;
}

void CollaborationDataMessage::setPayload(const ndn::Buffer& payload) {
    payload_ = payload;
}

const std::string& CollaborationDataMessage::getKeyScope() const {
    return keyScope_;
}

const ndn::Name& CollaborationDataMessage::getTopic() const {
    return topic_;
}

const std::string& CollaborationDataMessage::getProducerRole() const {
    return producerRole_;
}

uint64_t CollaborationDataMessage::getSequence() const {
    return sequence_;
}

const ndn::Buffer& CollaborationDataMessage::getPayload() const {
    return payload_;
}

void CollaborationDataMessage::Clear() {
    keyScope_.clear();
    topic_.clear();
    producerRole_.clear();
    sequence_ = 0;
    payload_.clear();
    m_wire.reset();
}

ndn::Block CollaborationDataMessage::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::CollaborationDataMessageType);
    block.push_back(ndn::makeStringBlock(tlv::KeyScopeType, keyScope_));
    block.push_back(ndn::makeStringBlock(tlv::TopicType, topic_.toUri()));
    block.push_back(ndn::makeStringBlock(tlv::ProducerRoleType, producerRole_));
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::SequenceType, sequence_));
    block.push_back(ndn::makeBinaryBlock(tlv::PayloadType,
                                         payload_.begin(),
                                         payload_.end()));
    block.encode();
    m_wire = block;
    return m_wire;
}

bool CollaborationDataMessage::WireDecode(const ndn::Block& block) {
    Clear();
    if (block.type() != tlv::CollaborationDataMessageType) {
        return false;
    }
    block.parse();
    for (auto b : block.elements()) {
        if (b.type() == tlv::KeyScopeType) {
            keyScope_ = ndn::readString(b);
        }
        else if (b.type() == tlv::TopicType) {
            topic_ = ndn::Name(ndn::readString(b));
        }
        else if (b.type() == tlv::ProducerRoleType) {
            producerRole_ = ndn::readString(b);
        }
        else if (b.type() == tlv::SequenceType) {
            sequence_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::PayloadType) {
            payload_ = ndn::Buffer(b.value(), b.value_size());
        }
    }
    return true;
}

HybridMessageEnvelope::HybridMessageEnvelope() {}

void HybridMessageEnvelope::setVersion(size_t version) { version_ = version; }
void HybridMessageEnvelope::setAlgorithm(const std::string& algorithm) { algorithm_ = algorithm; }
void HybridMessageEnvelope::setKeyId(const std::string& keyId) { keyId_ = keyId; }
void HybridMessageEnvelope::setEpochId(const std::string& epochId) { epochId_ = epochId; }
void HybridMessageEnvelope::setMessageType(const std::string& messageType) { messageType_ = messageType; }
void HybridMessageEnvelope::setNonce(const ndn::Buffer& nonce) { nonce_ = nonce; }
void HybridMessageEnvelope::setCipherText(const ndn::Buffer& cipherText) { cipherText_ = cipherText; }
void HybridMessageEnvelope::setAuthTag(const ndn::Buffer& authTag) { authTag_ = authTag; }
void HybridMessageEnvelope::setWrappedMessageKey(const ndn::Buffer& wrappedMessageKey) { wrappedMessageKey_ = wrappedMessageKey; }

size_t HybridMessageEnvelope::getVersion() const { return version_; }
const std::string& HybridMessageEnvelope::getAlgorithm() const { return algorithm_; }
const std::string& HybridMessageEnvelope::getKeyId() const { return keyId_; }
const std::string& HybridMessageEnvelope::getEpochId() const { return epochId_; }
const std::string& HybridMessageEnvelope::getMessageType() const { return messageType_; }
const ndn::Buffer& HybridMessageEnvelope::getNonce() const { return nonce_; }
const ndn::Buffer& HybridMessageEnvelope::getCipherText() const { return cipherText_; }
const ndn::Buffer& HybridMessageEnvelope::getAuthTag() const { return authTag_; }
const ndn::Buffer& HybridMessageEnvelope::getWrappedMessageKey() const { return wrappedMessageKey_; }
bool HybridMessageEnvelope::hasWrappedMessageKey() const { return !wrappedMessageKey_.empty(); }

void HybridMessageEnvelope::Clear() {
    version_ = 1;
    algorithm_ = "AES-256-GCM";
    keyId_.clear();
    epochId_.clear();
    messageType_.clear();
    nonce_.clear();
    cipherText_.clear();
    authTag_.clear();
    wrappedMessageKey_.clear();
    m_wire.reset();
}

ndn::Block HybridMessageEnvelope::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::HybridMessageEnvelopeType);
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, version_));
    block.push_back(ndn::makeStringBlock(tlv::AlgorithmType, algorithm_));
    block.push_back(ndn::makeStringBlock(tlv::KeyIdType, keyId_));
    block.push_back(ndn::makeStringBlock(tlv::EpochIdType, epochId_));
    block.push_back(ndn::makeStringBlock(tlv::MessageTypeType, messageType_));
    block.push_back(ndn::makeBinaryBlock(tlv::NonceType, nonce_.begin(), nonce_.end()));
    block.push_back(ndn::makeBinaryBlock(tlv::CipherTextType, cipherText_.begin(), cipherText_.end()));
    block.push_back(ndn::makeBinaryBlock(tlv::AuthTagType, authTag_.begin(), authTag_.end()));
    if (!wrappedMessageKey_.empty()) {
        block.push_back(ndn::makeBinaryBlock(tlv::WrappedMessageKeyType,
                                             wrappedMessageKey_.begin(),
                                             wrappedMessageKey_.end()));
    }
    block.encode();
    m_wire = block;
    return m_wire;
}

bool HybridMessageEnvelope::WireDecode(const ndn::Block& block) {
    Clear();
    if (block.type() != tlv::HybridMessageEnvelopeType) {
        return false;
    }
    block.parse();
    for (auto b : block.elements()) {
        if (b.type() == tlv::VersionType) {
            version_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::AlgorithmType) {
            algorithm_ = ndn::readString(b);
        }
        else if (b.type() == tlv::KeyIdType) {
            keyId_ = ndn::readString(b);
        }
        else if (b.type() == tlv::EpochIdType) {
            epochId_ = ndn::readString(b);
        }
        else if (b.type() == tlv::MessageTypeType) {
            messageType_ = ndn::readString(b);
        }
        else if (b.type() == tlv::NonceType) {
            nonce_ = ndn::Buffer(b.value(), b.value_size());
        }
        else if (b.type() == tlv::CipherTextType) {
            cipherText_ = ndn::Buffer(b.value(), b.value_size());
        }
        else if (b.type() == tlv::AuthTagType) {
            authTag_ = ndn::Buffer(b.value(), b.value_size());
        }
        else if (b.type() == tlv::WrappedMessageKeyType) {
            wrappedMessageKey_ = ndn::Buffer(b.value(), b.value_size());
        }
    }
    return version_ == 1 && algorithm_ == "AES-256-GCM" &&
           !keyId_.empty() && !epochId_.empty() && !nonce_.empty() &&
           !cipherText_.empty() && !authTag_.empty();
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

void PermissionResponse::setPolicyEpoch(size_t policyEpoch) {
    policyEpoch_ = policyEpoch;
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

size_t PermissionResponse::getPolicyEpoch() const {
    return policyEpoch_;
}

const std::vector<PermissionEntry>& PermissionResponse::getEntries() const {
    return entries_;
}

std::string PermissionResponse::toString() const {
    std::string result = "PermissionResponse{targetIdentity=" + targetIdentity_ +
                         ", permissionKind=" + std::to_string(permissionKind_) +
                         ", policyEpoch=" + std::to_string(policyEpoch_) +
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
    policyEpoch_ = 1;
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
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, policyEpoch_));
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
        else if (b.type() == tlv::VersionType) {
            policyEpoch_ = ndn::readNonNegativeInteger(b);
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

PolicyManifest::PolicyManifest() {}

void PolicyManifest::setPolicyEpoch(size_t policyEpoch) { policyEpoch_ = policyEpoch; }
void PolicyManifest::setValidFromMs(uint64_t validFromMs) { validFromMs_ = validFromMs; }
void PolicyManifest::setGracePeriodMs(uint64_t gracePeriodMs) { gracePeriodMs_ = gracePeriodMs; }
void PolicyManifest::setRequiredKeyEpoch(size_t requiredKeyEpoch) { requiredKeyEpoch_ = requiredKeyEpoch; }

size_t PolicyManifest::getPolicyEpoch() const { return policyEpoch_; }
uint64_t PolicyManifest::getValidFromMs() const { return validFromMs_; }
uint64_t PolicyManifest::getGracePeriodMs() const { return gracePeriodMs_; }
size_t PolicyManifest::getRequiredKeyEpoch() const { return requiredKeyEpoch_; }

std::string PolicyManifest::toString() const {
    return "PolicyManifest{policyEpoch=" + std::to_string(policyEpoch_) +
           ", validFromMs=" + std::to_string(validFromMs_) +
           ", gracePeriodMs=" + std::to_string(gracePeriodMs_) +
           ", requiredKeyEpoch=" + std::to_string(requiredKeyEpoch_) + "}";
}

void PolicyManifest::Clear() {
    policyEpoch_ = 1;
    validFromMs_ = 0;
    gracePeriodMs_ = 0;
    requiredKeyEpoch_ = 1;
    m_wire.reset();
}

ndn::Block PolicyManifest::WireEncode() const {
    if (m_wire.hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::PolicyManifestType);
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, policyEpoch_));
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::ValidFromType, validFromMs_));
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::GracePeriodMsType, gracePeriodMs_));
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::RequiredKeyEpochType, requiredKeyEpoch_));
    block.encode();
    m_wire = block;
    return m_wire;
}

bool PolicyManifest::WireDecode(const ndn::Block& block) {
    Clear();
    if (block.type() != tlv::PolicyManifestType) {
        return false;
    }
    block.parse();
    for (auto b : block.elements()) {
        if (b.type() == tlv::VersionType) {
            policyEpoch_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::ValidFromType) {
            validFromMs_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::GracePeriodMsType) {
            gracePeriodMs_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::RequiredKeyEpochType) {
            requiredKeyEpoch_ = ndn::readNonNegativeInteger(b);
        }
    }
    return policyEpoch_ > 0 && requiredKeyEpoch_ > 0;
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
