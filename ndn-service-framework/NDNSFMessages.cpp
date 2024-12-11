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
    ndn::Block block(tlv::ServiceCoordinationMessageType);
    for (const auto& id : requestIDs_) {
        block.push_back(ndn::makeStringBlock(tlv::RequestIDType, id));
    }
    block.encode();
    m_wire = block;
    return m_wire;
}

bool ServiceCoordinationMessage::WireDecode(const ndn::Block& block) {
    Clear(); // 清除已初始化的值

    if (block.type() != tlv::ServiceCoordinationMessageType) {
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

} // namespace ndn_service_framework
