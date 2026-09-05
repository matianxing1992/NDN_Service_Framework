#include "tests/boost-test.hpp"

#include "ndn-service-framework/NDNSFMessages.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace ndn_service_framework::test {
namespace {

template<size_t N>
std::array<uint8_t, N>
bytes(uint8_t first)
{
  std::array<uint8_t, N> value{};
  for (size_t i = 0; i < N; ++i) {
    value[i] = static_cast<uint8_t>(first + i);
  }
  return value;
}

StreamRequestOptions
validOptions()
{
  StreamRequestOptions value;
  value.generationId = bytes<16>(0x10);
  value.streamEpoch = 7;
  value.eventKeyCommitment = bytes<32>(0x20);
  value.deadlineEpochMs = 1234567;
  return value;
}

ConversationContinuationOptions
validConversationContinuation()
{
  ConversationContinuationOptions value;
  value.conversationId = bytes<16>(0x70);
  value.mode = ConversationInputMode::AppendDelta;
  value.parentContextEpoch = 3;
  value.parentCheckpoint = ndn::Buffer{0x01, 0x02, 0x03};
  value.turnInputDigest = bytes<32>(0x80);
  value.allowFullPrefillFallback = true;
  value.fallbackInputDigest = bytes<32>(0xA0);
  return value;
}

StreamBinding
validBinding()
{
  StreamBinding value;
  value.requestId = ndn::Name("/request/id");
  value.requester = ndn::Name("/requester/alice");
  value.serviceName = ndn::Name("/LLM/Qwen");
  value.producer = ndn::Name("/provider/final");
  value.producerBootId = "boot-7";
  value.attemptEpoch = 1;
  value.planDigest = bytes<32>(0x30);
  value.generationId = bytes<16>(0x10);
  value.streamEpoch = 7;
  value.eventKeyCommitment = bytes<32>(0x20);
  value.userToken = ndn::Buffer{0x55, 0x56, 0x57};
  value.policyEpoch = 3;
  value.deadlineEpochMs = 123456;
  return value;
}

ndn::Block
withUnknownField(const ndn::Block& wire)
{
  auto parsed = wire;
  parsed.parse();
  ndn::Block mutated(wire.type());
  for (const auto& element : parsed.elements()) {
    mutated.push_back(element);
  }
  mutated.push_back(ndn::makeNonNegativeIntegerBlock(0xF6FF, 1));
  mutated.encode();
  return mutated;
}

ndn::Block
withoutField(const ndn::Block& wire, size_t omittedIndex)
{
  auto parsed = wire;
  parsed.parse();
  ndn::Block mutated(wire.type());
  for (size_t i = 0; i < parsed.elements().size(); ++i) {
    if (i != omittedIndex) {
      mutated.push_back(parsed.elements()[i]);
    }
  }
  mutated.encode();
  return mutated;
}

ndn::Block
withDuplicateField(const ndn::Block& wire, size_t duplicateIndex)
{
  auto parsed = wire;
  parsed.parse();
  ndn::Block mutated(wire.type());
  for (const auto& element : parsed.elements()) {
    mutated.push_back(element);
  }
  mutated.push_back(parsed.elements().at(duplicateIndex));
  mutated.encode();
  return mutated;
}

ndn::Block
withSwappedFields(const ndn::Block& wire, size_t first, size_t second)
{
  auto parsed = wire;
  parsed.parse();
  std::vector<ndn::Block> elements(parsed.elements().begin(), parsed.elements().end());
  std::swap(elements.at(first), elements.at(second));
  ndn::Block mutated(wire.type());
  for (const auto& element : elements) {
    mutated.push_back(element);
  }
  mutated.encode();
  return mutated;
}

ndn::Block
withReplacementField(const ndn::Block& wire, size_t replacedIndex,
                     const ndn::Block& replacement)
{
  auto parsed = wire;
  parsed.parse();
  ndn::Block mutated(wire.type());
  for (size_t i = 0; i < parsed.elements().size(); ++i) {
    mutated.push_back(i == replacedIndex ? replacement : parsed.elements()[i]);
  }
  mutated.encode();
  return mutated;
}

std::string
toHex(const ndn::Block& wire)
{
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : wire) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

template<size_t N>
std::string
toHex(const std::array<uint8_t, N>& value)
{
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : value) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

StreamCompletion
validCompletion()
{
  const auto binding = validBinding();
  StreamCompletion completion;
  completion.bindingDigest = computeStreamBindingDigest(binding);
  completion.endEventName = makeInvocationEventName(binding, 9);
  completion.finalCursor = 9;
  completion.finishReason = StreamFinishReason::ApplicationComplete;
  completion.applicationEventCount = 8;
  completion.transcriptDigest = bytes<32>(0x40);
  completion.finalResultDigest = bytes<32>(0x60);
  return completion;
}

HybridMessageEnvelope
validEventKeyGrant()
{
  HybridMessageEnvelope envelope;
  envelope.setVersion(2);
  envelope.setAlgorithm("AES-256-GCM");
  envelope.setKeyId("0011223344556677");
  envelope.setEpochId("8899aabbccddeeff");
  envelope.setMessageType("REQUEST");
  envelope.setNonce(ndn::Buffer(12, 0x11));
  envelope.setCipherText(ndn::Buffer{0x22, 0x23, 0x24});
  envelope.setAuthTag(ndn::Buffer(16, 0x33));
  envelope.setWrappedMessageKey(ndn::Buffer{0x44, 0x45, 0x46});
  return envelope;
}

HybridMessageEnvelope
validRecipientWrappedEventKeyGrant()
{
  HybridMessageEnvelope envelope;
  envelope.setVersion(2);
  envelope.setAlgorithm("RSA-OAEP");
  envelope.setKeyId("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  envelope.setEpochId("7");
  envelope.setMessageType("STREAM-GRANT");
  envelope.setWrappedMessageKey(ndn::Buffer(256, 0x44));
  return envelope;
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec175InvocationStreamMessage)

BOOST_AUTO_TEST_CASE(StreamTlvRangeIsFrozenAndContiguous)
{
  BOOST_CHECK_EQUAL(tlv::StreamRequestOptionsType, 0xF661);
  BOOST_CHECK_EQUAL(tlv::StreamAttemptEpochType, 0xF684);
  BOOST_CHECK_EQUAL(
    tlv::StreamAttemptEpochType - tlv::StreamRequestOptionsType + 1, 36);
}

BOOST_AUTO_TEST_CASE(StreamRequestOptionsRoundTripIsCanonicalAndBounded)
{
  const auto value = validOptions();
  const auto wire = value.wireEncode();
  StreamRequestOptions decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded == value);
  BOOST_CHECK(std::equal(wire.begin(), wire.end(), decoded.wireEncode().begin()));

  BOOST_CHECK(!decoded.wireDecode(withUnknownField(wire)));
  BOOST_CHECK(!decoded.wireDecode(withoutField(wire, 3)));
  BOOST_CHECK(!decoded.wireDecode(withDuplicateField(wire, 3)));
  BOOST_CHECK(!decoded.wireDecode(withSwappedFields(wire, 3, 4)));

  const std::array<uint8_t, 2> nonCanonicalThree{{0, 3}};
  BOOST_CHECK(!decoded.wireDecode(withReplacementField(
    wire, 10, ndn::makeBinaryBlock(tlv::StreamMaxEventRetriesType,
      nonCanonicalThree.begin(), nonCanonicalThree.end()))));
  const std::array<uint8_t, 2> overflowingByte{{1, 0}};
  BOOST_CHECK(!decoded.wireDecode(withReplacementField(
    wire, 10, ndn::makeBinaryBlock(tlv::StreamMaxEventRetriesType,
      overflowingByte.begin(), overflowingByte.end()))));

  BOOST_CHECK_EQUAL(toHex(wire),
    "fdf66193a30101fdf6810100fdf66410101112131415161718191a1b1c1d1e1f"
    "fdf6840101fdf6650107fdf68220202122232425262728292a2b2c2d2e2f3031323334353637"
    "38393a3b3c3d3e3ffdf683040012d687fdf667020200fdf6680110fdf6690201f4"
    "fdf66a0103fdf66b"
    "0140fdf66c0140fdf66d0140fdf66e027530fdf66f021388fdf670024000fdf671"
    "0100fdf6720100");
  auto invalid = value;
  invalid.maxEvents = 0;
  BOOST_CHECK_THROW(invalid.wireEncode(), std::invalid_argument);
  invalid = value;
  invalid.allowReplacement = true;
  BOOST_CHECK_THROW(invalid.wireEncode(), std::invalid_argument);
  invalid = value;
  invalid.attemptEpoch = 0;
  BOOST_CHECK_THROW(invalid.wireEncode(), std::invalid_argument);
  invalid = value;
  invalid.attemptEpoch = 3;
  BOOST_CHECK_THROW(invalid.wireEncode(), std::invalid_argument);
  invalid = value;
  invalid.mode = InvocationMode::Targeted;
  invalid.allowReplacement = true;
  invalid.maxReplacements = 1;
  BOOST_CHECK_THROW(invalid.wireEncode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ConversationContinuationRoundTripIsCanonicalAndBounded)
{
  const auto value = validConversationContinuation();
  const auto wire = value.wireEncode();
  ConversationContinuationOptions decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded == value);
  BOOST_CHECK(std::equal(wire.begin(), wire.end(), decoded.wireEncode().begin()));

  auto parsed = wire;
  parsed.parse();
  BOOST_CHECK(!decoded.wireDecode(withUnknownField(wire)));
  BOOST_CHECK(!decoded.wireDecode(withoutField(wire, 3)));
  BOOST_CHECK(!decoded.wireDecode(withDuplicateField(wire, 3)));
  BOOST_CHECK(!decoded.wireDecode(withSwappedFields(wire, 1, 2)));

  ConversationContinuationOptions full = value;
  full.mode = ConversationInputMode::FullContext;
  full.parentContextEpoch.reset();
  full.parentCheckpoint.reset();
  full.allowFullPrefillFallback = false;
  full.fallbackInputDigest.reset();
  BOOST_REQUIRE(full.wireDecode(full.wireEncode()));
  BOOST_CHECK_THROW(
    ([]{ ConversationContinuationOptions invalid;
         invalid.conversationId = bytes<16>(0x01);
         invalid.turnInputDigest = bytes<32>(0x02);
         invalid.mode = ConversationInputMode::AppendDelta;
         invalid.validate(); })(),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RequestMessageCarriesOptionalConversationContinuation)
{
  RequestMessage request;
  request.setConversationContinuation(validConversationContinuation());
  BOOST_CHECK(request.hasConversationContinuation());
  const auto wire = request.WireEncode();
  RequestMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(wire));
  BOOST_CHECK(decoded.hasConversationContinuation());
  BOOST_CHECK(decoded.getConversationContinuation() ==
              request.getConversationContinuation());

  RequestMessage old;
  BOOST_CHECK(!old.hasConversationContinuation());
  RequestMessage oldDecoded;
  BOOST_REQUIRE(oldDecoded.WireDecode(old.WireEncode()));
  BOOST_CHECK(!oldDecoded.hasConversationContinuation());
}

BOOST_AUTO_TEST_CASE(StreamRequestOptionsAttemptTwoRoundTripsWithoutDowngrade)
{
  auto value = validOptions();
  value.attemptEpoch = 2;
  value.streamEpoch = 8;
  value.allowReplacement = false;
  value.maxReplacements = 0;

  StreamRequestOptions decoded;
  const auto wire = value.wireEncode();
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded == value);
  BOOST_CHECK_EQUAL(decoded.attemptEpoch, 2U);
  BOOST_CHECK_EQUAL(decoded.streamEpoch, 8U);
}

BOOST_AUTO_TEST_CASE(StreamRequestOptionsControllerVersionRoundTripsAndIsOptional)
{
  auto value = validOptions();
  value.controllerVersion = ControllerVersion{1788285600123ULL, 7};

  const auto wire = value.wireEncode();
  StreamRequestOptions decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded == value);
  BOOST_REQUIRE(decoded.controllerVersion.has_value());
  BOOST_CHECK_EQUAL(decoded.controllerVersion->controllerGenerationTimestamp,
                    1788285600123ULL);
  BOOST_CHECK_EQUAL(decoded.controllerVersion->controllerEpoch, 7ULL);

  auto parsed = wire;
  parsed.parse();
  BOOST_REQUIRE_EQUAL(parsed.elements().size(), 20);
  const auto versionIndex = size_t{7};
  BOOST_CHECK_EQUAL(parsed.elements()[versionIndex].type(),
                    tlv::StreamControllerVersionType);
  auto versionWrapper = parsed.elements()[versionIndex];
  versionWrapper.parse();
  BOOST_REQUIRE_EQUAL(versionWrapper.elements().size(), 1);
  BOOST_CHECK_EQUAL(versionWrapper.elements().front().type(), ControllerVersion::TYPE);

  auto changed = value;
  changed.controllerVersion->controllerEpoch++;
  BOOST_CHECK(toHex(changed.wireEncode()) != toHex(wire));

  auto legacy = validOptions();
  StreamRequestOptions legacyDecoded;
  BOOST_REQUIRE(legacyDecoded.wireDecode(legacy.wireEncode()));
  BOOST_CHECK(!legacyDecoded.controllerVersion.has_value());

  auto invalid = value;
  invalid.controllerVersion = ControllerVersion{0, 1};
  BOOST_CHECK_THROW(invalid.wireEncode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StreamBindingControllerVersionChangesCanonicalDigest)
{
  auto binding = validBinding();
  binding.controllerVersion = ControllerVersion{1788285600123ULL, 7};
  const auto currentDigest = computeStreamBindingDigest(binding);

  auto changed = binding;
  changed.controllerVersion->controllerEpoch++;
  const auto changedDigest = computeStreamBindingDigest(changed);
  BOOST_CHECK(currentDigest != changedDigest);

  auto legacy = binding;
  legacy.controllerVersion.reset();
  const auto legacyDigest = computeStreamBindingDigest(legacy);
  BOOST_CHECK(currentDigest != legacyDigest);

  auto invalid = binding;
  invalid.controllerVersion = ControllerVersion{0, 1};
  BOOST_CHECK_THROW(computeStreamBindingDigest(invalid), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StreamEventKeyGrantEnvelopeCarriesOnlyWrappedKey)
{
  const auto grant = validRecipientWrappedEventKeyGrant();
  const auto wire = grant.WireEncode();

  HybridMessageEnvelope decoded;
  BOOST_REQUIRE(decoded.WireDecode(wire));
  BOOST_CHECK_EQUAL(decoded.getMessageType(), "STREAM-GRANT");
  BOOST_CHECK_EQUAL(decoded.getAlgorithm(), "RSA-OAEP");
  BOOST_CHECK(decoded.getNonce().empty());
  BOOST_CHECK(decoded.getCipherText().empty());
  BOOST_CHECK(decoded.getAuthTag().empty());
  BOOST_CHECK_EQUAL(decoded.getWrappedMessageKey().size(), 256);
  BOOST_CHECK(std::equal(wire.begin(), wire.end(), decoded.WireEncode().begin()));

  auto malformed = wire;
  malformed.parse();
  ndn::Block malformedBlock(malformed.type());
  for (const auto& element : malformed.elements()) {
    malformedBlock.push_back(element);
  }
  const ndn::Buffer unexpectedNonce{0x01};
  malformedBlock.push_back(ndn::makeBinaryBlock(
    tlv::NonceType, unexpectedNonce.begin(), unexpectedNonce.end()));
  malformedBlock.encode();
  BOOST_CHECK(!decoded.WireDecode(malformedBlock));
}

BOOST_AUTO_TEST_CASE(RequestMessageCarriesOneFinalStreamOptionsBlock)
{
  RequestMessage request;
  request.setUserToken("user-token");
  ndn::Buffer payload{0x01, 0x02};
  request.setPayload(payload, payload.size());
  request.setStreamRequestOptions(validOptions());

  const auto wire = request.WireEncode();
  RequestMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(wire));
  BOOST_REQUIRE(decoded.hasStreamRequestOptions());
  BOOST_CHECK(decoded.getStreamRequestOptions() == validOptions());

  const RequestMessage copied(decoded);
  BOOST_REQUIRE(copied.hasStreamRequestOptions());
  BOOST_CHECK(copied.getStreamRequestOptions() == validOptions());

  auto parsed = wire;
  parsed.parse();
  const size_t streamIndex = parsed.elements().size() - 1;
  BOOST_CHECK_EQUAL(parsed.elements()[streamIndex].type(), tlv::StreamRequestOptionsType);
  BOOST_CHECK(!decoded.WireDecode(withDuplicateField(wire, streamIndex)));
  BOOST_CHECK(!decoded.WireDecode(withSwappedFields(wire, 0, streamIndex)));

  decoded.clearStreamRequestOptions();
  BOOST_CHECK(!decoded.hasStreamRequestOptions());
  BOOST_CHECK_THROW(decoded.getStreamRequestOptions(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(TargetedEventKeyGrantIsNestedAndNormalGrantIsRejected)
{
  auto options = validOptions();
  options.mode = InvocationMode::Targeted;
  options.eventKeyGrant = validEventKeyGrant().WireEncode();
  const auto wire = options.wireEncode();
  StreamRequestOptions decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_REQUIRE(decoded.eventKeyGrant.has_value());
  BOOST_CHECK_EQUAL(decoded.eventKeyGrant->type(), tlv::HybridMessageEnvelopeType);
  BOOST_CHECK(decoded == options);

  options.mode = InvocationMode::Normal;
  BOOST_CHECK_THROW(options.wireEncode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ApplicationAndEndEventsRoundTripWithExactDigests)
{
  const auto bindingDigest = computeStreamBindingDigest(validBinding());
  InvocationEventMessage application;
  application.bindingDigest = bindingDigest;
  application.cursor = 1;
  application.eventType = InvocationEventType::Application;
  application.publishedAtUs = 100;
  application.userToken = ndn::Buffer{0x55, 0x56, 0x57};
  application.setPayload(ndn::Buffer{0x01, 0x02, 0x03});
  const auto applicationWire = application.wireEncode();
  InvocationEventMessage decodedApplication;
  BOOST_REQUIRE(decodedApplication.wireDecode(applicationWire));
  BOOST_CHECK(decodedApplication == application);
  BOOST_CHECK_EQUAL(toHex(applicationWire),
    "fdf66269a30101fdf67320aa8c0e2fc57574d2ad7e6b70c25ba8c6b5f563bf7b7"
    "c0f87620a02654f3e438dfdf6740101fdf67501019703010203fdf6762003"
    "9058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81fdf"
    "6770164aa03555657fdf6780100");
  BOOST_CHECK(!decodedApplication.wireDecode(withoutField(applicationWire, 5)));
  BOOST_CHECK(!decodedApplication.wireDecode(withDuplicateField(applicationWire, 5)));
  BOOST_CHECK(!decodedApplication.wireDecode(withSwappedFields(applicationWire, 5, 6)));

  const std::array<uint8_t, 31> shortDigest{};
  BOOST_CHECK(!decodedApplication.wireDecode(withReplacementField(
    applicationWire, 5, ndn::makeBinaryBlock(tlv::StreamPayloadDigestType,
      shortDigest.begin(), shortDigest.end()))));

  StreamEndEvent end;
  end.finalCursor = 2;
  end.finishReason = StreamFinishReason::ApplicationComplete;
  end.applicationEventCount = 1;
  end.generatedTokenCount = 0;
  end.transcriptDigest = advanceStreamTranscriptDigest(
    initialStreamTranscriptDigest(), application.cursor,
    application.eventType, application.payloadDigest);
  end.finalResultDigest = computeStreamSha256(
    ndn::span<const uint8_t>(application.payload.data(), application.payload.size()));

  InvocationEventMessage terminal;
  terminal.bindingDigest = bindingDigest;
  terminal.cursor = 2;
  terminal.eventType = InvocationEventType::End;
  terminal.publishedAtUs = 110;
  terminal.userToken = application.userToken;
  terminal.terminal = true;
  terminal.end = end;
  terminal.setPayload({});
  const auto terminalWire = terminal.wireEncode();
  InvocationEventMessage decodedTerminal;
  BOOST_REQUIRE(decodedTerminal.wireDecode(terminalWire));
  BOOST_CHECK(decodedTerminal == terminal);
  BOOST_CHECK(!decodedTerminal.wireDecode(withUnknownField(terminalWire)));
  BOOST_CHECK_EQUAL(toHex(terminalWire),
    "fdf662c8a30101fdf67320aa8c0e2fc57574d2ad7e6b70c25ba8c6b5f563bf7b7"
    "c0f87620a02654f3e438dfdf6740102fdf67501029700fdf67620e3b0c442"
    "98fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855fdf67701"
    "6eaa03555657fdf6780101fdf6795efdf67a0102fdf67b0104fdf67c0101fdf67d"
    "0100fdf67e205a3df5f946970f645c78bc66242e7fa569e6cafa1f4a0db3bdb3d9"
    "bed6650406fdf67f20039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced528"
    "7d84a1a2011cfb819900");
}

BOOST_AUTO_TEST_CASE(StreamCompletionAndEventNameBindOneInvocation)
{
  const auto binding = validBinding();
  BOOST_CHECK_EQUAL(toHex(computeStreamBindingDigest(binding)),
    "aa8c0e2fc57574d2ad7e6b70c25ba8c6b5f563bf7b7c0f87620a02654f3e438d");
  const auto eventName = makeInvocationEventName(binding, 9);
  const auto parsed = parseInvocationEventName(eventName);
  BOOST_REQUIRE(parsed.has_value());
  BOOST_CHECK_EQUAL(parsed->producer, binding.producer);
  BOOST_CHECK_EQUAL(parsed->requester, binding.requester);
  BOOST_CHECK_EQUAL(parsed->serviceName, binding.serviceName);
  BOOST_CHECK_EQUAL(parsed->requestId, ndn::Name("/id"));
  BOOST_CHECK_EQUAL(parsed->cursor, 9);
  BOOST_CHECK_EQUAL(parsed->streamEpoch, binding.streamEpoch);
  BOOST_CHECK(parsed->planDigest == binding.planDigest);
  BOOST_CHECK(parsed->generationId == binding.generationId);
  BOOST_CHECK(!parseInvocationEventName(ndn::Name(eventName).append("extra")));

  StreamCompletion completion = validCompletion();
  const auto wire = completion.wireEncode();
  StreamCompletion decoded;
  BOOST_REQUIRE(decoded.wireDecode(wire));
  BOOST_CHECK(decoded == completion);
  BOOST_CHECK(!decoded.wireDecode(withUnknownField(wire)));
  BOOST_CHECK(!decoded.wireDecode(withoutField(wire, 3)));
  BOOST_CHECK(!decoded.wireDecode(withDuplicateField(wire, 3)));
  BOOST_CHECK(!decoded.wireDecode(withSwappedFields(wire, 3, 4)));
  BOOST_CHECK_EQUAL(toHex(wire),
    "fdf663fd0131a30101fdf67320aa8c0e2fc57574d2ad7e6b70c25ba8c6b5f563bf7b7"
    "c0f87620a02654f3e438dfdf680af07ad080870726f7669646572080566696e"
    "616c08054e444e534608054556454e5408102f7265717565737465722f616c6963"
    "6508034c4c4d08045177656e080269640801010840333033313332333333343335"
    "333633373338333933613362336333643365336634303431343234333434343534"
    "363437343834393461346234633464346534660820313031313132313331343135"
    "3136313731383139316131623163316431653166080107080109fdf67a0109fdf6"
    "7b0104fdf67c0108fdf67e20404142434445464748494a4b4c4d4e4f5051525354"
    "55565758595a5b5c5d5e5ffdf67f20606162636465666768696a6b6c6d6e6f70"
    "7172737475767778797a7b7c7d7e7f");
}

BOOST_AUTO_TEST_CASE(ResponseMessageCarriesOneFinalStreamCompletionBlock)
{
  ResponseMessage response;
  response.setStatus(true);
  response.setUserToken("user-token");
  ndn::Buffer payload{0xAA, 0xBB};
  response.setPayload(payload, payload.size());
  response.setStreamCompletion(validCompletion());

  const auto wire = response.WireEncode();
  ResponseMessage decoded;
  BOOST_REQUIRE(decoded.WireDecode(wire));
  BOOST_REQUIRE(decoded.hasStreamCompletion());
  BOOST_CHECK(decoded.getStreamCompletion() == validCompletion());

  const ResponseMessage copied(decoded);
  BOOST_REQUIRE(copied.hasStreamCompletion());
  BOOST_CHECK(copied.getStreamCompletion() == validCompletion());

  auto parsed = wire;
  parsed.parse();
  const size_t completionIndex = parsed.elements().size() - 1;
  BOOST_CHECK_EQUAL(parsed.elements()[completionIndex].type(), tlv::StreamCompletionType);
  BOOST_CHECK(!decoded.WireDecode(withDuplicateField(wire, completionIndex)));
  BOOST_CHECK(!decoded.WireDecode(withSwappedFields(wire, 0, completionIndex)));

  decoded.clearStreamCompletion();
  BOOST_CHECK(!decoded.hasStreamCompletion());
  BOOST_CHECK_THROW(decoded.getStreamCompletion(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(CompletionMustMatchEndNameBindingTranscriptAndResponse)
{
  const auto binding = validBinding();
  const auto bindingDigest = computeStreamBindingDigest(binding);
  ndn::Buffer finalPayload{0xAA, 0xBB};

  InvocationEventMessage endEvent;
  endEvent.bindingDigest = bindingDigest;
  endEvent.cursor = 9;
  endEvent.eventType = InvocationEventType::End;
  endEvent.publishedAtUs = 100;
  endEvent.userToken = binding.userToken;
  endEvent.terminal = true;
  endEvent.setPayload({});

  StreamEndEvent end;
  end.finalCursor = 9;
  end.finishReason = StreamFinishReason::ApplicationComplete;
  end.applicationEventCount = 8;
  end.transcriptDigest = bytes<32>(0x40);
  end.finalResultDigest = computeStreamSha256(
    ndn::span<const uint8_t>(finalPayload.data(), finalPayload.size()));
  endEvent.end = end;

  auto completion = validCompletion();
  completion.bindingDigest = bindingDigest;
  completion.transcriptDigest = end.transcriptDigest;
  completion.finalResultDigest = end.finalResultDigest;
  const auto endName = makeInvocationEventName(binding, 9);
  completion.endEventName = endName;
  BOOST_REQUIRE(isMatchingStreamCompletion(
    completion, endName, endEvent,
    ndn::span<const uint8_t>(finalPayload.data(), finalPayload.size())));

  auto mismatched = completion;
  mismatched.transcriptDigest[0] ^= 0xFF;
  BOOST_CHECK(!isMatchingStreamCompletion(
    mismatched, endName, endEvent,
    ndn::span<const uint8_t>(finalPayload.data(), finalPayload.size())));
  BOOST_CHECK(!isMatchingStreamCompletion(
    completion, ndn::Name(endName).append("wrong"), endEvent,
    ndn::span<const uint8_t>(finalPayload.data(), finalPayload.size())));
  const ndn::Buffer wrongPayload{0x00};
  BOOST_CHECK(!isMatchingStreamCompletion(
    completion, endName, endEvent,
    ndn::span<const uint8_t>(wrongPayload.data(), wrongPayload.size())));
}

BOOST_AUTO_TEST_CASE(OversizeAndWrongTerminalShapeAreRejected)
{
  InvocationEventMessage oversize;
  oversize.bindingDigest = computeStreamBindingDigest(validBinding());
  oversize.cursor = 1;
  oversize.eventType = InvocationEventType::Application;
  oversize.publishedAtUs = 100;
  oversize.userToken = ndn::Buffer{0x01};
  oversize.setPayload(ndn::Buffer(65536, 0xAA));
  const auto wire = oversize.wireEncode();
  BOOST_REQUIRE_GT(wire.size(), 65536);
  InvocationEventMessage decoded;
  BOOST_CHECK(!decoded.wireDecode(wire));

  StreamEndEvent wrongEnd;
  wrongEnd.finalCursor = 3;
  wrongEnd.finishReason = StreamFinishReason::ApplicationComplete;
  wrongEnd.applicationEventCount = 1;
  wrongEnd.transcriptDigest = bytes<32>(0x40);
  wrongEnd.finalResultDigest = bytes<32>(0x60);
  BOOST_CHECK_THROW(wrongEnd.wireEncode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NonceAndAssociatedDataAreDeterministicAndCursorBound)
{
  const auto key = bytes<32>(0x80);
  const auto binding = validBinding();
  const auto digest = computeStreamBindingDigest(binding);
  const auto name = makeInvocationEventName(binding, 1);
  const auto nonce1 = deriveInvocationEventNonce(key, digest, 1);
  const auto nonce1Again = deriveInvocationEventNonce(key, digest, 1);
  const auto nonce2 = deriveInvocationEventNonce(key, digest, 2);
  BOOST_CHECK(nonce1 == nonce1Again);
  BOOST_CHECK(nonce1 != nonce2);
  BOOST_CHECK_EQUAL(toHex(nonce1), "48e858c66cb61b9307d64557");

  const auto aad1 = makeInvocationEventAssociatedData(name, digest, 1);
  const auto aad2 = makeInvocationEventAssociatedData(name, digest, 2);
  BOOST_CHECK(aad1 != aad2);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
