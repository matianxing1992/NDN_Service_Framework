#include "InvocationStream.hpp"

#include "HybridMessageCrypto.hpp"
#include "NDNSFMessages.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace ndn_service_framework {
namespace {

template<size_t N>
bool
allZero(const std::array<uint8_t, N>& value)
{
  return std::all_of(value.begin(), value.end(), [] (uint8_t byte) {
    return byte == 0;
  });
}

bool
sameWire(const ndn::Block& left, const ndn::Block& right)
{
  return left.isValid() && right.isValid() && left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

template<size_t N>
ndn::Block
makeFixedBlock(uint32_t type, const std::array<uint8_t, N>& value)
{
  return ndn::makeBinaryBlock(
    type, ndn::span<const uint8_t>(value.data(), value.size()));
}

template<size_t N>
std::array<uint8_t, N>
readFixed(const ndn::Block& block)
{
  if (block.value_size() != N) {
    throw std::invalid_argument("invalid fixed-width stream field");
  }
  std::array<uint8_t, N> value{};
  std::copy(block.value_begin(), block.value_end(), value.begin());
  return value;
}

template<typename T>
T
readBoundedUnsigned(const ndn::Block& block)
{
  static_assert(std::is_integral<T>::value && std::is_unsigned<T>::value,
                "stream integer target must be unsigned");
  const auto value = ndn::readNonNegativeInteger(block);
  if (value > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
    throw std::invalid_argument("stream integer exceeds target width");
  }
  return static_cast<T>(value);
}

ndn::Buffer
readBuffer(const ndn::Block& block)
{
  return {block.value_begin(), block.value_end()};
}

template<typename T>
void
appendUint64(T& output, uint64_t value)
{
  for (int shift = 56; shift >= 0; shift -= 8) {
    output.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
  }
}

void
appendRaw(ndn::Buffer& output, ndn::span<const uint8_t> bytes)
{
  output.insert(output.end(), bytes.begin(), bytes.end());
}

void
appendNameWire(ndn::Buffer& output, const ndn::Name& name)
{
  const auto wire = name.wireEncode();
  appendRaw(output, ndn::span<const uint8_t>(wire.begin(), wire.size()));
}

void
appendLengthPrefixed(ndn::Buffer& output, ndn::span<const uint8_t> bytes)
{
  appendUint64(output, bytes.size());
  appendRaw(output, bytes);
}

template<size_t N>
void
appendArray(ndn::Buffer& output, const std::array<uint8_t, N>& value)
{
  appendRaw(output, ndn::span<const uint8_t>(value.data(), value.size()));
}

std::string
toLowerHex(ndn::span<const uint8_t> value)
{
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : value) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

template<size_t N>
std::optional<std::array<uint8_t, N>>
fromLowerHex(const std::string& value)
{
  if (value.size() != N * 2 ||
      !std::all_of(value.begin(), value.end(), [] (char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      })) {
    return std::nullopt;
  }
  std::array<uint8_t, N> result{};
  for (size_t i = 0; i < N; ++i) {
    const auto digit = [] (char ch) -> uint8_t {
      return ch <= '9' ? static_cast<uint8_t>(ch - '0')
                       : static_cast<uint8_t>(ch - 'a' + 10);
    };
    result[i] = static_cast<uint8_t>((digit(value[i * 2]) << 4) |
                                     digit(value[i * 2 + 1]));
  }
  return result;
}

bool
isKnownFinishReason(StreamFinishReason reason)
{
  const auto value = static_cast<uint64_t>(reason);
  return value >= static_cast<uint64_t>(StreamFinishReason::Eos) &&
         value <= static_cast<uint64_t>(StreamFinishReason::Failed);
}

bool
isSuccessfulFinishReason(StreamFinishReason reason)
{
  return reason == StreamFinishReason::Eos ||
         reason == StreamFinishReason::StopSequence ||
         reason == StreamFinishReason::MaxTokens ||
         reason == StreamFinishReason::ApplicationComplete;
}

ndn::Name
subName(const ndn::Name& name, size_t begin, size_t count)
{
  ndn::Name result;
  for (size_t index = 0; index < count; ++index) {
    result.append(name.get(begin + index));
  }
  return result;
}

void
appendRequesterComponent(ndn::Name& name, const ndn::Name& requester)
{
  name.append(ndn::name::Component(requester.toUri()));
}

void
appendRequestIdComponent(ndn::Name& name, const ndn::Name& requestId)
{
  // Legacy event names carry only the final request-id component. Structured
  // native IDs contain reserved path components; carry their canonical URI as
  // one component instead of silently dropping the structured identity.
  const bool structured = requestId.size() >= 4 &&
    requestId.get(0).toUri() == "NDNSF" &&
    requestId.get(1).toUri() == "DI" &&
    requestId.get(2).toUri() == "REQUEST";
  if (!structured) {
    // Preserve the pre-structured event-name wire for legacy multi-component
    // callers, whose parser historically exposed only the final component.
    name.append(requestId.get(-1));
  }
  else {
    name.append(ndn::name::Component(requestId.toUri()));
  }
}

std::optional<ndn::Name>
parseRequesterComponent(const ndn::name::Component& component)
{
  try {
    const std::string value(
      reinterpret_cast<const char*>(component.value()), component.value_size());
    if (value.empty() || value.front() != '/') {
      return std::nullopt;
    }
    ndn::Name requester(value);
    if (ndn::name::Component(requester.toUri()) != component) {
      return std::nullopt;
    }
    return requester;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<ndn::Name>
parseRequestIdComponent(const ndn::name::Component& component)
{
  try {
    const std::string value(
      reinterpret_cast<const char*>(component.value()), component.value_size());
    if (value.empty()) return std::nullopt;
    if (value.front() != '/') {
      ndn::Name requestId;
      requestId.append(component);
      return requestId;
    }
    ndn::Name requestId(value);
    if (ndn::name::Component(requestId.toUri()) != component) {
      return std::nullopt;
    }
    return requestId;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

template<typename Container>
const ndn::Block&
take(const Container& elements, size_t& index, uint32_t expectedType)
{
  if (index >= elements.size() || elements[index].type() != expectedType) {
    throw std::invalid_argument("non-canonical invocation stream field order");
  }
  return elements[index++];
}

} // namespace

void
StreamRequestOptions::validate() const
{
  if (version != VERSION ||
      (mode != InvocationMode::Normal && mode != InvocationMode::Targeted) ||
      allZero(generationId) || attemptEpoch < 1 || attemptEpoch > 2 ||
      streamEpoch == 0 || allZero(eventKeyCommitment) ||
      deadlineEpochMs == 0 ||
      maxEvents < 1 || maxEvents > 4096 ||
      interestWindow < 1 || interestWindow > 64 ||
      interestLifetimeMs < 100 || interestLifetimeMs > 5000 ||
      maxEventRetries > 8 ||
      publisherQueueCapacity < 1 || publisherQueueCapacity > 1024 ||
      callbackQueueCapacity < 1 || callbackQueueCapacity > 1024 ||
      reorderCapacity < interestWindow || reorderCapacity > 1024 ||
      retentionMs < 1000 || retentionMs > 300000 ||
      completionGraceMs > 30000 ||
      maxEventWireBytes < 512 || maxEventWireBytes > 65536 ||
      (controllerVersion && !controllerVersion->isValid()) ||
      (allowReplacement && maxReplacements != 1) ||
      (!allowReplacement && maxReplacements != 0) ||
      (mode == InvocationMode::Targeted && allowReplacement) ||
      (mode == InvocationMode::Normal && eventKeyGrant)) {
    throw std::invalid_argument("invalid streamed invocation options");
  }
  if (eventKeyGrant && eventKeyGrant->type() != tlv::HybridMessageEnvelopeType) {
    throw std::invalid_argument("invalid streamed invocation key grant");
  }
}

ndn::Block
StreamRequestOptions::wireEncode() const
{
  validate();
  ndn::Block block(tlv::StreamRequestOptionsType);
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, version));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    tlv::StreamInvocationModeType, static_cast<uint64_t>(mode)));
  block.push_back(makeFixedBlock(tlv::StreamGenerationIdType, generationId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    tlv::StreamAttemptEpochType, attemptEpoch));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamEpochType, streamEpoch));
  block.push_back(makeFixedBlock(
    tlv::StreamEventKeyCommitmentType, eventKeyCommitment));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    tlv::StreamDeadlineEpochMsType, deadlineEpochMs));
  if (controllerVersion) {
    ndn::Block version(tlv::StreamControllerVersionType);
    version.push_back(controllerVersion->wireEncode());
    version.encode();
    block.push_back(version);
  }
  if (eventKeyGrant) {
    ndn::Block grant(tlv::StreamEventKeyGrantType);
    grant.push_back(*eventKeyGrant);
    grant.encode();
    block.push_back(grant);
  }
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamMaxEventsType, maxEvents));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamInterestWindowType, interestWindow));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamInterestLifetimeMsType, interestLifetimeMs));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamMaxEventRetriesType, maxEventRetries));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamPublisherQueueType, publisherQueueCapacity));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamCallbackQueueType, callbackQueueCapacity));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamReorderCapacityType, reorderCapacity));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamRetentionMsType, retentionMs));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamCompletionGraceMsType, completionGraceMs));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamMaxEventWireBytesType, maxEventWireBytes));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamAllowReplacementType, allowReplacement ? 1 : 0));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamMaxReplacementsType, maxReplacements));
  block.encode();
  return block;
}

bool
StreamRequestOptions::wireDecode(const ndn::Block& wire)
{
  try {
    if (wire.type() != tlv::StreamRequestOptionsType) {
      return false;
    }
    auto block = wire;
    block.parse();
    const auto& elements = block.elements();
    size_t index = 0;
    StreamRequestOptions decoded;
    decoded.version = ndn::readNonNegativeInteger(take(elements, index, tlv::VersionType));
    decoded.mode = static_cast<InvocationMode>(ndn::readNonNegativeInteger(
      take(elements, index, tlv::StreamInvocationModeType)));
    decoded.generationId = readFixed<16>(take(elements, index, tlv::StreamGenerationIdType));
    decoded.attemptEpoch = ndn::readNonNegativeInteger(
      take(elements, index, tlv::StreamAttemptEpochType));
    decoded.streamEpoch = ndn::readNonNegativeInteger(take(elements, index, tlv::StreamEpochType));
    decoded.eventKeyCommitment = readFixed<32>(
      take(elements, index, tlv::StreamEventKeyCommitmentType));
    decoded.deadlineEpochMs = ndn::readNonNegativeInteger(
      take(elements, index, tlv::StreamDeadlineEpochMsType));
    if (index < elements.size() &&
        elements[index].type() == tlv::StreamControllerVersionType) {
      auto version = elements[index++];
      version.parse();
      if (version.elements().size() != 1 ||
          version.elements().front().type() != ControllerVersion::TYPE) {
        return false;
      }
      ControllerVersion controllerVersion;
      if (!controllerVersion.wireDecode(version.elements().front())) {
        return false;
      }
      decoded.controllerVersion = controllerVersion;
    }
    if (index < elements.size() && elements[index].type() == tlv::StreamEventKeyGrantType) {
      auto grant = elements[index++];
      grant.parse();
      if (grant.elements().size() != 1 ||
          grant.elements().front().type() != tlv::HybridMessageEnvelopeType) {
        return false;
      }
      HybridMessageEnvelope envelope;
      if (!envelope.WireDecode(grant.elements().front())) {
        return false;
      }
      decoded.eventKeyGrant = envelope.WireEncode();
    }
    decoded.maxEvents = readBoundedUnsigned<uint32_t>(
      take(elements, index, tlv::StreamMaxEventsType));
    decoded.interestWindow = readBoundedUnsigned<uint16_t>(
      take(elements, index, tlv::StreamInterestWindowType));
    decoded.interestLifetimeMs = readBoundedUnsigned<uint32_t>(
      take(elements, index, tlv::StreamInterestLifetimeMsType));
    decoded.maxEventRetries = readBoundedUnsigned<uint8_t>(
      take(elements, index, tlv::StreamMaxEventRetriesType));
    decoded.publisherQueueCapacity = readBoundedUnsigned<uint16_t>(
      take(elements, index, tlv::StreamPublisherQueueType));
    decoded.callbackQueueCapacity = readBoundedUnsigned<uint16_t>(
      take(elements, index, tlv::StreamCallbackQueueType));
    decoded.reorderCapacity = readBoundedUnsigned<uint16_t>(
      take(elements, index, tlv::StreamReorderCapacityType));
    decoded.retentionMs = readBoundedUnsigned<uint32_t>(
      take(elements, index, tlv::StreamRetentionMsType));
    decoded.completionGraceMs = readBoundedUnsigned<uint32_t>(
      take(elements, index, tlv::StreamCompletionGraceMsType));
    decoded.maxEventWireBytes = readBoundedUnsigned<uint32_t>(
      take(elements, index, tlv::StreamMaxEventWireBytesType));
    const auto allowReplacement = ndn::readNonNegativeInteger(
      take(elements, index, tlv::StreamAllowReplacementType));
    if (allowReplacement > 1) {
      return false;
    }
    decoded.allowReplacement = allowReplacement == 1;
    decoded.maxReplacements = readBoundedUnsigned<uint8_t>(
      take(elements, index, tlv::StreamMaxReplacementsType));
    if (index != elements.size()) {
      return false;
    }
    decoded.validate();
    const auto canonical = decoded.wireEncode();
    if (!sameWire(canonical, wire)) {
      return false;
    }
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
StreamRequestOptions::operator==(const StreamRequestOptions& other) const
{
  const auto grantEqual = [this, &other] {
    if (eventKeyGrant.has_value() != other.eventKeyGrant.has_value()) {
      return false;
    }
    return !eventKeyGrant || sameWire(*eventKeyGrant, *other.eventKeyGrant);
  };
  return version == other.version && mode == other.mode &&
         generationId == other.generationId && attemptEpoch == other.attemptEpoch &&
         streamEpoch == other.streamEpoch &&
         eventKeyCommitment == other.eventKeyCommitment &&
         deadlineEpochMs == other.deadlineEpochMs &&
         controllerVersion == other.controllerVersion && grantEqual() &&
         maxEvents == other.maxEvents && interestWindow == other.interestWindow &&
         interestLifetimeMs == other.interestLifetimeMs &&
         maxEventRetries == other.maxEventRetries &&
         publisherQueueCapacity == other.publisherQueueCapacity &&
         callbackQueueCapacity == other.callbackQueueCapacity &&
         reorderCapacity == other.reorderCapacity && retentionMs == other.retentionMs &&
         completionGraceMs == other.completionGraceMs &&
         maxEventWireBytes == other.maxEventWireBytes &&
         allowReplacement == other.allowReplacement &&
         maxReplacements == other.maxReplacements;
}

void
ConversationContinuationOptions::validate() const
{
  if (version != VERSION || allZero(conversationId) || allZero(turnInputDigest) ||
      (mode != ConversationInputMode::FullContext &&
       mode != ConversationInputMode::AppendDelta) ||
      (mode == ConversationInputMode::FullContext &&
       (parentContextEpoch.has_value() || parentCheckpoint.has_value())) ||
      (mode == ConversationInputMode::AppendDelta &&
       (!parentContextEpoch.has_value() || !parentCheckpoint.has_value() ||
        *parentContextEpoch == 0 || parentCheckpoint->empty())) ||
      (parentCheckpoint && parentCheckpoint->size() > 65536) ||
      (allowFullPrefillFallback != fallbackInputDigest.has_value())) {
    throw std::invalid_argument("invalid conversation continuation options");
  }
}

ndn::Block
ConversationContinuationOptions::wireEncode() const
{
  validate();
  ndn::Block block(tlv::ConversationContinuationType);
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, version));
  block.push_back(makeFixedBlock(tlv::ConversationIdType, conversationId));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    tlv::ConversationModeType, static_cast<uint64_t>(mode)));
  if (mode == ConversationInputMode::AppendDelta) {
    block.push_back(ndn::makeNonNegativeIntegerBlock(
      tlv::ConversationParentEpochType, *parentContextEpoch));
    block.push_back(ndn::makeBinaryBlock(
      tlv::ConversationCheckpointType,
      ndn::span<const uint8_t>(parentCheckpoint->data(), parentCheckpoint->size())));
  }
  block.push_back(makeFixedBlock(tlv::ConversationTurnInputDigestType,
                                 turnInputDigest));
  block.push_back(ndn::makeNonNegativeIntegerBlock(
    tlv::ConversationAllowFallbackType, allowFullPrefillFallback ? 1 : 0));
  if (fallbackInputDigest) {
    block.push_back(makeFixedBlock(tlv::ConversationFallbackInputDigestType,
                                   *fallbackInputDigest));
  }
  block.encode();
  return block;
}

bool
ConversationContinuationOptions::wireDecode(const ndn::Block& wire)
{
  try {
    if (wire.type() != tlv::ConversationContinuationType) {
      return false;
    }
    auto block = wire;
    block.parse();
    const auto& elements = block.elements();
    size_t index = 0;
    ConversationContinuationOptions decoded;
    decoded.version = ndn::readNonNegativeInteger(
      take(elements, index, tlv::VersionType));
    decoded.conversationId = readFixed<16>(
      take(elements, index, tlv::ConversationIdType));
    const auto mode = ndn::readNonNegativeInteger(
      take(elements, index, tlv::ConversationModeType));
    if (mode > static_cast<uint64_t>(ConversationInputMode::AppendDelta)) {
      return false;
    }
    decoded.mode = static_cast<ConversationInputMode>(mode);
    if (decoded.mode == ConversationInputMode::AppendDelta) {
      decoded.parentContextEpoch = ndn::readNonNegativeInteger(
        take(elements, index, tlv::ConversationParentEpochType));
      decoded.parentCheckpoint = readBuffer(
        take(elements, index, tlv::ConversationCheckpointType));
    }
    decoded.turnInputDigest = readFixed<32>(
      take(elements, index, tlv::ConversationTurnInputDigestType));
    const auto allowFallback = ndn::readNonNegativeInteger(
      take(elements, index, tlv::ConversationAllowFallbackType));
    if (allowFallback > 1) {
      return false;
    }
    decoded.allowFullPrefillFallback = allowFallback == 1;
    if (index < elements.size() &&
        elements[index].type() == tlv::ConversationFallbackInputDigestType) {
      decoded.fallbackInputDigest = readFixed<32>(elements[index++]);
    }
    if (index != elements.size()) {
      return false;
    }
    decoded.validate();
    if (!sameWire(decoded.wireEncode(), wire)) {
      return false;
    }
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
ConversationContinuationOptions::operator==(
  const ConversationContinuationOptions& other) const
{
  const auto checkpointEqual = [this, &other] {
    if (parentCheckpoint.has_value() != other.parentCheckpoint.has_value()) {
      return false;
    }
    return !parentCheckpoint || *parentCheckpoint == *other.parentCheckpoint;
  };
  const auto fallbackEqual = [this, &other] {
    if (fallbackInputDigest.has_value() != other.fallbackInputDigest.has_value()) {
      return false;
    }
    return !fallbackInputDigest || *fallbackInputDigest == *other.fallbackInputDigest;
  };
  return version == other.version && mode == other.mode &&
         conversationId == other.conversationId &&
         parentContextEpoch == other.parentContextEpoch && checkpointEqual() &&
         turnInputDigest == other.turnInputDigest &&
         allowFullPrefillFallback == other.allowFullPrefillFallback &&
         fallbackEqual();
}

void
StreamEndEvent::validate() const
{
  if (finalCursor == 0 || applicationEventCount != finalCursor - 1 ||
      generatedTokenCount > applicationEventCount || !isKnownFinishReason(finishReason) ||
      allZero(transcriptDigest) || allZero(finalResultDigest) ||
      (isSuccessfulFinishReason(finishReason) && !errorInfo.empty()) ||
      (!isSuccessfulFinishReason(finishReason) &&
       (errorInfo.empty() || errorInfo.size() > 1024))) {
    throw std::invalid_argument("invalid invocation End event");
  }
}

ndn::Block
StreamEndEvent::wireEncode() const
{
  validate();
  ndn::Block block(tlv::StreamEndEventType);
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamFinalCursorType, finalCursor));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamFinishReasonType, static_cast<uint64_t>(finishReason)));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamApplicationEventCountType, applicationEventCount));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamGeneratedTokenCountType, generatedTokenCount));
  block.push_back(makeFixedBlock(tlv::StreamTranscriptDigestType, transcriptDigest));
  block.push_back(makeFixedBlock(tlv::StreamFinalResultDigestType, finalResultDigest));
  block.push_back(ndn::makeStringBlock(tlv::ErrorInfoType, errorInfo));
  block.encode();
  return block;
}

bool
StreamEndEvent::wireDecode(const ndn::Block& wire)
{
  try {
    if (wire.type() != tlv::StreamEndEventType) return false;
    auto block = wire;
    block.parse();
    const auto& elements = block.elements();
    size_t index = 0;
    StreamEndEvent decoded;
    decoded.finalCursor = ndn::readNonNegativeInteger(take(elements, index, tlv::StreamFinalCursorType));
    decoded.finishReason = static_cast<StreamFinishReason>(ndn::readNonNegativeInteger(take(elements, index, tlv::StreamFinishReasonType)));
    decoded.applicationEventCount = ndn::readNonNegativeInteger(take(elements, index, tlv::StreamApplicationEventCountType));
    decoded.generatedTokenCount = ndn::readNonNegativeInteger(take(elements, index, tlv::StreamGeneratedTokenCountType));
    decoded.transcriptDigest = readFixed<32>(take(elements, index, tlv::StreamTranscriptDigestType));
    decoded.finalResultDigest = readFixed<32>(take(elements, index, tlv::StreamFinalResultDigestType));
    decoded.errorInfo = ndn::readString(take(elements, index, tlv::ErrorInfoType));
    if (index != elements.size()) return false;
    decoded.validate();
    if (!sameWire(decoded.wireEncode(), wire)) return false;
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
StreamEndEvent::operator==(const StreamEndEvent& other) const
{
  return finalCursor == other.finalCursor && finishReason == other.finishReason &&
         applicationEventCount == other.applicationEventCount &&
         generatedTokenCount == other.generatedTokenCount &&
         transcriptDigest == other.transcriptDigest &&
         finalResultDigest == other.finalResultDigest && errorInfo == other.errorInfo;
}

void
InvocationEventMessage::setPayload(const ndn::Buffer& value)
{
  payload = value;
  payloadDigest = computeStreamSha256(
    ndn::span<const uint8_t>(payload.data(), payload.size()));
}

void
InvocationEventMessage::validate() const
{
  const auto actualPayloadDigest = computeStreamSha256(
    ndn::span<const uint8_t>(payload.data(), payload.size()));
  const bool application = eventType == InvocationEventType::Application;
  const bool terminalEvent = eventType == InvocationEventType::End;
  if (version != VERSION || allZero(bindingDigest) || cursor == 0 ||
      (!application && !terminalEvent) || actualPayloadDigest != payloadDigest ||
      publishedAtUs == 0 || userToken.empty() ||
      (application && (payload.empty() || terminal || end)) ||
      (terminalEvent && (!payload.empty() || !terminal || !end ||
                         end->finalCursor != cursor))) {
    throw std::invalid_argument("invalid invocation event message");
  }
  if (end) end->validate();
}

ndn::Block
InvocationEventMessage::wireEncode() const
{
  validate();
  ndn::Block block(tlv::InvocationEventMessageType);
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, version));
  block.push_back(makeFixedBlock(tlv::StreamBindingDigestType, bindingDigest));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamCursorType, cursor));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamEventTypeType, static_cast<uint64_t>(eventType)));
  block.push_back(ndn::makeBinaryBlock(
    tlv::PayloadType, ndn::span<const uint8_t>(payload.data(), payload.size())));
  block.push_back(makeFixedBlock(tlv::StreamPayloadDigestType, payloadDigest));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamPublishedAtUsType, publishedAtUs));
  block.push_back(ndn::makeBinaryBlock(
    tlv::UserTokenType, ndn::span<const uint8_t>(userToken.data(), userToken.size())));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamTerminalType, terminal ? 1 : 0));
  if (end) block.push_back(end->wireEncode());
  block.encode();
  return block;
}

bool
InvocationEventMessage::wireDecode(const ndn::Block& wire)
{
  try {
    if (wire.type() != tlv::InvocationEventMessageType || wire.size() > 65536) {
      return false;
    }
    auto block = wire;
    block.parse();
    const auto& elements = block.elements();
    size_t index = 0;
    InvocationEventMessage decoded;
    decoded.version = ndn::readNonNegativeInteger(take(elements, index, tlv::VersionType));
    decoded.bindingDigest = readFixed<32>(take(elements, index, tlv::StreamBindingDigestType));
    decoded.cursor = ndn::readNonNegativeInteger(take(elements, index, tlv::StreamCursorType));
    decoded.eventType = static_cast<InvocationEventType>(ndn::readNonNegativeInteger(take(elements, index, tlv::StreamEventTypeType)));
    decoded.payload = readBuffer(take(elements, index, tlv::PayloadType));
    decoded.payloadDigest = readFixed<32>(take(elements, index, tlv::StreamPayloadDigestType));
    decoded.publishedAtUs = ndn::readNonNegativeInteger(take(elements, index, tlv::StreamPublishedAtUsType));
    decoded.userToken = readBuffer(take(elements, index, tlv::UserTokenType));
    const auto terminal = ndn::readNonNegativeInteger(take(elements, index, tlv::StreamTerminalType));
    if (terminal > 1) return false;
    decoded.terminal = terminal == 1;
    if (index < elements.size() && elements[index].type() == tlv::StreamEndEventType) {
      StreamEndEvent endEvent;
      if (!endEvent.wireDecode(elements[index++])) return false;
      decoded.end = std::move(endEvent);
    }
    if (index != elements.size()) return false;
    decoded.validate();
    if (!sameWire(decoded.wireEncode(), wire)) return false;
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
InvocationEventMessage::operator==(const InvocationEventMessage& other) const
{
  return version == other.version && bindingDigest == other.bindingDigest &&
         cursor == other.cursor && eventType == other.eventType &&
         payload == other.payload && payloadDigest == other.payloadDigest &&
         publishedAtUs == other.publishedAtUs && userToken == other.userToken &&
         terminal == other.terminal && end == other.end;
}

void
StreamCompletion::validate() const
{
  if (version != VERSION || allZero(bindingDigest) || endEventName.empty() ||
      finalCursor == 0 || applicationEventCount != finalCursor - 1 ||
      !isKnownFinishReason(finishReason) || allZero(transcriptDigest) ||
      allZero(finalResultDigest)) {
    throw std::invalid_argument("invalid stream completion");
  }
}

ndn::Block
StreamCompletion::wireEncode() const
{
  validate();
  ndn::Block block(tlv::StreamCompletionType);
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::VersionType, version));
  block.push_back(makeFixedBlock(tlv::StreamBindingDigestType, bindingDigest));
  ndn::Block endName(tlv::StreamEndEventNameType);
  endName.push_back(endEventName.wireEncode());
  endName.encode();
  block.push_back(endName);
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamFinalCursorType, finalCursor));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamFinishReasonType, static_cast<uint64_t>(finishReason)));
  block.push_back(ndn::makeNonNegativeIntegerBlock(tlv::StreamApplicationEventCountType, applicationEventCount));
  block.push_back(makeFixedBlock(tlv::StreamTranscriptDigestType, transcriptDigest));
  block.push_back(makeFixedBlock(tlv::StreamFinalResultDigestType, finalResultDigest));
  block.encode();
  return block;
}

bool
StreamCompletion::wireDecode(const ndn::Block& wire)
{
  try {
    if (wire.type() != tlv::StreamCompletionType) return false;
    auto block = wire;
    block.parse();
    const auto& elements = block.elements();
    size_t index = 0;
    StreamCompletion decoded;
    decoded.version = ndn::readNonNegativeInteger(take(elements, index, tlv::VersionType));
    decoded.bindingDigest = readFixed<32>(take(elements, index, tlv::StreamBindingDigestType));
    auto endName = take(elements, index, tlv::StreamEndEventNameType);
    endName.parse();
    if (endName.elements().size() != 1 ||
        endName.elements().front().type() != ndn::tlv::Name) return false;
    decoded.endEventName = ndn::Name(endName.elements().front());
    decoded.finalCursor = ndn::readNonNegativeInteger(take(elements, index, tlv::StreamFinalCursorType));
    decoded.finishReason = static_cast<StreamFinishReason>(ndn::readNonNegativeInteger(take(elements, index, tlv::StreamFinishReasonType)));
    decoded.applicationEventCount = ndn::readNonNegativeInteger(take(elements, index, tlv::StreamApplicationEventCountType));
    decoded.transcriptDigest = readFixed<32>(take(elements, index, tlv::StreamTranscriptDigestType));
    decoded.finalResultDigest = readFixed<32>(take(elements, index, tlv::StreamFinalResultDigestType));
    if (index != elements.size()) return false;
    decoded.validate();
    if (!sameWire(decoded.wireEncode(), wire)) return false;
    *this = std::move(decoded);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
StreamCompletion::operator==(const StreamCompletion& other) const
{
  return version == other.version && bindingDigest == other.bindingDigest &&
         endEventName == other.endEventName && finalCursor == other.finalCursor &&
         finishReason == other.finishReason &&
         applicationEventCount == other.applicationEventCount &&
         transcriptDigest == other.transcriptDigest &&
         finalResultDigest == other.finalResultDigest;
}

bool
StreamTerminalAuthority::claim(StreamTerminalState state)
{
  if (state == StreamTerminalState::None) {
    throw std::invalid_argument("cannot claim a non-terminal state");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (fenced_ || state_ != StreamTerminalState::None) {
    return false;
  }
  state_ = state;
  return true;
}

bool
StreamedResponseWriterCore::publish(const ndn::Buffer& payload, uint64_t& cursor)
{
  PublishCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || terminal_ || !publish_) return false;
    callback = publish_;
  }
  if (!callback(payload, cursor)) return false;
  return true;
}

bool
StreamedResponseWriterCore::finish(const ndn::Buffer& payload,
                                    StreamFinishReason reason)
{
  FinishCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || terminal_ || !finish_) return false;
    terminal_ = true;
    callback = finish_;
  }
  if (callback(payload, reason)) return true;
  std::lock_guard<std::mutex> lock(mutex_);
  terminal_ = false;
  return false;
}

bool
StreamedResponseWriterCore::fail(StreamedInvocationErrorCode code,
                                 const std::string& message)
{
  FailCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || terminal_ || !fail_) return false;
    terminal_ = true;
    callback = fail_;
  }
  if (callback(code, message)) return true;
  std::lock_guard<std::mutex> lock(mutex_);
  terminal_ = false;
  return false;
}

bool
StreamedResponseWriterCore::isCancelled() const
{
  CancelledCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || terminal_) return true;
    callback = cancelled_;
  }
  return callback && callback();
}

std::chrono::milliseconds
StreamedResponseWriterCore::remainingDeadline() const
{
  RemainingCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || !remaining_) return std::chrono::milliseconds(0);
    callback = remaining_;
  }
  return callback();
}

bool
StreamedResponseWriterCore::isTerminal() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return terminal_;
}

void
StreamedResponseWriterCore::invalidate()
{
  std::lock_guard<std::mutex> lock(mutex_);
  valid_ = false;
}

bool
StreamTerminalAuthority::cancel()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != StreamTerminalState::None) {
    return false;
  }
  state_ = StreamTerminalState::Cancelled;
  return true;
}

void
StreamTerminalAuthority::fence()
{
  std::lock_guard<std::mutex> lock(mutex_);
  fenced_ = true;
}

bool
StreamTerminalAuthority::isFenced() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return fenced_;
}

bool
StreamTerminalAuthority::isTerminal() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ != StreamTerminalState::None;
}

StreamTerminalState
StreamTerminalAuthority::state() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

StreamUserLifecycle::StreamUserLifecycle(
  std::shared_ptr<StreamTerminalAuthority> authority)
  : authority_(std::move(authority))
{
  if (!authority_) {
    throw std::invalid_argument("stream user lifecycle requires terminal authority");
  }
}

StreamUserLifecycleState
StreamUserLifecycle::state() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

const std::shared_ptr<StreamTerminalAuthority>&
StreamUserLifecycle::terminalAuthority() const
{
  return authority_;
}

bool
StreamUserLifecycle::isTerminal() const
{
  const auto current = state();
  return current == StreamUserLifecycleState::Completed ||
         current == StreamUserLifecycleState::Failed ||
         current == StreamUserLifecycleState::Cancelled;
}

void
StreamUserLifecycle::requireState(StreamUserLifecycleState expected) const
{
  if (state_ != expected) {
    throw std::logic_error("invalid streamed user lifecycle transition");
  }
  if (authority_->isFenced() || authority_->isTerminal()) {
    throw std::logic_error("stream user lifecycle is already fenced or terminal");
  }
}

void
StreamUserLifecycle::adoptExistingTerminal()
{
  switch (authority_->state()) {
  case StreamTerminalState::Completed:
    state_ = StreamUserLifecycleState::Completed;
    return;
  case StreamTerminalState::Failed:
    state_ = StreamUserLifecycleState::Failed;
    return;
  case StreamTerminalState::Cancelled:
    state_ = StreamUserLifecycleState::Cancelled;
    return;
  case StreamTerminalState::None:
    return;
  }
  throw std::logic_error("unknown streamed terminal state");
}

void
StreamUserLifecycle::beginRequest()
{
  std::lock_guard<std::mutex> lock(mutex_);
  requireState(StreamUserLifecycleState::Created);
  state_ = StreamUserLifecycleState::Requesting;
}

void
StreamUserLifecycle::beginSelection()
{
  std::lock_guard<std::mutex> lock(mutex_);
  requireState(StreamUserLifecycleState::Requesting);
  state_ = StreamUserLifecycleState::Selecting;
}

void
StreamUserLifecycle::beginStreaming()
{
  std::lock_guard<std::mutex> lock(mutex_);
  requireState(StreamUserLifecycleState::Selecting);
  state_ = StreamUserLifecycleState::Streaming;
}

void
StreamUserLifecycle::beginDraining()
{
  std::lock_guard<std::mutex> lock(mutex_);
  requireState(StreamUserLifecycleState::Streaming);
  state_ = StreamUserLifecycleState::Draining;
}

void
StreamUserLifecycle::complete()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StreamUserLifecycleState::Completed) return;
  if (state_ == StreamUserLifecycleState::Draining &&
      authority_->state() == StreamTerminalState::Completed) {
    state_ = StreamUserLifecycleState::Completed;
    return;
  }
  requireState(StreamUserLifecycleState::Draining);
  if (!authority_->claim(StreamTerminalState::Completed)) {
    adoptExistingTerminal();
    if (state_ != StreamUserLifecycleState::Completed) {
      throw std::logic_error("stream terminal was claimed by another outcome");
    }
    return;
  }
  state_ = StreamUserLifecycleState::Completed;
}

void
StreamUserLifecycle::fail()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StreamUserLifecycleState::Completed ||
      state_ == StreamUserLifecycleState::Failed ||
      state_ == StreamUserLifecycleState::Cancelled) {
    throw std::logic_error("duplicate streamed user terminal");
  }
  if (!authority_->claim(StreamTerminalState::Failed)) {
    adoptExistingTerminal();
    throw std::logic_error("stream terminal was claimed by another outcome");
  }
  state_ = StreamUserLifecycleState::Failed;
}

void
StreamUserLifecycle::cancel()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StreamUserLifecycleState::Completed ||
      state_ == StreamUserLifecycleState::Failed ||
      state_ == StreamUserLifecycleState::Cancelled) return;
  if (!authority_->cancel()) {
    adoptExistingTerminal();
    return;
  }
  state_ = StreamUserLifecycleState::Cancelled;
}

StreamProviderLifecycle::StreamProviderLifecycle(
  std::shared_ptr<StreamTerminalAuthority> authority)
  : authority_(std::move(authority))
{
  if (!authority_) {
    throw std::invalid_argument("stream provider lifecycle requires terminal authority");
  }
}

StreamProviderLifecycleState
StreamProviderLifecycle::state() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

const std::shared_ptr<StreamTerminalAuthority>&
StreamProviderLifecycle::terminalAuthority() const
{
  return authority_;
}

bool
StreamProviderLifecycle::isTerminal() const
{
  const auto current = state();
  return current == StreamProviderLifecycleState::Finished ||
         current == StreamProviderLifecycleState::Fenced ||
         current == StreamProviderLifecycleState::Failed ||
         current == StreamProviderLifecycleState::Cancelled;
}

void
StreamProviderLifecycle::requireState(StreamProviderLifecycleState expected) const
{
  if (state_ != expected) {
    throw std::logic_error("invalid streamed provider lifecycle transition");
  }
  if (authority_->isFenced() || authority_->isTerminal()) {
    throw std::logic_error("stream provider lifecycle is already fenced or terminal");
  }
}

void
StreamProviderLifecycle::adoptExistingTerminal()
{
  switch (authority_->state()) {
  case StreamTerminalState::Completed:
    state_ = StreamProviderLifecycleState::Finished;
    return;
  case StreamTerminalState::Failed:
    state_ = StreamProviderLifecycleState::Failed;
    return;
  case StreamTerminalState::Cancelled:
    state_ = StreamProviderLifecycleState::Cancelled;
    return;
  case StreamTerminalState::None:
    return;
  }
  throw std::logic_error("unknown streamed terminal state");
}

void
StreamProviderLifecycle::activate()
{
  std::lock_guard<std::mutex> lock(mutex_);
  requireState(StreamProviderLifecycleState::Prepared);
  state_ = StreamProviderLifecycleState::Active;
}

void
StreamProviderLifecycle::beginEnding()
{
  std::lock_guard<std::mutex> lock(mutex_);
  requireState(StreamProviderLifecycleState::Active);
  state_ = StreamProviderLifecycleState::Ending;
}

void
StreamProviderLifecycle::finish()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StreamProviderLifecycleState::Finished) {
    throw std::logic_error("duplicate streamed provider finish");
  }
  requireState(StreamProviderLifecycleState::Ending);
  if (!authority_->claim(StreamTerminalState::Completed)) {
    adoptExistingTerminal();
    throw std::logic_error("stream terminal was claimed by another outcome");
  }
  state_ = StreamProviderLifecycleState::Finished;
}

void
StreamProviderLifecycle::fail()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StreamProviderLifecycleState::Finished ||
      state_ == StreamProviderLifecycleState::Fenced ||
      state_ == StreamProviderLifecycleState::Failed ||
      state_ == StreamProviderLifecycleState::Cancelled) {
    throw std::logic_error("duplicate streamed provider terminal");
  }
  if (!authority_->claim(StreamTerminalState::Failed)) {
    adoptExistingTerminal();
    throw std::logic_error("stream terminal was claimed by another outcome");
  }
  state_ = StreamProviderLifecycleState::Failed;
}

void
StreamProviderLifecycle::cancel()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StreamProviderLifecycleState::Finished ||
      state_ == StreamProviderLifecycleState::Fenced ||
      state_ == StreamProviderLifecycleState::Failed ||
      state_ == StreamProviderLifecycleState::Cancelled) return;
  if (!authority_->cancel()) {
    adoptExistingTerminal();
    return;
  }
  state_ = StreamProviderLifecycleState::Cancelled;
}

void
StreamProviderLifecycle::fence()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == StreamProviderLifecycleState::Fenced) return;
  if (state_ == StreamProviderLifecycleState::Finished ||
      state_ == StreamProviderLifecycleState::Failed ||
      state_ == StreamProviderLifecycleState::Cancelled) {
    throw std::logic_error("cannot fence a completed streamed provider");
  }
  authority_->fence();
  state_ = StreamProviderLifecycleState::Fenced;
}

void
StreamBinding::validate() const
{
  if (requestId.empty() || requester.empty() || serviceName.empty() || producer.empty() ||
      producerBootId.empty() || producerBootId.size() > 1024 ||
      attemptEpoch < 1 || attemptEpoch > 2 ||
      allZero(planDigest) || allZero(generationId) || streamEpoch == 0 ||
      allZero(eventKeyCommitment) || userToken.empty() || policyEpoch == 0 ||
      (controllerVersion && !controllerVersion->isValid()) ||
      deadlineEpochMs == 0) {
    throw std::invalid_argument("invalid invocation stream binding");
  }
}

ndn::Buffer
StreamBinding::canonicalBytes() const
{
  validate();
  ndn::Buffer bytes;
  appendNameWire(bytes, requestId);
  appendNameWire(bytes, requester);
  appendNameWire(bytes, serviceName);
  appendNameWire(bytes, producer);
  appendLengthPrefixed(bytes, ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(producerBootId.data()), producerBootId.size()));
  appendUint64(bytes, attemptEpoch);
  appendArray(bytes, planDigest);
  appendArray(bytes, generationId);
  appendUint64(bytes, streamEpoch);
  appendArray(bytes, eventKeyCommitment);
  const auto tokenDigest = computeStreamSha256(
    ndn::span<const uint8_t>(userToken.data(), userToken.size()));
  appendArray(bytes, tokenDigest);
  appendUint64(bytes, policyEpoch);
  if (controllerVersion) {
    // Keep the legacy versionless binding transcript byte-for-byte stable.
    // New protected streams append an explicit presence marker and the
    // canonical ControllerVersion wire before the deadline field.
    appendUint64(bytes, 1);
    const auto versionWire = controllerVersion->wireEncode();
    appendLengthPrefixed(bytes, ndn::span<const uint8_t>(
      versionWire.begin(), versionWire.size()));
  }
  appendUint64(bytes, deadlineEpochMs);
  return bytes;
}

bool
StreamBinding::operator==(const StreamBinding& other) const
{
  return requestId == other.requestId && requester == other.requester &&
         serviceName == other.serviceName && producer == other.producer &&
         producerBootId == other.producerBootId && attemptEpoch == other.attemptEpoch &&
         planDigest == other.planDigest && generationId == other.generationId &&
         streamEpoch == other.streamEpoch &&
         eventKeyCommitment == other.eventKeyCommitment &&
         userToken == other.userToken && policyEpoch == other.policyEpoch &&
         controllerVersion == other.controllerVersion &&
         deadlineEpochMs == other.deadlineEpochMs;
}

StreamDigest
computeStreamSha256(ndn::span<const uint8_t> bytes)
{
  StreamDigest digest{};
  SHA256(bytes.data(), bytes.size(), digest.data());
  return digest;
}

StreamDigest
computeStreamBindingDigest(const StreamBinding& binding)
{
  static constexpr char DOMAIN[] = "NDNSF-STREAM-BINDING-V1";
  auto bytes = binding.canonicalBytes();
  ndn::Buffer input;
  input.insert(input.end(), DOMAIN, DOMAIN + sizeof(DOMAIN) - 1);
  input.insert(input.end(), bytes.begin(), bytes.end());
  return computeStreamSha256(ndn::span<const uint8_t>(input.data(), input.size()));
}

StreamDigest
computeStreamGrantBindingDigest(const StreamBinding& binding)
{
  auto preSelectionBinding = binding;
  static constexpr char DOMAIN[] = "NDNSF-STREAM-GRANT-PRESELECTION-V1";
  preSelectionBinding.planDigest = computeStreamSha256(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(DOMAIN), sizeof(DOMAIN) - 1));
  return computeStreamBindingDigest(preSelectionBinding);
}

StreamDigest
initialStreamTranscriptDigest()
{
  static constexpr char DOMAIN[] = "NDNSF-STREAM-V1";
  return computeStreamSha256(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(DOMAIN), sizeof(DOMAIN) - 1));
}

StreamDigest
advanceStreamTranscriptDigest(
  const StreamDigest& previous, uint64_t cursor, InvocationEventType eventType,
  const StreamDigest& payloadDigest)
{
  if (cursor == 0 || eventType != InvocationEventType::Application) {
    throw std::invalid_argument("invalid transcript event");
  }
  ndn::Buffer input;
  appendArray(input, previous);
  appendUint64(input, cursor);
  appendUint64(input, static_cast<uint64_t>(eventType));
  appendArray(input, payloadDigest);
  return computeStreamSha256(ndn::span<const uint8_t>(input.data(), input.size()));
}

StreamNonce
deriveInvocationEventNonce(
  const StreamDigest& eventKey, const StreamDigest& bindingDigest, uint64_t cursor)
{
  if (cursor == 0 || allZero(eventKey) || allZero(bindingDigest)) {
    throw std::invalid_argument("invalid event nonce input");
  }
  static constexpr char DOMAIN[] = "NDNSF-EVENT-NONCE-V1";
  ndn::Buffer input;
  input.insert(input.end(), DOMAIN, DOMAIN + sizeof(DOMAIN) - 1);
  appendArray(input, bindingDigest);
  appendUint64(input, cursor);
  unsigned int outputSize = 0;
  std::array<uint8_t, SHA256_DIGEST_LENGTH> output{};
  if (HMAC(EVP_sha256(), eventKey.data(), static_cast<int>(eventKey.size()),
           input.data(), input.size(), output.data(), &outputSize) == nullptr ||
      outputSize != output.size()) {
    throw std::runtime_error("event nonce derivation failed");
  }
  StreamNonce nonce{};
  std::copy_n(output.begin(), nonce.size(), nonce.begin());
  return nonce;
}

ndn::Buffer
makeInvocationEventAssociatedData(
  const ndn::Name& dataName, const StreamDigest& bindingDigest, uint64_t cursor)
{
  if (dataName.empty() || allZero(bindingDigest) || cursor == 0) {
    throw std::invalid_argument("invalid event associated data input");
  }
  ndn::Buffer result;
  const auto nameWire = dataName.wireEncode();
  appendRaw(result, ndn::span<const uint8_t>(nameWire.begin(), nameWire.size()));
  appendUint64(result, tlv::InvocationEventMessageType);
  appendArray(result, bindingDigest);
  appendUint64(result, cursor);
  return result;
}

bool
isMatchingStreamCompletion(
  const StreamCompletion& completion, const ndn::Name& observedEndEventName,
  const InvocationEventMessage& observedEndEvent,
  ndn::span<const uint8_t> finalResponsePayload)
{
  try {
    completion.validate();
    observedEndEvent.validate();
    if (observedEndEvent.eventType != InvocationEventType::End ||
        !observedEndEvent.terminal || !observedEndEvent.end) {
      return false;
    }
    const auto& end = *observedEndEvent.end;
    return completion.bindingDigest == observedEndEvent.bindingDigest &&
           completion.endEventName == observedEndEventName &&
           completion.finalCursor == observedEndEvent.cursor &&
           completion.finalCursor == end.finalCursor &&
           completion.finishReason == end.finishReason &&
           completion.applicationEventCount == end.applicationEventCount &&
           completion.transcriptDigest == end.transcriptDigest &&
           completion.finalResultDigest == end.finalResultDigest &&
           completion.finalResultDigest == computeStreamSha256(finalResponsePayload);
  }
  catch (const std::exception&) {
    return false;
  }
}

ndn::Name
makeInvocationEventName(const StreamBinding& binding, uint64_t cursor)
{
  binding.validate();
  if (cursor == 0) {
    throw std::invalid_argument("stream event cursor must be positive");
  }
  ndn::Name name(binding.producer);
  name.append("NDNSF").append("EVENT");
  appendRequesterComponent(name, binding.requester);
  name.append(binding.serviceName);
  appendRequestIdComponent(name, binding.requestId);
  name.appendNumber(binding.attemptEpoch);
  name.append(toLowerHex(ndn::span<const uint8_t>(
    binding.planDigest.data(), binding.planDigest.size())));
  name.append(toLowerHex(ndn::span<const uint8_t>(
    binding.generationId.data(), binding.generationId.size())));
  name.appendNumber(binding.streamEpoch);
  name.appendNumber(cursor);
  return name;
}

std::optional<ParsedInvocationEventName>
parseInvocationEventName(const ndn::Name& name)
{
  try {
    std::optional<size_t> marker;
    for (size_t index = 0; index + 1 < name.size(); ++index) {
      if (name.get(index).toUri() == "NDNSF" &&
          name.get(index + 1).toUri() == "EVENT") {
        if (marker) return std::nullopt;
        marker = index;
      }
    }
    if (!marker || *marker == 0) return std::nullopt;
    const size_t requesterIndex = *marker + 2;
    const size_t serviceIndex = requesterIndex + 1;
    constexpr size_t TRAILING_COMPONENTS = 6;
    if (name.size() < serviceIndex + 1 + TRAILING_COMPONENTS) return std::nullopt;
    const size_t serviceCount = name.size() - serviceIndex - TRAILING_COMPONENTS;
    if (serviceCount == 0) return std::nullopt;
    const size_t requestIdIndex = serviceIndex + serviceCount;
    const size_t attemptIndex = requestIdIndex + 1;
    const size_t planIndex = attemptIndex + 1;
    const size_t generationIndex = planIndex + 1;
    const size_t epochIndex = generationIndex + 1;
    const size_t cursorIndex = epochIndex + 1;
    const auto requester = parseRequesterComponent(name.get(requesterIndex));
    if (!requester || !name.get(attemptIndex).isNumber() ||
        !name.get(epochIndex).isNumber() || !name.get(cursorIndex).isNumber()) {
      return std::nullopt;
    }
    const auto plan = fromLowerHex<32>(name.get(planIndex).toUri());
    const auto generation = fromLowerHex<16>(name.get(generationIndex).toUri());
    if (!plan || !generation) return std::nullopt;
    ParsedInvocationEventName parsed;
    parsed.producer = subName(name, 0, *marker);
    parsed.requester = *requester;
    parsed.serviceName = subName(name, serviceIndex, serviceCount);
    const auto requestId = parseRequestIdComponent(name.get(requestIdIndex));
    if (!requestId) return std::nullopt;
    parsed.requestId = *requestId;
    parsed.attemptEpoch = name.get(attemptIndex).toNumber();
    parsed.planDigest = *plan;
    parsed.generationId = *generation;
    parsed.streamEpoch = name.get(epochIndex).toNumber();
    parsed.cursor = name.get(cursorIndex).toNumber();
    if (parsed.attemptEpoch < 1 || parsed.attemptEpoch > 2 ||
        parsed.streamEpoch == 0 || parsed.cursor == 0 ||
        allZero(parsed.planDigest) || allZero(parsed.generationId)) return std::nullopt;

    ndn::Name canonical(parsed.producer);
    canonical.append("NDNSF").append("EVENT");
    appendRequesterComponent(canonical, parsed.requester);
    canonical.append(parsed.serviceName);
    appendRequestIdComponent(canonical, parsed.requestId);
    canonical.appendNumber(parsed.attemptEpoch);
    canonical.append(toLowerHex(ndn::span<const uint8_t>(parsed.planDigest.data(), parsed.planDigest.size())));
    canonical.append(toLowerHex(ndn::span<const uint8_t>(parsed.generationId.data(), parsed.generationId.size())));
    canonical.appendNumber(parsed.streamEpoch).appendNumber(parsed.cursor);
    if (canonical != name) return std::nullopt;
    return parsed;
  }
  catch (const std::exception&) {
    return std::nullopt;
  }
}

StreamEventPublisher::StreamEventPublisher(
  StreamBinding binding, StreamRequestOptions options, ndn::Buffer eventKey,
  std::shared_ptr<StreamInvocationLifecycle> lifecycle, ndn::KeyChain& keyChain,
  ndn::security::SigningInfo signingInfo, PublishCallback publish)
  : binding_(std::move(binding))
  , options_(std::move(options))
  , eventKey_(std::move(eventKey))
  , lifecycle_(std::move(lifecycle))
  , keyChain_(keyChain)
  , signingInfo_(std::move(signingInfo))
  , publish_(std::move(publish))
  , admissionQueue_(options_.publisherQueueCapacity)
{
  options_.validate();
  binding_.validate();
  if (!lifecycle_) {
    throw std::invalid_argument("stream event publisher requires lifecycle owner");
  }
  if (eventKey_.size() != 32 ||
      computeStreamSha256(ndn::span<const uint8_t>(eventKey_.data(), eventKey_.size())) !=
        binding_.eventKeyCommitment) {
    throw std::invalid_argument("stream event key does not match commitment");
  }
}

void
StreamEventPublisher::start()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    throw std::logic_error("stream event publisher already started");
  }
  lifecycle_->provider().activate();
  started_ = true;
}

std::optional<PublishedStreamEvent>
StreamEventPublisher::encodeAndSign(const InvocationEventMessage& event,
                                    uint64_t cursor) const
{
  event.validate();
  const auto dataName = makeInvocationEventName(binding_, cursor);
  const auto plaintextBlock = event.wireEncode();
  if (plaintextBlock.size() > options_.maxEventWireBytes) {
    return std::nullopt;
  }
  const auto bindingDigest = computeStreamBindingDigest(binding_);
  StreamDigest eventKeyDigest{};
  std::copy(eventKey_.begin(), eventKey_.end(), eventKeyDigest.begin());
  const auto nonce = deriveInvocationEventNonce(
    eventKeyDigest, bindingDigest, cursor);
  const auto associatedData = makeInvocationEventAssociatedData(
    dataName, bindingDigest, cursor);
  const auto encrypted = hybridAesGcmEncryptWithNonce(
    eventKey_, ndn::span<const uint8_t>(nonce.data(), nonce.size()),
    ndn::span<const uint8_t>(plaintextBlock.data(), plaintextBlock.size()),
    ndn::span<const uint8_t>(associatedData.data(), associatedData.size()));

  HybridMessageEnvelope envelope;
  envelope.setKeyId("NDNSF-STREAM-V1-" +
                   toLowerHex(ndn::span<const uint8_t>(
                     binding_.eventKeyCommitment.data(),
                     binding_.eventKeyCommitment.size())));
  envelope.setEpochId(std::to_string(binding_.streamEpoch));
  envelope.setMessageType("EVENT");
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);

  ndn::Data data(dataName);
  data.setFreshnessPeriod(ndn::time::milliseconds(options_.retentionMs));
  data.setContent(envelope.WireEncode());
  keyChain_.sign(data, signingInfo_);
  const auto wire = data.wireEncode();

  PublishedStreamEvent result;
  result.name = dataName;
  result.cursor = cursor;
  result.terminal = event.terminal;
  result.signedWire = ndn::Buffer(wire.begin(), wire.end());
  return result;
}

void
StreamEventPublisher::reapExpiredLocked(std::chrono::steady_clock::time_point now)
{
  for (auto it = retained_.begin(); it != retained_.end();) {
    if (it->second.expiresAt <= now) {
      it = retained_.erase(it);
    }
    else {
      ++it;
    }
  }
}

void
StreamEventPublisher::failProviderLocked()
{
  failed_ = true;
  admissionQueue_.close();
  if (!lifecycle_->terminalAuthority()->isTerminal() && started_) {
    try {
      lifecycle_->provider().fail();
    }
    catch (const std::logic_error&) {
      // A concurrent terminal claimant owns the outcome.
    }
  }
}

void
StreamEventPublisher::releaseAdmission()
{
  InvocationEventMessage ignored;
  admissionQueue_.tryPop(ignored);
}

std::optional<PublishedStreamEvent>
StreamEventPublisher::makeAndRetain(InvocationEventMessage event,
                                    std::chrono::steady_clock::time_point deadline)
{
  if (!admissionQueue_.waitPush(event, deadline)) {
    failProviderLocked();
    return std::nullopt;
  }

  const auto cursor = nextCursor_;
  // Cursor/transcript ownership is committed only after queue admission. If
  // encryption or signing fails below, the provider is terminal and this
  // cursor is never reused for a later event.
  ++nextCursor_;
  if (event.eventType == InvocationEventType::Application) {
    ++applicationEventCount_;
    transcriptDigest_ = advanceStreamTranscriptDigest(
      transcriptDigest_, cursor, event.eventType, event.payloadDigest);
  }

  try {
    auto encoded = encodeAndSign(event, cursor);
    if (!encoded) {
      releaseAdmission();
      failProviderLocked();
      return std::nullopt;
    }
    retained_[encoded->name.toUri()] = Retained{
      *encoded, std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options_.retentionMs)};
    return encoded;
  }
  catch (const std::exception& error) {
    releaseAdmission();
    failProviderLocked();
    return std::nullopt;
  }
}

void
StreamEventPublisher::emit(const PublishedStreamEvent& event) const
{
  if (publish_) {
    publish_(event);
  }
}

std::optional<PublishedStreamEvent>
StreamEventPublisher::publish(const ndn::Buffer& payload,
                              std::chrono::steady_clock::time_point deadline)
{
  std::optional<PublishedStreamEvent> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    reapExpiredLocked(std::chrono::steady_clock::now());
    if (!started_ || finished_ || failed_ || lifecycle_->terminalAuthority()->isTerminal() ||
        nextCursor_ > options_.maxEvents) {
      return std::nullopt;
    }
    InvocationEventMessage event;
    event.bindingDigest = computeStreamBindingDigest(binding_);
    event.cursor = nextCursor_;
    event.publishedAtUs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    event.userToken = binding_.userToken;
    event.setPayload(payload);
    if (event.wireEncode().size() > options_.maxEventWireBytes) {
      failProviderLocked();
      return std::nullopt;
    }
    result = makeAndRetain(std::move(event), deadline);
  }
  if (result) {
    bool emitted = true;
    try {
      emit(*result);
    }
    catch (const std::exception& error) {
      emitted = false;
    }
    releaseAdmission();
    if (!emitted) {
      std::lock_guard<std::mutex> lock(mutex_);
      failProviderLocked();
      // `makeAndRetain` reserves the cursor and keeps the exact signed bytes
      // before the transport commit.  If the Provider Face rejects the
      // commit (for example after Controller revocation), that event must not
      // remain available through the local retry/retention path and the
      // caller must observe publication failure rather than a false success.
      retained_.erase(result->name.toUri());
      result.reset();
    }
  }
  return result;
}

std::optional<StreamCompletion>
StreamEventPublisher::finish(const ndn::Buffer& finalResult,
                             StreamFinishReason reason,
                             std::chrono::steady_clock::time_point deadline)
{
  std::optional<PublishedStreamEvent> endEvent;
  StreamCompletion completion;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    reapExpiredLocked(std::chrono::steady_clock::now());
    if (!started_ || finished_ || failed_ || lifecycle_->terminalAuthority()->isTerminal() ||
        nextCursor_ > options_.maxEvents) {
      return std::nullopt;
    }
    InvocationEventMessage event;
    event.bindingDigest = computeStreamBindingDigest(binding_);
    event.cursor = nextCursor_;
    event.publishedAtUs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    event.userToken = binding_.userToken;
    event.eventType = InvocationEventType::End;
    event.terminal = true;
    event.setPayload(ndn::Buffer());
    StreamEndEvent end;
    end.finalCursor = event.cursor;
    end.finishReason = reason;
    end.applicationEventCount = applicationEventCount_;
    end.generatedTokenCount = applicationEventCount_;
    end.transcriptDigest = transcriptDigest_;
    end.finalResultDigest = computeStreamSha256(
      ndn::span<const uint8_t>(finalResult.data(), finalResult.size()));
    event.end = end;
    endEvent = makeAndRetain(std::move(event), deadline);
    if (!endEvent) {
      return std::nullopt;
    }
    completion.bindingDigest = computeStreamBindingDigest(binding_);
    completion.endEventName = endEvent->name;
    completion.finalCursor = end.finalCursor;
    completion.finishReason = reason;
    completion.applicationEventCount = end.applicationEventCount;
    completion.transcriptDigest = end.transcriptDigest;
    completion.finalResultDigest = end.finalResultDigest;
    completion.validate();
    lifecycle_->provider().beginEnding();
    lifecycle_->provider().finish();
    finished_ = true;
  }
  try {
    emit(*endEvent);
  }
  catch (const std::exception&) {
    // The terminal is already committed; transport retry uses retained Data.
  }
  releaseAdmission();
  return completion;
}

bool
StreamEventPublisher::fail(const std::string&)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (finished_ || failed_) {
    return false;
  }
  failProviderLocked();
  return true;
}

std::optional<PublishedStreamEvent>
StreamEventPublisher::satisfy(const ndn::Interest& interest)
{
  if (interest.getCanBePrefix()) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  reapExpiredLocked(std::chrono::steady_clock::now());
  const auto it = retained_.find(interest.getName().toUri());
  if (it == retained_.end()) {
    return std::nullopt;
  }
  return it->second.event;
}

std::optional<PublishedStreamEvent>
StreamEventPublisher::republish(const ndn::Name& name)
{
  std::optional<PublishedStreamEvent> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    reapExpiredLocked(std::chrono::steady_clock::now());
    const auto it = retained_.find(name.toUri());
    if (it == retained_.end()) {
      return std::nullopt;
    }
    result = it->second.event;
  }
  try {
    emit(*result);
  }
  catch (const std::exception&) {
    // Retained bytes remain available for a later exact retry.
  }
  return result;
}

size_t
StreamEventPublisher::retainedCount() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return retained_.size();
}

size_t
StreamEventPublisher::highWaterMark() const
{
  return admissionQueue_.highWaterMark();
}

uint64_t
StreamEventPublisher::nextCursor() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return nextCursor_;
}

const StreamDigest&
StreamEventPublisher::transcriptDigest() const
{
  return transcriptDigest_;
}

StreamEventConsumer::StreamEventConsumer(
  StreamBinding binding, StreamRequestOptions options, ndn::Buffer eventKey,
  std::shared_ptr<StreamInvocationLifecycle> lifecycle, VerifyCallback verify,
  EventCallback onEvent, CompletionCallback onComplete, ErrorCallback onError,
  RetryCallback onRetry, RetryAccountingCallback onRetryAccounting,
  std::string expectedProgressOperationId)
  : binding_(std::move(binding))
  , options_(std::move(options))
  , eventKey_(std::move(eventKey))
  , lifecycle_(std::move(lifecycle))
  , verify_(std::move(verify))
  , onEvent_(std::move(onEvent))
  , onComplete_(std::move(onComplete))
  , onError_(std::move(onError))
  , onRetry_(std::move(onRetry))
  , onRetryAccounting_(std::move(onRetryAccounting))
  , expectedProgressOperationId_(std::move(expectedProgressOperationId))
  , callbackQueue_(options_.callbackQueueCapacity)
{
  options_.validate();
  binding_.validate();
  if (!lifecycle_) {
    throw std::invalid_argument("stream event consumer requires lifecycle owner");
  }
  if (eventKey_.size() != 32 ||
      computeStreamSha256(ndn::span<const uint8_t>(eventKey_.data(), eventKey_.size())) !=
        binding_.eventKeyCommitment) {
    throw std::invalid_argument("stream event key does not match commitment");
  }
}

StreamEventConsumer::~StreamEventConsumer()
{
  stopCallbackWorker();
}

void
StreamEventConsumer::start()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      throw std::logic_error("stream event consumer already started");
    }
    lifecycle_->user().beginRequest();
    lifecycle_->user().beginSelection();
    lifecycle_->user().beginStreaming();
    started_ = true;
  }
  callbackThread_ = std::thread([this] { callbackWorkerLoop(); });
}

void
StreamEventConsumer::prefetchWindow()
{
  std::vector<ndn::Name> names;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || failed_ || complete_ || !onRetry_) {
      return;
    }
    const auto count = std::min<uint64_t>(options_.interestWindow,
                                          options_.maxEvents);
    const auto firstCursor = expectedCursor_;
    prefetchStarted_ = true;
    names.reserve(count);
    for (uint64_t cursor = firstCursor; cursor < firstCursor + count;
         ++cursor) {
      initialInterestCursors_.insert(cursor);
      names.push_back(makeInvocationEventName(binding_, cursor));
    }
  }
  // Do not hold the consumer mutex while handing names to the transport.  In
  // the production User path the callback posts to the Face io_context, but
  // test transports may invoke synchronously.
  for (const auto& name : names) {
    onRetry_(name);
  }
}

void
StreamEventConsumer::setAuthorizationCallback(AuthorizationCallback callback)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    throw std::logic_error(
        "stream event authorization callback must be set before start");
  }
  authorization_ = std::move(callback);
}

bool
StreamEventConsumer::decodeAndValidate(const ndn::Data& data,
                                       InvocationEventMessage& event,
                                       StreamedInvocationErrorCode& code,
                                       std::string& message) const
{
  if (verify_ && !verify_(data)) {
    code = StreamedInvocationErrorCode::InvalidSignature;
    message = "stream event signer does not match selected Provider";
    return false;
  }
  const auto parsed = parseInvocationEventName(data.getName());
  if (!parsed || data.getName() != makeInvocationEventName(binding_, parsed->cursor)) {
    code = StreamedInvocationErrorCode::InvalidName;
    message = "stream event name is not canonical";
    return false;
  }
  if (parsed->requestId != binding_.requestId ||
      parsed->requester != binding_.requester ||
      parsed->serviceName != binding_.serviceName ||
      parsed->producer != binding_.producer ||
      parsed->attemptEpoch != binding_.attemptEpoch ||
      parsed->planDigest != binding_.planDigest ||
      parsed->generationId != binding_.generationId ||
      parsed->streamEpoch != binding_.streamEpoch) {
    code = StreamedInvocationErrorCode::BindingMismatch;
    message = "stream event name lineage does not match the accepted binding";
    return false;
  }

  try {
    const auto& content = data.getContent();
    const auto envelopeBlock = content.type() == tlv::HybridMessageEnvelopeType ?
      content : content.blockFromValue();
    HybridMessageEnvelope envelope;
    if (!envelope.WireDecode(envelopeBlock) || envelope.getMessageType() != "EVENT") {
      code = StreamedInvocationErrorCode::DecryptionFailed;
      message = "stream event envelope is invalid";
      return false;
    }
    const auto bindingDigest = computeStreamBindingDigest(binding_);
    const auto associatedData = makeInvocationEventAssociatedData(
      data.getName(), bindingDigest, parsed->cursor);
    ndn::Buffer plaintext;
    if (!hybridAesGcmDecrypt(
          eventKey_, envelope,
          ndn::span<const uint8_t>(associatedData.data(), associatedData.size()),
          plaintext)) {
      code = StreamedInvocationErrorCode::DecryptionFailed;
      message = "stream event AEAD verification failed";
      return false;
    }
    if (!event.wireDecode(ndn::Block(plaintext))) {
      code = StreamedInvocationErrorCode::BindingMismatch;
      message = "stream event plaintext is not a canonical event";
      return false;
    }
    event.validate();
    if (event.cursor != parsed->cursor) {
      code = StreamedInvocationErrorCode::CursorMismatch;
      message = "stream event cursor does not match its name";
      return false;
    }
    if (event.bindingDigest != bindingDigest ||
        event.userToken != binding_.userToken) {
      code = StreamedInvocationErrorCode::BindingMismatch;
      message = "stream event payload binding does not match the accepted request";
      return false;
    }
    if (event.terminal != (event.eventType == InvocationEventType::End)) {
      code = StreamedInvocationErrorCode::TerminalMismatch;
      message = "stream event terminal shape is invalid";
      return false;
    }
    return true;
  }
  catch (const std::exception&) {
    code = StreamedInvocationErrorCode::BindingMismatch;
    message = "stream event validation failed";
    return false;
  }
}

void
StreamEventConsumer::requestGapRetry()
{
  std::optional<ndn::Name> retryName;
  bool exhausted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_ || complete_) {
      return;
    }
    // The initial exact-interest window is already in flight for this
    // cursor.  Wait for its authoritative timeout before consuming retry
    // budget; otherwise a future SVS event causes a duplicate same-name
    // Interest and its timeout can be mistaken for another retry.
    if (initialInterestCursors_.count(expectedCursor_) != 0) {
      return;
    }
    if (retryInFlightCursor_ && *retryInFlightCursor_ == expectedCursor_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (nextRetryAt_ != std::chrono::steady_clock::time_point{} && now < nextRetryAt_) {
      return;
    }
    if (retryCount_ >= options_.maxEventRetries) {
      exhausted = true;
    }
    else {
      ++retryCount_;
      retryInFlightCursor_ = expectedCursor_;
      nextRetryAt_ = now + std::chrono::milliseconds(options_.interestLifetimeMs);
      retryName = makeInvocationEventName(binding_, expectedCursor_);
    }
  }
  if (exhausted) {
    fail(StreamedInvocationErrorCode::EventTimeout,
         "stream event gap exceeded retry budget");
  }
  else if (retryName && onRetry_) {
    if (onRetryAccounting_) {
      onRetryAccounting_(*retryName);
    }
    onRetry_(*retryName);
  }
}

bool
StreamEventConsumer::dispatchApplicationCallback(
  const InvocationEventMessage& event,
  StreamedInvocationErrorCode& code,
  std::string& message)
{
  auto completion = std::make_shared<std::promise<void>>();
  auto finished = completion->get_future();
  const auto systemNow = std::chrono::system_clock::now();
  const auto epochNow = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      systemNow.time_since_epoch()).count());
  const auto remainingMs = binding_.deadlineEpochMs > epochNow ?
    binding_.deadlineEpochMs - epochNow : 0;
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(remainingMs);
  if (!callbackQueue_.waitPush(CallbackTask{event, completion}, deadline)) {
    code = StreamedInvocationErrorCode::QueueDeadline;
    message = "stream callback queue deadline expired";
    return false;
  }
  if (finished.wait_until(deadline) != std::future_status::ready) {
    code = StreamedInvocationErrorCode::QueueDeadline;
    message = "stream callback execution deadline expired";
    return false;
  }
  try {
    finished.get();
    return true;
  }
  catch (const std::exception&) {
    code = StreamedInvocationErrorCode::ApplicationCallbackFailed;
    message = "stream event callback failed";
    return false;
  }
}

void
StreamEventConsumer::callbackWorkerLoop()
{
  while (true) {
    CallbackTask task;
    if (!callbackQueue_.waitPop(
          task, std::chrono::steady_clock::now() + std::chrono::milliseconds(100))) {
      if (callbackQueue_.isClosed()) {
        return;
      }
      continue;
    }
    try {
      if (onEvent_) {
        onEvent_(task.event);
      }
      task.completion->set_value();
    }
    catch (...) {
      task.completion->set_exception(std::current_exception());
    }
  }
}

void
StreamEventConsumer::stopCallbackWorker()
{
  callbackQueue_.close();
  if (callbackThread_.joinable()) {
    if (callbackThread_.get_id() == std::this_thread::get_id()) {
      callbackThread_.detach();
    }
    else {
      callbackThread_.join();
    }
  }
}

bool
StreamEventConsumer::deliverReady()
{
  while (true) {
    InvocationEventMessage event;
    std::shared_ptr<ResponseMessage> responseAfterEnd;
    bool hasEvent = false;
    bool retryTimedOutGap = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = reorder_.find(expectedCursor_);
      if (it == reorder_.end()) {
        retryTimedOutGap = timedOutCursors_.erase(expectedCursor_) != 0;
      }
      else {
        hasEvent = true;
        event = std::move(it->second);
        reorder_.erase(it);
        initialInterestCursors_.erase(event.cursor);
        if (retryInFlightCursor_ && *retryInFlightCursor_ == event.cursor) {
          retryInFlightCursor_.reset();
        }
        timedOutCursors_.erase(event.cursor);
        ++expectedCursor_;
        // Retry budget is per cursor. A late timeout/retry for a delivered
        // cursor must not consume the next cursor's bounded budget.
        retryCount_ = 0;
        nextRetryAt_ = std::chrono::steady_clock::time_point{};
        if (event.terminal) {
          observedEnd_ = event;
          observedEndName_ = makeInvocationEventName(binding_, event.cursor);
          responseAfterEnd = std::move(pendingResponse_);
        }
      }
    }
    if (!hasEvent) {
      bool endObserved = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        endObserved = observedEnd_.has_value();
      }
      // End is the terminal stream event.  Its cursor advances the ordered
      // event frontier, but the encrypted Response may arrive separately and
      // later.  Do not manufacture a post-End cursor gap while waiting for
      // that Response to close the stream.
      if (retryTimedOutGap && !endObserved) {
        requestGapRetry();
      }
      std::lock_guard<std::mutex> lock(mutex_);
      return !failed_;
    }

    // Cancellation/fencing may race with an application callback.  The
    // callback is allowed to cancel the handle; do not drain the remaining
    // buffered events after that terminal claim.
    if (lifecycle_->terminalAuthority()->isTerminal() ||
        lifecycle_->terminalAuthority()->isFenced()) {
      return false;
    }

    AuthorizationCallback authorization;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      authorization = authorization_;
    }
    if (authorization && !authorization()) {
      fail(StreamedInvocationErrorCode::Unauthorized,
           "stream event rejected by Controller revocation state");
      return false;
    }

    if (event.eventType == InvocationEventType::Application) {
      auto code = StreamedInvocationErrorCode::ApplicationCallbackFailed;
      std::string message;
      if (!dispatchApplicationCallback(event, code, message)) {
        fail(code, message);
        return false;
      }
    }
    else {
      try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!lifecycle_->user().isTerminal()) {
          lifecycle_->user().beginDraining();
        }
      }
      catch (const std::logic_error&) {
        fail(StreamedInvocationErrorCode::TerminalMismatch,
             "stream user lifecycle could not enter draining");
        return false;
      }
    }

    // Response and End are independent encrypted Data packets. A provider
    // may enqueue Response before the exact End Interest is satisfied,
    // especially for a zero-event stream. Complete only after End delivery,
    // using the same completion-binding validation path.
    if (responseAfterEnd && !acceptResponse(*responseAfterEnd)) {
      return false;
    }
  }
}

bool
StreamEventConsumer::bufferOrDeliver(InvocationEventMessage event)
{
  bool requestRetry = false;
  bool overflow = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || failed_ || complete_) {
      return false;
    }
    if (event.cursor < expectedCursor_) {
      return true; // exact duplicate or a late retry
    }
    if (reorder_.find(event.cursor) != reorder_.end()) {
      return true;
    }
    if (event.cursor > expectedCursor_) {
      if (reorder_.size() >= options_.reorderCapacity) {
        // Fail outside the mutex so the error callback cannot re-enter us.
        overflow = true;
      }
      else {
        reorder_.emplace(event.cursor, std::move(event));
        requestRetry = true;
      }
    }
    else {
      reorder_.emplace(event.cursor, std::move(event));
    }
  }
  if (overflow) {
    fail(StreamedInvocationErrorCode::QueueDeadline,
         "stream reorder capacity exceeded");
    return false;
  }
  if (requestRetry) {
    requestGapRetry();
  }
  return deliverReady();
}

bool
StreamEventConsumer::accept(const ndn::Data& data)
{
  InvocationEventMessage event;
  auto code = StreamedInvocationErrorCode::BindingMismatch;
  std::string message;
  if (!decodeAndValidate(data, event, code, message)) {
    fail(code, message);
    return false;
  }
  return bufferOrDeliver(std::move(event));
}

bool
StreamEventConsumer::acceptResponse(const ResponseMessage& response)
{
  std::optional<InvocationEventMessage> end;
  ndn::Name endName;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_ || complete_) {
      return false;
    }
    if (!observedEnd_) {
      // Keep one immutable copy until End closes the cursor. Duplicate
      // Responses are harmless while the first copy is pending.
      if (!pendingResponse_) {
        pendingResponse_ = std::make_shared<ResponseMessage>(response);
      }
      return true;
    }
    end = observedEnd_;
    endName = observedEndName_;
  }
  if (!response.hasStreamCompletion() ||
      !isMatchingStreamCompletion(response.getStreamCompletion(), endName, *end,
                                   ndn::span<const uint8_t>(
                                     response.getPayload().data(),
                                     response.getPayload().size()))) {
    fail(StreamedInvocationErrorCode::TerminalMismatch,
         "stream Response completion does not match End");
    return false;
  }
  try {
    lifecycle_->user().complete();
  }
  catch (const std::logic_error&) {
    fail(StreamedInvocationErrorCode::TerminalMismatch,
         "stream user lifecycle could not complete");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    complete_ = true;
  }
  stopCallbackWorker();
  if (onComplete_) {
    onComplete_(response);
  }
  return true;
}

bool
StreamEventConsumer::observeAuthenticatedProgress(
  const SelectionExecutionStatus& status)
{
  const auto epochNow = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  if (binding_.deadlineEpochMs != 0 && epochNow >= binding_.deadlineEpochMs) {
    return false;
  }
  if (!status.providerName.equals(binding_.producer) ||
      !status.serviceName.equals(binding_.serviceName) ||
      !status.requestId.equals(binding_.requestId) ||
      status.selectionDigest.empty()) {
    return false;
  }
  const auto selectionDigest = computeStreamSha256(
    ndn::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(status.selectionDigest.data()),
      status.selectionDigest.size()));
  if (selectionDigest != binding_.planDigest) {
    return false;
  }

  const CollaborationMemberStatus* candidate = nullptr;
  for (const auto& member : status.memberStatuses) {
    if (!member.providerName.equals(binding_.producer) ||
        !member.serviceName.equals(binding_.serviceName) ||
        !member.requestId.equals(binding_.requestId) ||
        member.selectionDigest != status.selectionDigest ||
        member.operation != "ensure-deployment" ||
        member.operationId.empty() || member.sequence == 0 ||
        expectedProgressOperationId_.empty() ||
        member.operationId != expectedProgressOperationId_ ||
        member.state != "RUNNING" ||
        member.detailsSchema != "ndnsf-di-preparation-progress-v1") {
      continue;
    }
    if (candidate == nullptr || member.epoch > candidate->epoch ||
        (member.epoch == candidate->epoch &&
         member.sequence > candidate->sequence)) {
      candidate = &member;
    }
  }
  if (candidate == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (failed_ || complete_ || observedEnd_) {
    return false;
  }
  // One stream consumer belongs to one terminal Provider role.  Keep the
  // first authenticated assembly operation as its source and ignore status
  // members from other roles in the same collaboration snapshot.
  if (!progressOperationId_.empty() &&
      progressOperationId_ != candidate->operationId) {
    return false;
  }
  if (progressOperationId_ == candidate->operationId &&
      (candidate->epoch < progressEpoch_ ||
       (candidate->epoch == progressEpoch_ &&
        candidate->sequence <= progressSequence_))) {
    return false;
  }
  progressOperationId_ = candidate->operationId;
  progressEpoch_ = candidate->epoch;
  progressSequence_ = candidate->sequence;
  retryCount_ = 0;
  nextRetryAt_ = std::chrono::steady_clock::time_point{};
  return true;
}

void
StreamEventConsumer::onInactivityTimeout(
  std::chrono::steady_clock::time_point now)
{
  (void)now;
  requestGapRetry();
}

void
StreamEventConsumer::onRetryTimeout(
  const ndn::Name& timedOutName,
  std::chrono::steady_clock::time_point now)
{
  (void)now;
  const auto parsed = parseInvocationEventName(timedOutName);
  if (!parsed || timedOutName != makeInvocationEventName(binding_, parsed->cursor)) {
    return;
  }
  bool due = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // InterestWindow speculatively expresses several exact names. Only a
    // timeout for the cursor currently blocking delivery may consume retry
    // budget. Unlike the old reorder-only condition, this also detects a
    // Provider that becomes completely silent after the last delivered event.
    if (failed_ || complete_ || observedEnd_ || parsed->cursor < expectedCursor_) {
      return;
    }
    const bool initialInterest =
      initialInterestCursors_.erase(parsed->cursor) != 0;
    timedOutCursors_.insert(parsed->cursor);
    // Direct users of the low-level consumer may express an initial exact
    // Interest themselves instead of calling prefetchWindow().  In that
    // mode there is no speculative-window bookkeeping, so the timeout for
    // the expected cursor is still authoritative.
    const bool externallyTrackedInterest =
      !prefetchStarted_ && initialInterestCursors_.empty() &&
      !retryInFlightCursor_;
    due = parsed->cursor == expectedCursor_ &&
          (initialInterest ||
           (retryInFlightCursor_ && *retryInFlightCursor_ == parsed->cursor) ||
           externallyTrackedInterest);
    if (due) {
      // This callback is the authoritative expiry of an actually expressed
      // exact Interest. Timer granularity may report it a few ticks before the
      // locally estimated nextRetryAt_; refusing it would leave no future
      // callback and stall the stream forever. Clear the estimate so this
      // observed timeout advances the bounded retry state machine exactly once.
      timedOutCursors_.erase(parsed->cursor);
      nextRetryAt_ = std::chrono::steady_clock::time_point{};
      if (!initialInterest && retryInFlightCursor_ &&
          *retryInFlightCursor_ == parsed->cursor) {
        retryInFlightCursor_.reset();
      }
    }
  }
  if (due) {
    requestGapRetry();
  }
}

void
StreamEventConsumer::reject(StreamedInvocationErrorCode code,
                            const std::string& message)
{
  fail(code, message);
}

void
StreamEventConsumer::fail(StreamedInvocationErrorCode code,
                          const std::string& message)
{
  ErrorCallback callback;
  uint64_t expectedCursor = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_ || complete_) {
      return;
    }
    failed_ = true;
    expectedCursor = expectedCursor_;
    callback = onError_;
  }
  try {
    if (!lifecycle_->terminalAuthority()->isTerminal() && started_) {
      lifecycle_->user().fail();
    }
  }
  catch (const std::logic_error&) {
  }
  stopCallbackWorker();
  if (callback) {
    callback(StreamedInvocationError{code, message, binding_.requestId,
                                     expectedCursor, binding_.producer});
  }
}

uint64_t
StreamEventConsumer::expectedCursor() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return expectedCursor_;
}

size_t
StreamEventConsumer::reorderSize() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return reorder_.size();
}

uint8_t
StreamEventConsumer::retryCount() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return retryCount_;
}

size_t
StreamEventConsumer::callbackHighWaterMark() const
{
  return callbackQueue_.highWaterMark();
}

} // namespace ndn_service_framework
