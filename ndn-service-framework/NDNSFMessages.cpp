#include "NDNSFMessages.hpp"
#include "HybridMessageCrypto.hpp"

#include <algorithm>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>

namespace ndn_service_framework {

namespace {

ndn::Block
makePayloadBlock(const uint8_t* payload, size_t size)
{
    if (size == 0) {
        ndn::Block block(tlv::PayloadType);
        block.encode();
        return block;
    }
    auto block = ndn::makeBinaryBlock(tlv::PayloadType, payload, payload + size);
    block.encode();
    return block;
}

std::shared_ptr<const ndn::Block>
makePayloadBlockPtr(const uint8_t* payload, size_t size)
{
    return std::make_shared<const ndn::Block>(makePayloadBlock(payload, size));
}

ndn::Block
makePayloadBlockFromBuffer(const ndn::Buffer& payload, size_t size)
{
    const auto boundedSize = std::min(size, payload.size());
    return makePayloadBlock(payload.data(), boundedSize);
}

std::shared_ptr<const ndn::Block>
clonePayloadBlock(const ndn::Block& payloadBlock)
{
    if (!payloadBlock.isValid()) {
        return makePayloadBlockPtr(nullptr, 0);
    }
    if (payloadBlock.type() != tlv::PayloadType) {
        throw std::invalid_argument("message payload block has unexpected TLV type");
    }
    return makePayloadBlockPtr(payloadBlock.value(), payloadBlock.value_size());
}

const ndn::Block&
emptyPayloadBlock()
{
    static const auto payloadBlock = makePayloadBlockPtr(nullptr, 0);
    return *payloadBlock;
}

const ndn::Block&
payloadBlockOrEmpty(const std::shared_ptr<const ndn::Block>& payloadBlock)
{
    return payloadBlock ? *payloadBlock : emptyPayloadBlock();
}

ndn::Buffer
payloadValueAsBuffer(const std::shared_ptr<const ndn::Block>& payloadBlock)
{
    if (!payloadBlock || !payloadBlock->isValid()) {
        return {};
    }
    return ndn::Buffer(payloadBlock->value(), payloadBlock->value_size());
}

} // namespace

DeploymentControlMessage::DeploymentControlMessage(uint32_t messageType)
  : messageType_(messageType)
{
}

void DeploymentControlMessage::setVersion(uint64_t version)
{
    version_ = version;
    wire_.reset();
}

uint64_t DeploymentControlMessage::getVersion() const { return version_; }

void DeploymentControlMessage::setField(const std::string& name, const std::string& value)
{
    if (name.empty() || name.size() > MAX_FIELD_NAME || value.size() > MAX_FIELD_VALUE) {
        throw std::invalid_argument("deployment control field exceeds bounds");
    }
    if (!hasField(name) && fields_.size() >= MAX_FIELDS) {
        throw std::invalid_argument("too many deployment control fields");
    }
    fields_[name] = value;
    wire_.reset();
}

bool DeploymentControlMessage::hasField(const std::string& name) const
{
    return fields_.find(name) != fields_.end();
}

const std::string& DeploymentControlMessage::getField(const std::string& name) const
{
    const auto it = fields_.find(name);
    if (it == fields_.end()) {
        throw std::out_of_range("missing deployment control field: " + name);
    }
    return it->second;
}

const std::map<std::string, std::string>& DeploymentControlMessage::getFields() const
{
    return fields_;
}

uint32_t DeploymentControlMessage::getMessageType() const { return messageType_; }

void DeploymentControlMessage::Clear()
{
    version_ = VERSION;
    fields_.clear();
    wire_.reset();
}

ndn::Block DeploymentControlMessage::WireEncode() const
{
    if (version_ != VERSION) {
        throw std::invalid_argument("unsupported deployment control version");
    }
    ndn::Block block(messageType_);
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, version_));
    for (const auto& [name, value] : fields_) {
        ndn::Block field(tlv::DeploymentControlFieldType);
        field.push_back(ndn::makeStringBlock(tlv::DeploymentControlFieldNameType, name));
        field.push_back(ndn::makeBinaryBlock(tlv::DeploymentControlFieldValueType,
                                             reinterpret_cast<const uint8_t*>(value.data()),
                                             reinterpret_cast<const uint8_t*>(value.data()) + value.size()));
        field.encode();
        block.push_back(field);
    }
    block.encode();
    if (block.size() > MAX_WIRE_SIZE) {
        throw std::invalid_argument("deployment control message exceeds wire bound");
    }
    wire_ = block;
    return wire_;
}

bool DeploymentControlMessage::WireDecode(const ndn::Block& block)
{
    try {
        if (block.type() != messageType_ || block.size() > MAX_WIRE_SIZE) return false;
        block.parse();
        const auto& elements = block.elements();
        if (elements.empty() || elements.front().type() != tlv::VersionType ||
            elements.size() - 1 > MAX_FIELDS) return false;
        DeploymentControlMessage decoded(messageType_);
        decoded.version_ = ndn::readNonNegativeInteger(elements.front());
        if (decoded.version_ != VERSION) return false;
        std::string previousName;
        for (size_t i = 1; i < elements.size(); ++i) {
            const auto& item = elements[i];
            if (item.type() != tlv::DeploymentControlFieldType) return false;
            item.parse();
            if (item.elements().size() != 2 ||
                item.elements()[0].type() != tlv::DeploymentControlFieldNameType ||
                item.elements()[1].type() != tlv::DeploymentControlFieldValueType) return false;
            const auto name = ndn::readString(item.elements()[0]);
            const auto& valueBlock = item.elements()[1];
            if (name.empty() || name.size() > MAX_FIELD_NAME ||
                valueBlock.value_size() > MAX_FIELD_VALUE ||
                (!previousName.empty() && name <= previousName)) return false;
            decoded.fields_.emplace(name, std::string(
                reinterpret_cast<const char*>(valueBlock.value()), valueBlock.value_size()));
            previousName = name;
        }
        const auto canonical = decoded.WireEncode();
        if (canonical.size() != block.size() ||
            !std::equal(canonical.begin(), canonical.end(), block.begin())) return false;
        version_ = decoded.version_;
        fields_ = std::move(decoded.fields_);
        wire_ = canonical;
        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

std::string DeploymentControlMessage::computeDigest() const
{
    const auto wire = WireEncode();
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(&*wire.begin(), wire.size(), digest);
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (unsigned char byte : digest) os << std::setw(2) << unsigned(byte);
    return os.str();
}

DeploymentIntent::DeploymentIntent() : DeploymentControlMessage(tlv::DeploymentIntentType) {}
ProviderCapabilityOffer::ProviderCapabilityOffer() : DeploymentControlMessage(tlv::ProviderCapabilityOfferType) {}
DeploymentPlan::DeploymentPlan() : DeploymentControlMessage(tlv::DeploymentPlanType) {}
ProviderReadyMessage::ProviderReadyMessage() : DeploymentControlMessage(tlv::ProviderReadyMessageType) {}
ReadyAcknowledgement::ReadyAcknowledgement() : DeploymentControlMessage(tlv::ReadyAcknowledgementType) {}
ExecutionActivateMessage::ExecutionActivateMessage() : DeploymentControlMessage(tlv::ExecutionActivateMessageType) {}
SecureStatusQuery::SecureStatusQuery() : DeploymentControlMessage(tlv::SecureStatusQueryType) {}
SecureStatusSnapshot::SecureStatusSnapshot() : DeploymentControlMessage(tlv::SecureStatusSnapshotType) {}
RequestCapabilities::RequestCapabilities() : DeploymentControlMessage(tlv::RequestCapabilitiesType) {}
EncryptedRequestInput::EncryptedRequestInput() : DeploymentControlMessage(tlv::EncryptedRequestInputType) {}
SelectionInputKeyOffer::SelectionInputKeyOffer() : DeploymentControlMessage(tlv::SelectionInputKeyOfferType) {}
SelectionInputKeyGrant::SelectionInputKeyGrant() : DeploymentControlMessage(tlv::SelectionInputKeyGrantType) {}
ReservationLease::ReservationLease() : DeploymentControlMessage(tlv::ReservationLeaseType) {}
SelectionDecision::SelectionDecision() : DeploymentControlMessage(tlv::SelectionDecisionType) {}
SelectionDecisionReceipt::SelectionDecisionReceipt() : DeploymentControlMessage(tlv::SelectionDecisionReceiptType) {}
RecipientEncryptedAssignment::RecipientEncryptedAssignment() : DeploymentControlMessage(tlv::RecipientEncryptedAssignmentType) {}
StageInputEvidence::StageInputEvidence() : DeploymentControlMessage(tlv::StageInputEvidenceType) {}
StageAbort::StageAbort() : DeploymentControlMessage(tlv::StageAbortType) {}
SelectionDecisionTombstone::SelectionDecisionTombstone() : DeploymentControlMessage(tlv::SelectionDecisionTombstoneType) {}

RequestMessage::RequestMessage() {}

RequestMessage::RequestMessage(const RequestMessage& other)
{
    *this = other;
}

RequestMessage&
RequestMessage::operator=(const RequestMessage& other)
{
    if (this != &other) {
        tokens_ = other.tokens_;
        userToken_ = other.userToken_;
        providerToken_ = other.providerToken_;
        payloadBlock_ = clonePayloadBlock(other.getPayloadBlock());
        payloadSize_ = other.payloadSize_;
        strategy_ = other.strategy_;
        requestMode_ = other.requestMode_;
        targetProvider_ = other.targetProvider_;
        policyEpoch_ = other.policyEpoch_;
        deploymentIntent_ = other.deploymentIntent_;
        requestCapabilities_ = other.requestCapabilities_;
        encryptedRequestInput_ = other.encryptedRequestInput_;
        m_wire.reset();
    }
    return *this;
}

void RequestMessage::setTokens(const std::map<std::string, std::string>& tokens) {
    tokens_ = tokens;
}

void RequestMessage::setUserToken(const std::string& userToken) {
    userToken_ = userToken;
}

void RequestMessage::setProviderToken(const std::string& providerToken) {
    providerToken_ = providerToken;
}

void RequestMessage::setPayload(ndn::Buffer& payload, size_t size) {
    payloadBlock_ = std::make_shared<const ndn::Block>(makePayloadBlockFromBuffer(payload, size));
    payloadSize_ = payloadBlock_->value_size();
}

void RequestMessage::setPayloadBlock(const ndn::Block& payloadBlock) {
    payloadBlock_ = clonePayloadBlock(payloadBlock);
    payloadSize_ = payloadBlock_->value_size();
}

void RequestMessage::setStrategy(size_t strategy) {
    strategy_ = strategy;
}

void RequestMessage::setRequestMode(size_t requestMode) {
    requestMode_ = requestMode;
}

void RequestMessage::setTargetProvider(const ndn::Name& targetProvider) {
    targetProvider_ = targetProvider;
}

void RequestMessage::setPolicyEpoch(size_t policyEpoch) {
    policyEpoch_ = policyEpoch;
}

void RequestMessage::setDeploymentIntent(const DeploymentIntent& intent) {
    if (intent.getVersion() != DeploymentControlMessage::VERSION) {
        throw std::invalid_argument("unsupported deployment intent version");
    }
    deploymentIntent_ = intent;
    m_wire.reset();
}
void RequestMessage::setRequestCapabilities(const RequestCapabilities& capabilities) {
    requestCapabilities_ = capabilities;
    m_wire.reset();
}
void RequestMessage::setEncryptedRequestInput(const EncryptedRequestInput& input) {
    encryptedRequestInput_ = input;
    m_wire.reset();
}
bool RequestMessage::hasDeploymentIntent() const { return deploymentIntent_.has_value(); }
bool RequestMessage::hasRequestCapabilities() const { return requestCapabilities_.has_value(); }
bool RequestMessage::hasEncryptedRequestInput() const { return encryptedRequestInput_.has_value(); }
const DeploymentIntent& RequestMessage::getDeploymentIntent() const {
    if (!deploymentIntent_) throw std::logic_error("request has no deployment intent");
    return *deploymentIntent_;
}
const RequestCapabilities& RequestMessage::getRequestCapabilities() const {
    if (!requestCapabilities_) throw std::logic_error("request has no capabilities");
    return *requestCapabilities_;
}
const EncryptedRequestInput& RequestMessage::getEncryptedRequestInput() const {
    if (!encryptedRequestInput_) throw std::logic_error("request has no encrypted input");
    return *encryptedRequestInput_;
}

const std::map<std::string, std::string>& RequestMessage::getTokens() const {
    return tokens_;
}

const std::string& RequestMessage::getUserToken() const {
    return userToken_;
}

const std::string& RequestMessage::getProviderToken() const {
    return providerToken_;
}

ndn::Buffer RequestMessage::getPayload() const {
    return payloadValueAsBuffer(payloadBlock_);
}

const ndn::Block& RequestMessage::getPayloadBlock() const {
    return payloadBlockOrEmpty(payloadBlock_);
}

size_t RequestMessage::getPayloadSize() const {
    return payloadSize_;
}

size_t RequestMessage::getStrategy() const {
    return strategy_;
}

size_t RequestMessage::getRequestMode() const {
    return requestMode_;
}

const ndn::Name& RequestMessage::getTargetProvider() const {
    return targetProvider_;
}

size_t RequestMessage::getPolicyEpoch() const {
    return policyEpoch_;
}

void RequestMessage::Clear() {
    tokens_.clear();
    userToken_.clear();
    providerToken_.clear();
    payloadBlock_.reset();
    payloadSize_ = 0;
    strategy_ = tlv::FirstResponding;
    requestMode_ = tlv::NormalRequest;
    targetProvider_.clear();
    m_wire.reset();
    policyEpoch_ = 0;
    deploymentIntent_.reset();
    requestCapabilities_.reset();
    encryptedRequestInput_.reset();
}

ndn::Block RequestMessage::WireEncode() const {
    if (m_wire && m_wire->hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::RequestMessageType);
    for (const auto& token : tokens_) {
        block.push_back(ndn::makeStringBlock(tlv::TokenType, token.first + "=" + token.second));
    }
    if (!userToken_.empty()) {
        block.push_back(ndn::makeStringBlock(tlv::UserTokenType, userToken_));
    }
    if (!providerToken_.empty()) {
        block.push_back(ndn::makeStringBlock(tlv::ProviderTokenType, providerToken_));
    }
    // payload
    block.push_back(payloadBlockOrEmpty(payloadBlock_));
    // strategy
    ndn::Block strategyloadBlock = ndn::makeNonNegativeIntegerBlock(tlv::StrategyType, strategy_);
    block.push_back(strategyloadBlock);
    if (requestMode_ != tlv::NormalRequest) {
        block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::RequestModeType, requestMode_));
    }
    if (!targetProvider_.empty()) {
        block.push_back(ndn::makeStringBlock(tlv::ProviderNameType, targetProvider_.toUri()));
    }
    if (policyEpoch_ > 0) {
        block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, policyEpoch_));
    }
    if (deploymentIntent_) block.push_back(deploymentIntent_->WireEncode());
    if (requestCapabilities_) block.push_back(requestCapabilities_->WireEncode());
    if (encryptedRequestInput_) block.push_back(encryptedRequestInput_->WireEncode());
    block.encode();
    m_wire = std::make_shared<const ndn::Block>(block);
    return *m_wire;
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
        else if (b.type() == tlv::ProviderTokenType) {
            providerToken_ = ndn::readString(b);
        }
        else if (b.type() == tlv::PayloadType) {
            payloadBlock_ = clonePayloadBlock(b);
            payloadSize_ = payloadBlock_->value_size();
        }
        else if (b.type() == tlv::StrategyType) {
            strategy_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::RequestModeType) {
            requestMode_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::ProviderNameType) {
            targetProvider_ = ndn::Name(ndn::readString(b));
        }
        else if (b.type() == tlv::VersionType) {
            policyEpoch_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::DeploymentIntentType) {
            DeploymentIntent intent;
            if (!intent.WireDecode(b)) return false;
            deploymentIntent_ = std::move(intent);
        }
        else if (b.type() == tlv::RequestCapabilitiesType) {
            RequestCapabilities capabilities;
            if (!capabilities.WireDecode(b)) return false;
            requestCapabilities_ = std::move(capabilities);
        }
        else if (b.type() == tlv::EncryptedRequestInputType) {
            EncryptedRequestInput input;
            if (!input.WireDecode(b)) return false;
            encryptedRequestInput_ = std::move(input);
        }
    }

    return true;
}

ResponseMessage::ResponseMessage() {}

ResponseMessage::ResponseMessage(const ResponseMessage& other)
{
    *this = other;
}

ResponseMessage&
ResponseMessage::operator=(const ResponseMessage& other)
{
    if (this != &other) {
        status_ = other.status_;
        errorInfo_ = other.errorInfo_;
        tokens_ = other.tokens_;
        userToken_ = other.userToken_;
        payloadBlock_ = clonePayloadBlock(other.getPayloadBlock());
        payloadSize_ = other.payloadSize_;
        policyEpoch_ = other.policyEpoch_;
        dataName_ = other.dataName_;
        signerCertificate_ = other.signerCertificate_;
        wireDigest_ = other.wireDigest_;
        m_wire.reset();
    }
    return *this;
}

void ResponseMessage::setStatus(bool status) {
    status_ = status;
}

void ResponseMessage::setErrorInfo(const std::string& errorInfo) {
    errorInfo_ = errorInfo;
}

void ResponseMessage::setTokens(const std::map<std::string, std::string>& tokens) {
    tokens_ = tokens;
}

void ResponseMessage::setUserToken(const std::string& userToken) {
    userToken_ = userToken;
}

void ResponseMessage::setPayload(ndn::Buffer& payload, size_t size) {
    payloadBlock_ = std::make_shared<const ndn::Block>(makePayloadBlockFromBuffer(payload, size));
    payloadSize_ = payloadBlock_->value_size();
}

void ResponseMessage::setPayloadBlock(const ndn::Block& payloadBlock) {
    payloadBlock_ = clonePayloadBlock(payloadBlock);
    payloadSize_ = payloadBlock_->value_size();
}

void ResponseMessage::setPolicyEpoch(size_t policyEpoch) {
    policyEpoch_ = policyEpoch;
}

void ResponseMessage::setAuthenticatedTransportEvidence(
    const std::string& dataName,
    const std::string& signerCertificate,
    const std::string& wireDigest) {
    dataName_ = dataName;
    signerCertificate_ = signerCertificate;
    wireDigest_ = wireDigest;
}

bool ResponseMessage::getStatus() const {
    return status_;
}

const std::string& ResponseMessage::getErrorInfo() const {
    return errorInfo_;
}

const std::map<std::string, std::string>& ResponseMessage::getTokens() const {
    return tokens_;
}

const std::string& ResponseMessage::getUserToken() const {
    return userToken_;
}

ndn::Buffer ResponseMessage::getPayload() const {
    return payloadValueAsBuffer(payloadBlock_);
}

const ndn::Block& ResponseMessage::getPayloadBlock() const {
    return payloadBlockOrEmpty(payloadBlock_);
}

size_t ResponseMessage::getPayloadSize() const {
    return payloadSize_;
}

size_t ResponseMessage::getPolicyEpoch() const {
    return policyEpoch_;
}

const std::string& ResponseMessage::getDataName() const {
    return dataName_;
}

const std::string& ResponseMessage::getSignerCertificate() const {
    return signerCertificate_;
}

const std::string& ResponseMessage::getWireDigest() const {
    return wireDigest_;
}

void ResponseMessage::Clear() {
    status_ = false;
    errorInfo_.clear();
    tokens_.clear();
    userToken_.clear();
    payloadBlock_.reset();
    payloadSize_ = 0;
    policyEpoch_ = 0;
    dataName_.clear();
    signerCertificate_.clear();
    wireDigest_.clear();
    m_wire.reset();
}

ndn::Block ResponseMessage::WireEncode() const {
    if (m_wire && m_wire->hasWire()) {
        m_wire.reset();
    }
    ndn::Block block(tlv::ResponseMessageType);
    // 编码 status
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StatusType, static_cast<int>(status_)));
    // 编码 errorInfo
    block.push_back(ndn::makeStringBlock(tlv::ErrorInfoType, errorInfo_));
    for (const auto& token : tokens_) {
        block.push_back(ndn::makeStringBlock(tlv::TokenType, token.first + "=" + token.second));
    }
    if (!userToken_.empty()) {
        block.push_back(ndn::makeStringBlock(tlv::UserTokenType, userToken_));
    }
    // 编码 payload
    block.push_back(payloadBlockOrEmpty(payloadBlock_));
    if (policyEpoch_ > 0) {
        block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, policyEpoch_));
    }
    block.encode();
    m_wire = std::make_shared<const ndn::Block>(block);
    return *m_wire;
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
        else if (b.type() == tlv::TokenType) {
            std::string tokenStr = ndn::readString(b);
            size_t pos = tokenStr.find('=');
            if (pos != std::string::npos) {
                tokens_[tokenStr.substr(0, pos)] = tokenStr.substr(pos + 1);
            }
        }
        else if (b.type() == tlv::UserTokenType) {
            userToken_ = ndn::readString(b);
        }
        else if (b.type() == tlv::PayloadType) {
            payloadBlock_ = clonePayloadBlock(b);
            payloadSize_ = payloadBlock_->value_size();
        }
        else if (b.type() == tlv::VersionType) {
            policyEpoch_ = ndn::readNonNegativeInteger(b);
        }
    }

    return true;
}

RequestAckMessage::RequestAckMessage() {}

RequestAckMessage::RequestAckMessage(const RequestAckMessage& other)
{
    *this = other;
}

RequestAckMessage&
RequestAckMessage::operator=(const RequestAckMessage& other)
{
    if (this != &other) {
        status_ = other.status_;
        message_ = other.message_;
        userToken_ = other.userToken_;
        providerToken_ = other.providerToken_;
        payloadBlock_ = clonePayloadBlock(other.getPayloadBlock());
        payloadSize_ = other.payloadSize_;
        policyEpoch_ = other.policyEpoch_;
        providerCapabilityOffer_ = other.providerCapabilityOffer_;
        selectionInputKeyOffer_ = other.selectionInputKeyOffer_;
        reservationLease_ = other.reservationLease_;
        m_wire.reset();
    }
    return *this;
}

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
    payloadBlock_ = std::make_shared<const ndn::Block>(makePayloadBlockFromBuffer(payload, size));
    payloadSize_ = payloadBlock_->value_size();
}

void RequestAckMessage::setPayloadBlock(const ndn::Block& payloadBlock) {
    payloadBlock_ = clonePayloadBlock(payloadBlock);
    payloadSize_ = payloadBlock_->value_size();
}

void RequestAckMessage::setPolicyEpoch(size_t policyEpoch) {
    policyEpoch_ = policyEpoch;
}

void RequestAckMessage::setProviderCapabilityOffer(const ProviderCapabilityOffer& offer) {
    providerCapabilityOffer_ = offer;
    m_wire.reset();
}
bool RequestAckMessage::hasProviderCapabilityOffer() const { return providerCapabilityOffer_.has_value(); }
const ProviderCapabilityOffer& RequestAckMessage::getProviderCapabilityOffer() const {
    if (!providerCapabilityOffer_) throw std::logic_error("ACK has no provider capability offer");
    return *providerCapabilityOffer_;
}
void RequestAckMessage::setSelectionInputKeyOffer(const SelectionInputKeyOffer& offer) {
    selectionInputKeyOffer_ = offer;
    m_wire.reset();
}
void RequestAckMessage::setReservationLease(const ReservationLease& lease) {
    reservationLease_ = lease;
    m_wire.reset();
}
bool RequestAckMessage::hasSelectionInputKeyOffer() const { return selectionInputKeyOffer_.has_value(); }
bool RequestAckMessage::hasReservationLease() const { return reservationLease_.has_value(); }
const SelectionInputKeyOffer& RequestAckMessage::getSelectionInputKeyOffer() const {
    if (!selectionInputKeyOffer_) throw std::logic_error("ACK has no selection input key offer");
    return *selectionInputKeyOffer_;
}
const ReservationLease& RequestAckMessage::getReservationLease() const {
    if (!reservationLease_) throw std::logic_error("ACK has no reservation lease");
    return *reservationLease_;
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
    return payloadValueAsBuffer(payloadBlock_);
}

const ndn::Block& RequestAckMessage::getPayloadBlock() const {
    return payloadBlockOrEmpty(payloadBlock_);
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
    payloadBlock_.reset();
    payloadSize_ = 0;
    policyEpoch_ = 0;
    providerCapabilityOffer_.reset();
    selectionInputKeyOffer_.reset();
    reservationLease_.reset();
    m_wire.reset();
}

ndn::Block RequestAckMessage::WireEncode() const {
    if (m_wire && m_wire->hasWire()) {
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
    block.push_back(payloadBlockOrEmpty(payloadBlock_));
    if (policyEpoch_ > 0) {
        block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, policyEpoch_));
    }
    if (providerCapabilityOffer_) block.push_back(providerCapabilityOffer_->WireEncode());
    if (selectionInputKeyOffer_) block.push_back(selectionInputKeyOffer_->WireEncode());
    if (reservationLease_) block.push_back(reservationLease_->WireEncode());
    block.encode();
    m_wire = std::make_shared<const ndn::Block>(block);
    return *m_wire;
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
            payloadBlock_ = clonePayloadBlock(b);
            payloadSize_ = payloadBlock_->value_size();
        }
        else if (b.type() == tlv::VersionType) {
            policyEpoch_ = ndn::readNonNegativeInteger(b);
        }
        else if (b.type() == tlv::ProviderCapabilityOfferType) {
            ProviderCapabilityOffer offer;
            if (!offer.WireDecode(b)) return false;
            providerCapabilityOffer_ = std::move(offer);
        }
        else if (b.type() == tlv::SelectionInputKeyOfferType) {
            SelectionInputKeyOffer offer;
            if (!offer.WireDecode(b)) return false;
            selectionInputKeyOffer_ = std::move(offer);
        }
        else if (b.type() == tlv::ReservationLeaseType) {
            ReservationLease lease;
            if (!lease.WireDecode(b)) return false;
            reservationLease_ = std::move(lease);
        }
    }

    return true;
}

ServiceSelectionMessage::ServiceSelectionMessage() {}

ndn::Buffer
encodeOpaqueAssignmentSet(const std::vector<ndn::Buffer>& assignments)
{
    if (assignments.empty()) {
        return {};
    }
    if (assignments.size() == 1) {
        return assignments.front();
    }
    if (assignments.size() > 256) {
        throw std::length_error("opaque assignment set exceeds item bound");
    }
    ndn::Block block(tlv::OpaqueAssignmentSetType);
    size_t total = 0;
    for (const auto& assignment : assignments) {
        if (assignment.size() > 1024 * 1024 ||
            total > 4 * 1024 * 1024 - assignment.size()) {
            throw std::length_error("opaque assignment set exceeds byte bound");
        }
        total += assignment.size();
        block.push_back(ndn::makeBinaryBlock(
            tlv::OpaqueAssignmentItemType,
            {assignment.data(), assignment.size()}));
    }
    block.encode();
    return ndn::Buffer(block.data(), block.size());
}

std::vector<ndn::Buffer>
decodeOpaqueAssignmentSet(const ndn::Buffer& payload)
{
    if (payload.empty()) {
        return {};
    }
    try {
        ndn::Block block(payload);
        if (block.type() != tlv::OpaqueAssignmentSetType) {
            return {payload};
        }
        block.parse();
        std::vector<ndn::Buffer> assignments;
        for (const auto& element : block.elements()) {
            if (element.type() != tlv::OpaqueAssignmentItemType ||
                assignments.size() >= 256 ||
                element.value_size() > 1024 * 1024) {
                throw std::runtime_error("invalid opaque assignment set");
            }
            assignments.emplace_back(element.value_begin(), element.value_end());
        }
        if (assignments.size() < 2) {
            throw std::runtime_error("non-canonical opaque assignment set");
        }
        return assignments;
    }
    catch (const ndn::tlv::Error&) {
        return {payload};
    }
}

ndn::Buffer
encodeCollaborationAssignmentEnvelope(
    const CollaborationAssignmentEnvelope& assignment)
{
    if (assignment.role.empty() || assignment.role.size() > 1024 ||
        assignment.assignedArtifact.toUri().size() > 8192 ||
        assignment.opaquePayload.size() > 1024 * 1024) {
        throw std::invalid_argument(
            "collaboration assignment envelope exceeds bounds");
    }
    ndn::Block block(tlv::CollaborationAssignmentEnvelopeType);
    block.push_back(ndn::makeStringBlock(
        tlv::CollaborationRoleType, assignment.role));
    if (!assignment.assignedArtifact.empty()) {
        block.push_back(ndn::makeStringBlock(
            tlv::CollaborationArtifactType,
            assignment.assignedArtifact.toUri()));
    }
    block.push_back(ndn::makeNonNegativeIntegerBlock(
        tlv::CollaborationProvisioningType,
        assignment.requiresProvisioning ? 1 : 0));
    block.push_back(ndn::makeNonNegativeIntegerBlock(
        tlv::CollaborationProvisioningTimeoutType,
        assignment.provisioningTimeoutMs));
    if (!assignment.scopeKeys.empty()) {
        ndn::Block scopeKeys(tlv::CollaborationScopeKeysType);
        for (const auto& [scope, key] : assignment.scopeKeys) {
            if (scope.empty() || scope.size() > 1024 ||
                key.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE) {
                throw std::invalid_argument(
                    "invalid collaboration assignment scope key");
            }
            ndn::Block entry(tlv::CollaborationScopeKeyType);
            entry.push_back(ndn::makeStringBlock(
                tlv::CollaborationScopeKeyNameType, scope));
            entry.push_back(ndn::makeBinaryBlock(
                tlv::CollaborationScopeKeyValueType,
                {key.data(), key.size()}));
            entry.encode();
            scopeKeys.push_back(entry);
        }
        scopeKeys.encode();
        block.push_back(scopeKeys);
    }
    block.push_back(ndn::makeBinaryBlock(
        tlv::CollaborationOpaquePayloadType,
        {assignment.opaquePayload.data(), assignment.opaquePayload.size()}));
    block.encode();
    return ndn::Buffer(block.data(), block.size());
}

bool
decodeCollaborationAssignmentEnvelope(
    const ndn::Buffer& payload,
    CollaborationAssignmentEnvelope& assignment)
{
    if (payload.empty()) {
        return false;
    }
    const auto wire = ndn::span<const uint8_t>(
        payload.data(), payload.size());
    auto [isBlock, candidate] = ndn::Block::fromBuffer(wire);
    if (!isBlock ||
        candidate.type() != tlv::CollaborationAssignmentEnvelopeType) {
        return false;
    }
    try {
        ndn::Block block = std::move(candidate);
        block.parse();
        CollaborationAssignmentEnvelope decoded;
        bool hasRole = false;
        bool hasOpaquePayload = false;
        for (const auto& element : block.elements()) {
            switch (element.type()) {
            case tlv::CollaborationRoleType:
                if (hasRole) {
                    throw std::runtime_error(
                        "duplicate collaboration assignment role");
                }
                decoded.role = ndn::readString(element);
                hasRole = true;
                break;
            case tlv::CollaborationArtifactType:
                if (!decoded.assignedArtifact.empty()) {
                    throw std::runtime_error(
                        "duplicate collaboration assignment artifact");
                }
                decoded.assignedArtifact = ndn::Name(ndn::readString(element));
                break;
            case tlv::CollaborationProvisioningType:
                decoded.requiresProvisioning =
                    ndn::readNonNegativeInteger(element) != 0;
                break;
            case tlv::CollaborationProvisioningTimeoutType:
                decoded.provisioningTimeoutMs =
                    ndn::readNonNegativeInteger(element);
                break;
            case tlv::CollaborationScopeKeysType:
                if (element.value_size() > 64 * 1024) {
                    throw std::runtime_error(
                        "collaboration assignment scope-key set exceeds bounds");
                }
                {
                    auto scopeBlock = element;
                    scopeBlock.parse();
                    for (const auto& scopeElement : scopeBlock.elements()) {
                        if (scopeElement.type() != tlv::CollaborationScopeKeyType) {
                            throw std::runtime_error(
                                "unknown collaboration assignment scope-key field");
                        }
                        auto entry = scopeElement;
                        entry.parse();
                        std::string scope;
                        ndn::Buffer key;
                        for (const auto& field : entry.elements()) {
                            if (field.type() == tlv::CollaborationScopeKeyNameType) {
                                if (!scope.empty()) {
                                    throw std::runtime_error(
                                        "duplicate collaboration assignment scope-key name");
                                }
                                scope = ndn::readString(field);
                            }
                            else if (field.type() == tlv::CollaborationScopeKeyValueType) {
                                if (!key.empty()) {
                                    throw std::runtime_error(
                                        "duplicate collaboration assignment scope-key value");
                                }
                                key = ndn::Buffer(field.value_begin(), field.value_end());
                            }
                            else {
                                throw std::runtime_error(
                                    "unknown collaboration assignment scope-key field");
                            }
                        }
                        if (scope.empty() || scope.size() > 1024 ||
                            key.size() != HybridMessageCrypto::MESSAGE_KEY_SIZE ||
                            decoded.scopeKeys.count(scope) != 0) {
                            throw std::runtime_error(
                                "invalid collaboration assignment scope key");
                        }
                        decoded.scopeKeys.emplace(std::move(scope), std::move(key));
                    }
                }
                break;
            case tlv::CollaborationOpaquePayloadType:
                if (hasOpaquePayload || element.value_size() > 1024 * 1024) {
                    throw std::runtime_error(
                        "invalid collaboration opaque assignment payload");
                }
                decoded.opaquePayload = ndn::Buffer(
                    element.value_begin(), element.value_end());
                hasOpaquePayload = true;
                break;
            default:
                throw std::runtime_error(
                    "unknown collaboration assignment envelope field");
            }
        }
        if (!hasRole || decoded.role.empty() || !hasOpaquePayload) {
            throw std::runtime_error(
                "incomplete collaboration assignment envelope");
        }
        assignment = std::move(decoded);
        return true;
    }
    catch (const ndn::tlv::Error&) {
        throw std::runtime_error(
            "invalid collaboration assignment envelope wire");
    }
}

ServiceSelectionMessage::ServiceSelectionMessage(const ServiceSelectionMessage& other)
{
    *this = other;
}

ServiceSelectionMessage&
ServiceSelectionMessage::operator=(const ServiceSelectionMessage& other)
{
    if (this != &other) {
        requestIDs_ = other.requestIDs_;
        providerToken_ = other.providerToken_;
        assignmentPayload_ = other.assignmentPayload_;
        policyEpoch_ = other.policyEpoch_;
        attempt_ = other.attempt_;
        providerEntries_ = other.providerEntries_;
        deploymentPlan_ = other.deploymentPlan_;
        selectionDecision_ = other.selectionDecision_;
        selectionInputKeyGrant_ = other.selectionInputKeyGrant_;
        recipientEncryptedAssignment_ = other.recipientEncryptedAssignment_;
        m_wire.reset();
    }
    return *this;
}

void ServiceSelectionMessage::setRequestIDs(const std::vector<std::string>& requestIDs) {
    requestIDs_ = requestIDs;
}

void ServiceSelectionMessage::setProviderToken(const std::string& providerToken) {
    providerToken_ = providerToken;
}

void ServiceSelectionMessage::setAssignmentPayload(const ndn::Buffer& payload) {
    assignmentPayload_ = payload;
}

void ServiceSelectionMessage::setPolicyEpoch(size_t policyEpoch) {
    policyEpoch_ = policyEpoch;
}

void ServiceSelectionMessage::setAttempt(uint64_t attempt) {
    if (attempt == 0) throw std::invalid_argument("Selection attempt must be positive");
    attempt_ = attempt;
    m_wire.reset();
}

void ServiceSelectionMessage::addProviderEntry(const SelectionProviderEntry& entry) {
    providerEntries_.push_back(entry);
}

void ServiceSelectionMessage::setDeploymentPlan(const DeploymentPlan& plan) {
    deploymentPlan_ = plan;
    m_wire.reset();
}
bool ServiceSelectionMessage::hasDeploymentPlan() const { return deploymentPlan_.has_value(); }
const DeploymentPlan& ServiceSelectionMessage::getDeploymentPlan() const {
    if (!deploymentPlan_) throw std::logic_error("Selection has no deployment plan");
    return *deploymentPlan_;
}
void ServiceSelectionMessage::setSelectionDecision(const SelectionDecision& decision) {
    selectionDecision_ = decision;
    m_wire.reset();
}
void ServiceSelectionMessage::setSelectionInputKeyGrant(const SelectionInputKeyGrant& grant) {
    selectionInputKeyGrant_ = grant;
    m_wire.reset();
}
void ServiceSelectionMessage::setRecipientEncryptedAssignment(const RecipientEncryptedAssignment& assignment) {
    recipientEncryptedAssignment_ = assignment;
    m_wire.reset();
}
bool ServiceSelectionMessage::hasSelectionDecision() const { return selectionDecision_.has_value(); }
bool ServiceSelectionMessage::hasSelectionInputKeyGrant() const { return selectionInputKeyGrant_.has_value(); }
bool ServiceSelectionMessage::hasRecipientEncryptedAssignment() const { return recipientEncryptedAssignment_.has_value(); }
const SelectionDecision& ServiceSelectionMessage::getSelectionDecision() const {
    if (!selectionDecision_) throw std::logic_error("Selection has no R1 decision");
    return *selectionDecision_;
}
const SelectionInputKeyGrant& ServiceSelectionMessage::getSelectionInputKeyGrant() const {
    if (!selectionInputKeyGrant_) throw std::logic_error("Selection has no input key grant");
    return *selectionInputKeyGrant_;
}
const RecipientEncryptedAssignment& ServiceSelectionMessage::getRecipientEncryptedAssignment() const {
    if (!recipientEncryptedAssignment_) throw std::logic_error("Selection has no encrypted assignment");
    return *recipientEncryptedAssignment_;
}

const std::vector<std::string>& ServiceSelectionMessage::getRequestIDs() const {
    return requestIDs_;
}

const std::string& ServiceSelectionMessage::getProviderToken() const {
    return providerToken_;
}

const ndn::Buffer& ServiceSelectionMessage::getAssignmentPayload() const {
    return assignmentPayload_;
}

size_t ServiceSelectionMessage::getPolicyEpoch() const {
    return policyEpoch_;
}

uint64_t ServiceSelectionMessage::getAttempt() const {
    return attempt_;
}

const std::vector<SelectionProviderEntry>& ServiceSelectionMessage::getProviderEntries() const {
    return providerEntries_;
}

void ServiceSelectionMessage::Clear() {
    requestIDs_.clear();
    providerToken_.clear();
    assignmentPayload_.clear();
    policyEpoch_ = 0;
    attempt_ = 1;
    providerEntries_.clear();
    deploymentPlan_.reset();
    selectionDecision_.reset();
    selectionInputKeyGrant_.reset();
    recipientEncryptedAssignment_.reset();
    m_wire.reset();
}

ndn::Block ServiceSelectionMessage::WireEncode() const {
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
    block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::AttemptType, attempt_));
    for (const auto& entry : providerEntries_) {
        ndn::Block entryBlock(tlv::SelectionProviderEntryType);
        entryBlock.push_back(ndn::makeStringBlock(tlv::ProviderNameType,
                                                  entry.providerName.toUri()));
        if (!entry.providerTokenHash.empty()) {
            entryBlock.push_back(ndn::makeStringBlock(tlv::ProviderTokenType,
                                                      entry.providerTokenHash));
        }
        if (!entry.assignmentPayload.empty()) {
            entryBlock.push_back(ndn::makeBinaryBlock(tlv::AssignmentPayloadType,
                                                      entry.assignmentPayload.begin(),
                                                      entry.assignmentPayload.end()));
        }
        entryBlock.encode();
        block.push_back(entryBlock);
    }
    if (deploymentPlan_) block.push_back(deploymentPlan_->WireEncode());
    if (selectionDecision_) block.push_back(selectionDecision_->WireEncode());
    if (selectionInputKeyGrant_) block.push_back(selectionInputKeyGrant_->WireEncode());
    if (recipientEncryptedAssignment_) block.push_back(recipientEncryptedAssignment_->WireEncode());
    block.encode();
    m_wire = block;
    return m_wire;
}

bool ServiceSelectionMessage::WireDecode(const ndn::Block& block) {
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
        else if (b.type() == tlv::AttemptType) {
            attempt_ = ndn::readNonNegativeInteger(b);
            if (attempt_ == 0) return false;
        }
        else if (b.type() == tlv::SelectionProviderEntryType) {
            SelectionProviderEntry entry;
            b.parse();
            for (auto e : b.elements()) {
                if (e.type() == tlv::ProviderNameType) {
                    entry.providerName = ndn::Name(ndn::readString(e));
                }
                else if (e.type() == tlv::ProviderTokenType) {
                    entry.providerTokenHash = ndn::readString(e);
                }
                else if (e.type() == tlv::AssignmentPayloadType) {
                    entry.assignmentPayload = ndn::Buffer(e.value(), e.value_size());
                }
            }
            if (!entry.providerName.empty()) {
                providerEntries_.push_back(entry);
            }
        }
        else if (b.type() == tlv::DeploymentPlanType) {
            DeploymentPlan plan;
            if (!plan.WireDecode(b)) return false;
            deploymentPlan_ = std::move(plan);
        }
        else if (b.type() == tlv::SelectionDecisionType) {
            SelectionDecision decision;
            if (!decision.WireDecode(b)) return false;
            selectionDecision_ = std::move(decision);
        }
        else if (b.type() == tlv::SelectionInputKeyGrantType) {
            SelectionInputKeyGrant grant;
            if (!grant.WireDecode(b)) return false;
            selectionInputKeyGrant_ = std::move(grant);
        }
        else if (b.type() == tlv::RecipientEncryptedAssignmentType) {
            RecipientEncryptedAssignment assignment;
            if (!assignment.WireDecode(b)) return false;
            recipientEncryptedAssignment_ = std::move(assignment);
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
