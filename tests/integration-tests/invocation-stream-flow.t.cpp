#include "tests/boost-test.hpp"

#include "tests/integration-tests/ndnsf-integration-fixture.hpp"
#include "ndn-service-framework/InvocationStream.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ndn_service_framework::test {

namespace {

struct TestHybridPublication
{
  HybridMessageKey key;
  ndn::Buffer wire;
};

TestHybridPublication
makeTestHybridPublication(const ndn::Name& messageName,
                          const ndn::Name& serviceName,
                          const ndn::Name& requestId,
                          const ndn::Name& senderPrefix,
                          const std::string& messageType,
                          const ndn::Buffer& plaintext)
{
  HybridMessageCrypto crypto;
  HybridCryptoCounters counters;
  const auto accessAttribute = hybridAccessAttributeForName(messageName, serviceName);
  auto key = crypto.getOrCreateSendKey(serviceName, senderPrefix,
                                       accessAttribute, messageType, counters);
  const auto associatedData = hybridAssociatedData(
      messageName, messageType, requestId, serviceName, senderPrefix,
      key.keyId, key.epochId);
  const auto encrypted = hybridAesGcmEncrypt(
      key.key, ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
      ndn::span<const uint8_t>(associatedData.data(), associatedData.size()));
  HybridMessageEnvelope envelope;
  envelope.setKeyId(key.keyId);
  envelope.setEpochId(key.epochId);
  envelope.setMessageType(messageType);
  envelope.setNonce(encrypted.nonce);
  envelope.setCipherText(encrypted.ciphertext);
  envelope.setAuthTag(encrypted.tag);
  const auto block = envelope.WireEncode();
  return {std::move(key), ndn::Buffer(block.data(), block.size())};
}

void
prepareStreamCrypto(NdnsfIntegrationEnvironment& environment,
                    const ndn::Name& serviceName)
{
  const auto ackKey = environment.provider().prepareHybridSendKeyForTest(
      serviceName, "ACK");
  environment.user().cacheHybridReceiveKeyForTest(
      ackKey.keyId, ackKey.epochId, ackKey.key);
  const auto responseKey = environment.provider().prepareHybridSendKeyForTest(
      serviceName, "RESPONSE");
  environment.user().cacheHybridReceiveKeyForTest(
      responseKey.keyId, responseKey.epochId, responseKey.key);
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(
      serviceName, "SELECTION");
  environment.provider().cacheHybridReceiveKeyForTest(
      selectionKey.keyId, selectionKey.epochId, selectionKey.key);
}

void
installEncryptedRequestPublisher(NdnsfIntegrationEnvironment& environment,
                                 const ndn::Name& serviceName,
                                 RequestScope* requestScope = nullptr)
{
  environment.user().setRequestPublisher(
      [&environment, serviceName, requestScope] (const ndn::Name&, const ndn::Name& requestName,
                                                 const std::vector<ndn::Name>& providers,
                                                 const ndn::Name& publishedService,
                                                 const RequestMessage& request, size_t strategy) {
        BOOST_REQUIRE(providers.empty());
        BOOST_CHECK_EQUAL(publishedService, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
        const auto requestId = parseRequestNameV2(requestName)->requestId;
        const auto requestBlock = request.WireEncode();
        const auto encrypted = makeTestHybridPublication(
            requestName, serviceName, requestId, environment.user().getName(),
            "REQUEST", ndn::Buffer(requestBlock.data(), requestBlock.size()));
        environment.provider().cacheHybridReceiveKeyForTest(
            encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
        if (requestScope && !requestScope->requestPublished) {
          environment.markRequestPublished(*requestScope);
        }
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
      });
}

void
installEncryptedTargetedRequestPublisher(NdnsfIntegrationEnvironment& environment,
                                         const ndn::Name& serviceName,
                                         const ndn::Name& providerName)
{
  environment.user().setRequestPublisher(
      [&environment, serviceName, providerName] (const ndn::Name&, const ndn::Name& requestName,
                                                 const std::vector<ndn::Name>& providers,
                                                 const ndn::Name& publishedService,
                                                 const RequestMessage& request, size_t strategy) {
        BOOST_REQUIRE_EQUAL(providers.size(), 1);
        BOOST_CHECK_EQUAL(providers.front(), providerName);
        BOOST_CHECK_EQUAL(publishedService, serviceName);
        BOOST_CHECK_EQUAL(strategy, tlv::FirstResponding);
        BOOST_CHECK_EQUAL(request.getRequestMode(), tlv::TargetedBootstrapRequest);
        const auto requestId = parseRequestNameV2(requestName)->requestId;
        const auto requestBlock = request.WireEncode();
        const auto encrypted = makeTestHybridPublication(
            requestName, serviceName, requestId, environment.user().getName(),
            "REQUEST", ndn::Buffer(requestBlock.data(), requestBlock.size()));
        environment.provider().cacheHybridReceiveKeyForTest(
            encrypted.key.keyId, encrypted.key.epochId, encrypted.key.key);
        environment.userPubSub().publish(
            requestName,
            ndn::span<const uint8_t>(encrypted.wire.data(), encrypted.wire.size()));
      });
}

StreamedInvocationOptions
testStreamOptions()
{
  StreamedInvocationOptions options;
  options.maxEvents = 8;
  options.interestWindow = 4;
  options.reorderCapacity = 4;
  options.publisherQueueCapacity = 4;
  options.retentionMs = 3000;
  options.maxEventWireBytes = 4096;
  return options;
}

std::filesystem::path
findSpec175OneRoleFixture()
{
  const auto relative = std::filesystem::path(
    "tests/fixtures/spec175/tiny-causal-lm-v1/one-role");
  for (const auto& root : {std::filesystem::current_path(),
                           std::filesystem::current_path().parent_path(),
                           std::filesystem::current_path().parent_path().parent_path()}) {
    const auto candidate = root / relative;
    if (std::filesystem::exists(candidate / "role-0.onnx")) {
      return candidate;
    }
  }
  return {};
}

ndnsf::di::TensorBundle
makeStreamZeroState(const std::string& name)
{
  return ndnsf::di::makeEncodedTensorBundle(
    name,
    {ndnsf::di::NamedTensor{name, ndnsf::di::TensorElementType::Float32,
                            {4, 8},
                            std::vector<std::uint8_t>(4 * 8 * sizeof(float), 0)}});
}

ndnsf::di::TensorBundle
makeStreamInputIds(std::int64_t token)
{
  std::vector<std::uint8_t> bytes(sizeof(token));
  std::memcpy(bytes.data(), &token, sizeof(token));
  return ndnsf::di::makeEncodedTensorBundle(
    "input_ids",
    {ndnsf::di::NamedTensor{"input_ids", ndnsf::di::TensorElementType::Int64,
                            {1, 1}, std::move(bytes)}});
}

ndnsf::di::NativeModelRunnerSpec
makeStreamStatefulSpec(const std::filesystem::path& path)
{
  ndnsf::di::NativeModelRunnerSpec spec;
  spec.role = "/LLM/Pipeline/Stage/0";
  spec.kind = "spec175-tiny-stateful-onnx";
  spec.backend = "onnxruntime";
  spec.path = path.string();
  spec.metadata["executionProvider"] = "cpu";
  spec.metadata["statefulModel"] = "true";
  spec.metadata["inputNames"] =
    "input_ids,attention_kv_in,recurrent_state_in,convolution_state_in";
  spec.metadata["outputNames"] =
    "logits,attention_kv_out,recurrent_state_out,convolution_state_out";
  spec.metadata["stateInputNames"] =
    "attention_kv_in,recurrent_state_in,convolution_state_in";
  spec.metadata["stateOutputNames"] =
    "attention_kv_out,recurrent_state_out,convolution_state_out";
  return spec;
}

std::vector<ndnsf::di::NamedTensor>
decodeStreamOutput(const std::map<std::string, ndnsf::di::TensorBundle>& outputs)
{
  const auto found = outputs.find("onnx-output-bundle");
  if (found == outputs.end()) {
    throw std::runtime_error("native stream runner returned no ONNX output bundle");
  }
  return ndnsf::di::decodeTensorBundle(found->second.payload);
}

ndnsf::di::TensorBundle
streamStateOutputAsInput(const std::vector<ndnsf::di::NamedTensor>& tensors,
                         const std::string& outputName)
{
  const auto& output = ndnsf::di::findTensor(tensors, outputName);
  const auto inputName = outputName.substr(0, outputName.size() - 4) + "_in";
  return ndnsf::di::makeEncodedTensorBundle(
    inputName,
    {ndnsf::di::NamedTensor{inputName, output.elementType, output.shape,
                            output.payload}});
}

} // namespace

BOOST_AUTO_TEST_SUITE(Spec175InvocationStream)

BOOST_AUTO_TEST_CASE(NativeTinyOnnxStreamRunsPersistentState)
{
  const auto fixture = findSpec175OneRoleFixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(),
                        "Spec175 one-role ONNX fixture is unavailable");
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/NativeTiny");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  auto runner = std::make_shared<ndnsf::di::OnnxRuntimeModelRunner>(
    makeStreamStatefulSpec(fixture / "role-0.onnx"));
  bool handlerCalled = false;
  // maxEvents includes the terminal End cursor; eight application events
  // therefore require a nine-cursor budget.
  auto streamOptions = testStreamOptions();
  streamOptions.maxEvents = 9;
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
    serviceName,
    [runner, &handlerCalled] (const ndn::Buffer& request,
                              StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
      handlerCalled = request.size() == 5;
      std::map<std::string, ndnsf::di::TensorBundle> state{
        {"attention_kv_in", makeStreamZeroState("attention_kv_in")},
        {"recurrent_state_in", makeStreamZeroState("recurrent_state_in")},
        {"convolution_state_in", makeStreamZeroState("convolution_state_in")},
      };
      std::int64_t token = 3;
      const std::vector<std::int64_t> expected{4, 5, 6, 7, 8, 9, 10, 2};
      for (const auto expectedToken : expected) {
        ndnsf::di::RoleExecutionContext context;
        context.sessionId = "spec175-native-stream";
        context.role = "/LLM/Pipeline/Stage/0";
        context.inputsByScope = state;
        context.inputsByScope.emplace("input_ids", makeStreamInputIds(token));
        const auto outputs = decodeStreamOutput(runner->run(context));
        const auto& logits = ndnsf::di::findTensor(outputs, "logits");
        BOOST_REQUIRE_EQUAL(static_cast<int>(logits.elementType),
                            static_cast<int>(ndnsf::di::TensorElementType::Float32));
        BOOST_REQUIRE_EQUAL(logits.payload.size(), 32U * sizeof(float));
        const auto* values = reinterpret_cast<const float*>(logits.payload.data());
        const auto* best = std::max_element(values, values + 32);
        BOOST_REQUIRE(best != values + 32);
        BOOST_REQUIRE_EQUAL(std::distance(values, best), expectedToken);

        std::string eventText = std::to_string(expectedToken);
        const ndn::Buffer event(reinterpret_cast<const std::uint8_t*>(eventText.data()),
                                eventText.size());
        BOOST_REQUIRE(writer.publish(event));
        state["attention_kv_in"] = streamStateOutputAsInput(
          outputs, "attention_kv_out");
        state["recurrent_state_in"] = streamStateOutputAsInput(
          outputs, "recurrent_state_out");
        state["convolution_state_in"] = streamStateOutputAsInput(
          outputs, "convolution_state_out");
        token = expectedToken;
      }
      const ndn::Buffer result(reinterpret_cast<const std::uint8_t*>("native-onnx"), 11);
      writer.finish(result, StreamFinishReason::Eos);
    });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);

  const auto options = streamOptions;
  const ndn::Buffer request(reinterpret_cast<const std::uint8_t*>("hello"), 5);
  std::vector<std::string> events;
  std::string result;
  std::vector<StreamedInvocationError> errors;
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
    serviceName, request, options,
    [&] (const ndn::Buffer& event) {
      events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
    },
    [&] (const ndn::Buffer& response) {
      result.assign(reinterpret_cast<const char*>(response.data()), response.size());
    },
    [&] (const StreamedInvocationError& error) { errors.push_back(error); });

  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 16 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }
  const std::vector<std::string> expectedEvents{"4", "5", "6", "7", "8", "9", "10", "2"};
  BOOST_CHECK(handlerCalled);
  for (const auto& error : errors) {
    BOOST_TEST_MESSAGE("native tiny stream error code=" <<
                       static_cast<int>(error.code) << " message=" << error.message);
  }
  BOOST_CHECK(errors.empty());
  BOOST_CHECK_EQUAL_COLLECTIONS(events.begin(), events.end(),
                                expectedEvents.begin(), expectedEvents.end());
  BOOST_CHECK_EQUAL(result, "native-onnx");
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Completed);
  BOOST_CHECK_EQUAL(handle->metrics().deliveredEvents, expectedEvents.size());
}

BOOST_AUTO_TEST_CASE(NativeTinyOnnxAdapterRunsIncrementalStream)
{
  const auto fixture = findSpec175OneRoleFixture();
  BOOST_REQUIRE_MESSAGE(!fixture.empty(),
                        "Spec175 one-role ONNX fixture is unavailable");
  auto spec = makeStreamStatefulSpec(fixture / "role-0.onnx");
  spec.metadata["streamingGeneration"] = "true";
  spec.metadata["maxGeneratedTokens"] = "8";
  spec.metadata["eosTokenIds"] = "2";
  spec.metadata["samplingDigest"] =
    "sha256:1750001000000000000000000000000000000000000000000000000000000000";
  ndnsf::di::OnnxRuntimeModelRunner runner(spec);

  ndnsf::di::RoleExecutionContext context;
  context.sessionId = "spec175-native-adapter-stream";
  context.role = "/LLM/Pipeline/Stage/0";
  context.inputsByScope.emplace("input_ids", makeStreamInputIds(3));
  std::vector<std::string> events;
  context.streamEventSink = [&events] (const std::vector<std::uint8_t>& payload) {
    events.emplace_back(payload.begin(), payload.end());
    return true;
  };

  const auto outputs = runner.runStreamed(context);
  BOOST_REQUIRE(outputs.has_value());
  BOOST_REQUIRE(outputs->count("final-response") == 1);
  const auto& finalPayload = outputs->at("final-response").payload;
  const std::string finalText(finalPayload.begin(), finalPayload.end());
  BOOST_CHECK(finalText.find("\"tokenIds\":[4,5,6,7,8,9,10,2]") !=
              std::string::npos);
  BOOST_REQUIRE_EQUAL(events.size(), 8U);
  BOOST_CHECK(events.front().find("\"tokenId\":4") != std::string::npos);
  BOOST_CHECK(events.back().find("\"tokenId\":2") != std::string::npos);
  BOOST_CHECK(events.back().find("\"finishHint\":\"EOS\"") !=
              std::string::npos);
}

BOOST_AUTO_TEST_CASE(NormalServiceOnlyRequestPublishesOrderedEventsAndOneResult)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  bool handlerCalled = false;
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer& request,
           StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        handlerCalled = request.size() == 5;
        const ndn::Buffer first(reinterpret_cast<const uint8_t*>("event-1"), 7);
        const ndn::Buffer second(reinterpret_cast<const uint8_t*>("event-2"), 7);
        const ndn::Buffer third(reinterpret_cast<const uint8_t*>("event-3"), 7);
        const ndn::Buffer result(reinterpret_cast<const uint8_t*>("result"), 6);
        if (!writer.publish(first) || !writer.publish(second) || !writer.publish(third)) {
          writer.fail(StreamedInvocationErrorCode::ProviderFailure,
                      "test publisher rejected an event");
          return;
        }
        writer.finish(result, StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();

  // The LocalMock fixture deliberately bypasses controller/NAC-ABE bootstrap,
  // but still publishes the real encrypted Request through SVS.  This keeps
  // the stream test focused on Core event transport and exact Interests.
  installEncryptedRequestPublisher(environment, serviceName);

  auto options = testStreamOptions();

  const ndn::Buffer request(reinterpret_cast<const uint8_t*>("hello"), 5);
  std::vector<std::string> events;
  std::string result;
  std::vector<StreamedInvocationError> errors;
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, request, options,
      [&] (const ndn::Buffer& event) {
        events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
      },
      [&] (const ndn::Buffer& response) {
        result.assign(reinterpret_cast<const char*>(response.data()), response.size());
      },
      [&] (const StreamedInvocationError& error) {
        errors.push_back(error);
      });

  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 8 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  BOOST_CHECK(handlerCalled);
  BOOST_CHECK(errors.empty());
  const std::vector<std::string> expectedEvents{"event-1", "event-2", "event-3"};
  BOOST_CHECK_EQUAL_COLLECTIONS(
      events.begin(), events.end(),
      expectedEvents.begin(), expectedEvents.end());
  BOOST_CHECK_EQUAL(result, "result");
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Completed);
  BOOST_CHECK_EQUAL(handle->metrics().deliveredEvents, 3);
}

BOOST_AUTO_TEST_CASE(NormalStreamRetriesOneSuppressedEventFromProviderIms)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/Retry");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  bool handlerCalled = false;
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer& request,
           StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        handlerCalled = request.size() == 5;
        for (const char* payload : {"event-1", "event-2", "event-3"}) {
          if (!writer.publish(ndn::Buffer(reinterpret_cast<const uint8_t*>(payload),
                                          std::strlen(payload)))) {
            writer.fail(StreamedInvocationErrorCode::ProviderFailure,
                        "test publisher rejected an event");
            return;
          }
        }
        writer.finish(ndn::Buffer(reinterpret_cast<const uint8_t*>("result"), 6),
                      StreamFinishReason::Eos);
  });
  environment.enableProductionIngressForTest();

  // Drop the first exact Interest for cursor 1, then suppress its SVS
  // publication after retention.  The subsequent bounded retry must recover
  // the already-retained signed Data from the Provider IMS.
  FaultProfile retryFault;
  retryFault.dropStreamInterestCursor = 1;
  retryFault.dropStreamInterestCount = 1;
  auto requestScope = environment.beginRequest("stream-retry", retryFault);
  installEncryptedRequestPublisher(environment, serviceName, &requestScope);
  std::atomic<bool> suppressNext{true};
  std::atomic<size_t> suppressed{0};
  environment.provider().setStreamPublicationInterceptorForTest(
      [&] (const ndn::Data&) {
        if (suppressNext.exchange(false)) {
          ++suppressed;
          return false;
        }
        return true;
      });

  StreamedInvocationOptions options = testStreamOptions();
  options.maxEventRetries = 2;
  options.interestLifetimeMs = 100;
  const ndn::Buffer request(reinterpret_cast<const uint8_t*>("hello"), 5);
  std::vector<std::string> events;
  std::string result;
  std::vector<StreamedInvocationError> errors;
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, request, options,
      [&] (const ndn::Buffer& event) {
        events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
      },
      [&] (const ndn::Buffer& response) {
        result.assign(reinterpret_cast<const char*>(response.data()), response.size());
      },
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 8 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  BOOST_CHECK(handlerCalled);
  BOOST_CHECK(errors.empty());
  const std::vector<std::string> expectedEvents{"event-1", "event-2", "event-3"};
  BOOST_CHECK_EQUAL_COLLECTIONS(
      events.begin(), events.end(),
      expectedEvents.begin(), expectedEvents.end());
  BOOST_CHECK_EQUAL(result, "result");
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Completed);
  BOOST_CHECK_EQUAL(handle->metrics().deliveredEvents, 3);
  BOOST_CHECK_EQUAL(handle->metrics().retryCount, 1);
  BOOST_CHECK_EQUAL(suppressed.load(), 1);

  environment.provider().setStreamPublicationInterceptorForTest({});
  environment.flushReorderedPackets();
  environment.updateRequestResidue(requestScope, {});
  environment.resetRequest(requestScope);
}

BOOST_AUTO_TEST_CASE(NormalStreamPublishesSvsPacketOnProviderFaceEventLoop)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/EventLoopPublication");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  const auto faceThread = std::this_thread::get_id();
  std::atomic<bool> handlerRanOffFaceThread{false};
  std::atomic<bool> publicationObserved{false};
  std::atomic<bool> publicationRanOnFaceThread{false};
  environment.provider().setStreamPublicationInterceptorForTest(
      [&] (const ndn::Data&) {
        publicationObserved = true;
        publicationRanOnFaceThread = std::this_thread::get_id() == faceThread;
        return true;
      });
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer&,
           StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        handlerRanOffFaceThread = std::this_thread::get_id() != faceThread;
        const ndn::Buffer event{
          reinterpret_cast<const uint8_t*>("event"), 5};
        const ndn::Buffer result{
          reinterpret_cast<const uint8_t*>("result"), 6};
        if (!writer.publish(event)) {
          writer.fail(StreamedInvocationErrorCode::ProviderFailure,
                      "test publisher rejected event");
          return;
        }
        writer.finish(result, StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);

  std::vector<std::string> events;
  std::string result;
  std::vector<StreamedInvocationError> errors;
  const ndn::Buffer request{
    reinterpret_cast<const uint8_t*>("hello"), 5};
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, request, testStreamOptions(),
      [&] (const ndn::Buffer& event) {
        events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
      },
      [&] (const ndn::Buffer& response) {
        result.assign(reinterpret_cast<const char*>(response.data()), response.size());
      },
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 8 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  BOOST_REQUIRE(handlerRanOffFaceThread.load());
  BOOST_REQUIRE(publicationObserved.load());
  BOOST_CHECK(publicationRanOnFaceThread.load());
  BOOST_CHECK(errors.empty());
  BOOST_REQUIRE_EQUAL(events.size(), 1U);
  BOOST_CHECK_EQUAL(events.front(), "event");
  BOOST_CHECK_EQUAL(result, "result");
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Completed);
  environment.provider().setStreamPublicationInterceptorForTest({});
}

BOOST_AUTO_TEST_CASE(NormalStreamReordersOneEventAndDrainsInOrder)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/Reorder");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer&, StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        for (const char* payload : {"event-1", "event-2", "event-3", "event-4", "event-5"}) {
          BOOST_REQUIRE(writer.publish(ndn::Buffer(
              reinterpret_cast<const uint8_t*>(payload), std::strlen(payload))));
        }
        writer.finish(ndn::Buffer(reinterpret_cast<const uint8_t*>("result"), 6),
                      StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);
  std::optional<ndn::Data> delayed;
  bool reinjecting = false;
  size_t reordered = 0;
  environment.provider().setStreamPublicationInterceptorForTest(
      [&] (const ndn::Data& data) {
        const auto parsed = parseInvocationEventName(data.getName());
        if (reinjecting || !parsed) return true;
        if (parsed->cursor == 3 && !delayed) {
          delayed = data;
          return false;
        }
        if (delayed) {
          reinjecting = true;
          environment.providerPubSub().publishPacket(data);
          environment.providerPubSub().publishPacket(*delayed);
          reinjecting = false;
          delayed.reset();
          ++reordered;
          return false;
        }
        return true;
      });

  std::vector<std::string> events;
  std::vector<StreamedInvocationError> errors;
  std::string result;
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, ndn::Buffer(reinterpret_cast<const uint8_t*>("hello"), 5),
      testStreamOptions(),
      [&] (const ndn::Buffer& event) {
        events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
      },
      [&] (const ndn::Buffer& response) {
        result.assign(reinterpret_cast<const char*>(response.data()), response.size());
      },
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 12 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  const std::vector<std::string> expected{
    "event-1", "event-2", "event-3", "event-4", "event-5"};
  BOOST_CHECK_EQUAL_COLLECTIONS(events.begin(), events.end(),
                                expected.begin(), expected.end());
  BOOST_CHECK(errors.empty());
  BOOST_CHECK_EQUAL(result, "result");
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Completed);
  BOOST_CHECK_EQUAL(handle->metrics().deliveredEvents, expected.size());
  BOOST_CHECK_EQUAL(reordered, 1);
  environment.provider().setStreamPublicationInterceptorForTest({});
}

BOOST_AUTO_TEST_CASE(NormalStreamSuppressesDuplicateEventData)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/Duplicate");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer&, StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        for (const char* payload : {"event-1", "event-2", "event-3", "event-4", "event-5"}) {
          BOOST_REQUIRE(writer.publish(ndn::Buffer(
              reinterpret_cast<const uint8_t*>(payload), std::strlen(payload))));
        }
        writer.finish(ndn::Buffer(reinterpret_cast<const uint8_t*>("result"), 6),
                      StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);
  bool reinjecting = false;
  size_t duplicated = 0;
  environment.provider().setStreamPublicationInterceptorForTest(
      [&] (const ndn::Data& data) {
        const auto parsed = parseInvocationEventName(data.getName());
        if (reinjecting || !parsed || parsed->cursor != 4) return true;
        reinjecting = true;
        environment.providerPubSub().publishPacket(data);
        environment.providerPubSub().publishPacket(data);
        reinjecting = false;
        ++duplicated;
        return false;
      });

  std::vector<std::string> events;
  std::vector<StreamedInvocationError> errors;
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, ndn::Buffer(reinterpret_cast<const uint8_t*>("hello"), 5),
      testStreamOptions(),
      [&] (const ndn::Buffer& event) {
        events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
      },
      [] (const ndn::Buffer&) {},
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 12 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  const std::vector<std::string> expected{
    "event-1", "event-2", "event-3", "event-4", "event-5"};
  BOOST_CHECK_EQUAL_COLLECTIONS(events.begin(), events.end(),
                                expected.begin(), expected.end());
  BOOST_CHECK(errors.empty());
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Completed);
  BOOST_CHECK_EQUAL(handle->metrics().deliveredEvents, expected.size());
  BOOST_CHECK_EQUAL(duplicated, 1);
  environment.provider().setStreamPublicationInterceptorForTest({});
}

BOOST_AUTO_TEST_CASE(NormalStreamRejectsTamperedEventBeforeDelivery)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/Tamper");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer&, StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        for (const char* payload : {"event-1", "event-2"}) {
          BOOST_REQUIRE(writer.publish(ndn::Buffer(
              reinterpret_cast<const uint8_t*>(payload), std::strlen(payload))));
        }
        writer.finish(ndn::Buffer(reinterpret_cast<const uint8_t*>("result"), 6),
                      StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);
  bool tampered = false;
  bool reinjecting = false;
  // Drop the original packet before IMS insertion and inject only a tampered
  // copy into SVS.  This makes the validator failure deterministic: no valid
  // exact Data can race the intentionally invalid publication.
  environment.provider().setStreamRetentionInterceptorForTest(
      [&] (const ndn::Data& data) {
        const auto parsed = parseInvocationEventName(data.getName());
        if (reinjecting || tampered || !parsed || parsed->cursor != 1) return true;
        tampered = true;
        auto bad = data;
        const auto content = bad.getContent();
        ndn::Buffer altered(content.value(), content.value_size());
        BOOST_REQUIRE(!altered.empty());
        altered[0] ^= 0x01;
        bad.setContent(altered);
        reinjecting = true;
        environment.providerPubSub().publishPacket(bad);
        reinjecting = false;
        return false;
      });

  StreamedInvocationOptions options = testStreamOptions();
  options.maxEventRetries = 0;
  std::vector<std::string> events;
  std::vector<StreamedInvocationError> errors;
  std::string result;
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, ndn::Buffer(reinterpret_cast<const uint8_t*>("hello"), 5),
      options,
      [&] (const ndn::Buffer& event) {
        events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
      },
      [&] (const ndn::Buffer& response) {
        result.assign(reinterpret_cast<const char*>(response.data()), response.size());
      },
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 12 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  BOOST_CHECK(tampered);
  BOOST_CHECK(!errors.empty());
  BOOST_CHECK(events.empty());
  BOOST_CHECK(result.empty());
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Failed);
  environment.provider().setStreamRetentionInterceptorForTest({});
}

BOOST_AUTO_TEST_CASE(NormalStreamContainsCallbackException)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/CallbackException");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer&, StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        for (const char* payload : {"event-1", "event-2", "event-3", "event-4"}) {
          BOOST_REQUIRE(writer.publish(ndn::Buffer(
              reinterpret_cast<const uint8_t*>(payload), std::strlen(payload))));
        }
        writer.finish(ndn::Buffer(reinterpret_cast<const uint8_t*>("result"), 6),
                      StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);

  size_t delivered = 0;
  std::vector<StreamedInvocationError> errors;
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, ndn::Buffer(reinterpret_cast<const uint8_t*>("hello"), 5),
      testStreamOptions(),
      [&] (const ndn::Buffer&) {
        ++delivered;
        if (delivered == 3) throw std::runtime_error("callback failure");
      },
      [] (const ndn::Buffer&) {},
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 12 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  BOOST_CHECK_EQUAL(delivered, 3);
  BOOST_REQUIRE_EQUAL(errors.size(), 1);
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Failed);
}

BOOST_AUTO_TEST_CASE(NormalStreamFailsWhenMissingEventWasNeverRetained)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/PermanentGap");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  bool handlerCalled = false;
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer& request,
           StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        handlerCalled = request.size() == 5;
        for (const char* payload : {"event-1", "event-2"}) {
          if (!writer.publish(ndn::Buffer(reinterpret_cast<const uint8_t*>(payload),
                                          std::strlen(payload)))) {
            writer.fail(StreamedInvocationErrorCode::ProviderFailure,
                        "test publisher rejected an event");
            return;
          }
        }
        writer.finish(ndn::Buffer(reinterpret_cast<const uint8_t*>("result"), 6),
                      StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);

  // Suppress the first event before IMS insertion.  The later event and End
  // expose a real gap, but no exact retransmission can succeed because the
  // missing Data was never retained.
  std::atomic<bool> suppressNext{true};
  std::atomic<size_t> suppressed{0};
  environment.provider().setStreamRetentionInterceptorForTest(
      [&] (const ndn::Data&) {
        if (suppressNext.exchange(false)) {
          ++suppressed;
          return false;
        }
        return true;
      });

  StreamedInvocationOptions options = testStreamOptions();
  options.maxEventRetries = 1;
  options.interestLifetimeMs = 100;
  const ndn::Buffer request(reinterpret_cast<const uint8_t*>("hello"), 5);
  std::vector<std::string> events;
  std::string result;
  std::vector<StreamedInvocationError> errors;
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, request, options,
      [&] (const ndn::Buffer& event) {
        events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
      },
      [&] (const ndn::Buffer& response) {
        result.assign(reinterpret_cast<const char*>(response.data()), response.size());
      },
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 8 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  BOOST_CHECK(handlerCalled);
  BOOST_REQUIRE_EQUAL(errors.size(), 1);
  BOOST_CHECK_EQUAL(errors.front().message, "stream event gap exceeded retry budget");
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Failed);
  BOOST_CHECK(events.empty());
  BOOST_CHECK(result.empty());
  BOOST_CHECK_EQUAL(handle->metrics().retryCount, 1);
  BOOST_CHECK_EQUAL(suppressed.load(), 1);

  environment.provider().setStreamRetentionInterceptorForTest({});
}

BOOST_AUTO_TEST_CASE(StreamedTargetedOverloadRejectsNormalModeAndProviderList)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/Targeted");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();

  StreamedInvocationOptions normal;
  normal.mode = InvocationMode::Normal;
  const ndn::Buffer request(reinterpret_cast<const uint8_t*>("hello"), 5);
  auto wrongMode = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                                ndn::Buffer,
                                                                ndn::Buffer>(
      environment.provider().getName(), profile.serviceName, request, normal,
      [] (const ndn::Buffer&) {}, [] (const ndn::Buffer&) {},
      [] (const StreamedInvocationError&) {});
  BOOST_CHECK(!wrongMode);
}

BOOST_AUTO_TEST_CASE(NormalStreamCompletesWithZeroEvents)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/Zero");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  bool handlerCalled = false;
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer&, StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        handlerCalled = true;
        writer.finish(ndn::Buffer(), StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);

  bool completed = false;
  std::vector<StreamedInvocationError> errors;
  const ndn::Buffer request(reinterpret_cast<const uint8_t*>("hello"), 5);
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, request, testStreamOptions(),
      [] (const ndn::Buffer&) {},
      [&] (const ndn::Buffer& response) {
        completed = response.empty();
      },
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    return handle->status() == StreamedInvocationStatus::Completed ||
           handle->status() == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 8 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }
  BOOST_CHECK(handlerCalled);
  BOOST_CHECK(completed);
  BOOST_CHECK(errors.empty());
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Completed);
  BOOST_CHECK_EQUAL(handle->metrics().deliveredEvents, 0);
}

BOOST_AUTO_TEST_CASE(NormalStreamProviderFailureClosesWithError)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/Failure");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  bool handlerCalled = false;
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer&, StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        handlerCalled = true;
        writer.fail(StreamedInvocationErrorCode::ProviderFailure,
                    "expected integration failure");
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);

  std::vector<StreamedInvocationError> errors;
  const ndn::Buffer request(reinterpret_cast<const uint8_t*>("hello"), 5);
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      serviceName, request, testStreamOptions(),
      [] (const ndn::Buffer&) {},
      [] (const ndn::Buffer&) {},
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    return handle->status() == StreamedInvocationStatus::Completed ||
           handle->status() == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 8 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }
  BOOST_CHECK(handlerCalled);
  BOOST_REQUIRE_EQUAL(errors.size(), 1);
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Failed);
  BOOST_CHECK_EQUAL(static_cast<int>(errors.front().code),
                    static_cast<int>(StreamedInvocationErrorCode::ProviderFailure));
  BOOST_CHECK_EQUAL(errors.front().providerName,
                    environment.provider().getName());
  BOOST_CHECK(!errors.front().requestId.empty());
}

BOOST_AUTO_TEST_CASE(NormalStreamCancellationFencesLaterCallbacks)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/Cancel");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  bool handlerCalled = false;
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer&, StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        handlerCalled = true;
        for (const char* payload : {"event-1", "event-2", "event-3", "event-4", "event-5"}) {
          if (!writer.publish(ndn::Buffer(reinterpret_cast<const uint8_t*>(payload),
                                          std::strlen(payload)))) {
            return;
          }
        }
        writer.finish(ndn::Buffer(
            reinterpret_cast<const uint8_t*>("result"), 6), StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);

  std::vector<std::string> events;
  std::vector<StreamedInvocationError> errors;
  bool completed = false;
  std::shared_ptr<StreamedInvocationHandle<ndn::Buffer, ndn::Buffer>> handle;
  const ndn::Buffer request(reinterpret_cast<const uint8_t*>("hello"), 5);
  handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                       ndn::Buffer,
                                                       ndn::Buffer>(
      serviceName, request, testStreamOptions(),
      [&] (const ndn::Buffer& event) {
        events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
        if (events.size() == 1 && handle) handle->cancel();
      },
      [&] (const ndn::Buffer&) { completed = true; },
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Cancelled ||
           status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 8 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  BOOST_CHECK(handlerCalled);
  BOOST_REQUIRE_EQUAL(events.size(), 1);
  BOOST_CHECK_EQUAL(events.front(), "event-1");
  BOOST_CHECK(!completed);
  BOOST_CHECK(errors.empty());
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Cancelled);
}

BOOST_AUTO_TEST_CASE(NormalStreamCapacityOneSlowConsumerConcurrentCancel)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/CapacityOne");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto serviceName = environment.profile().serviceName;
  prepareStreamCrypto(environment, serviceName);
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer&, StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        for (const char* payload : {"event-1", "event-2", "event-3", "event-4"}) {
          if (!writer.publish(ndn::Buffer(reinterpret_cast<const uint8_t*>(payload),
                                          std::strlen(payload)))) {
            return;
          }
        }
        writer.finish(ndn::Buffer(reinterpret_cast<const uint8_t*>("result"), 6),
                      StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedRequestPublisher(environment, serviceName);

  auto options = testStreamOptions();
  options.publisherQueueCapacity = 1;
  options.callbackQueueCapacity = 1;
  options.interestWindow = 1;
  options.reorderCapacity = 4;

  std::atomic<size_t> delivered{0};
  std::shared_ptr<StreamedInvocationHandle<ndn::Buffer, ndn::Buffer>> handle;
  std::vector<StreamedInvocationError> errors;
  handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                       ndn::Buffer,
                                                       ndn::Buffer>(
      serviceName, ndn::Buffer(reinterpret_cast<const uint8_t*>("hello"), 5),
      options,
      [&] (const ndn::Buffer&) {
        const auto count = ++delivered;
        if (count == 1) {
          // Keep the callback occupied while another thread claims cancel.
          // The consumer must fence every later callback after this race,
          // even though the Provider may already have published its queue.
          std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            handle->cancel();
          });
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          canceller.join();
        }
      },
      [] (const ndn::Buffer&) {},
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Cancelled ||
           status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 12 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  BOOST_CHECK_EQUAL(delivered.load(), 1);
  BOOST_CHECK(errors.empty());
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Cancelled);
}

BOOST_AUTO_TEST_CASE(TargetedStreamBootstrapsAndUsesOneSelection)
{
  BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec175/Stream/TargetedBootstrap");
  NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  const auto serviceName = environment.profile().serviceName;
  const auto providerName = environment.provider().getName();
  prepareStreamCrypto(environment, serviceName);
  bool handlerCalled = false;
  environment.provider().addStreamingHandler<ndn::Buffer, ndn::Buffer, ndn::Buffer>(
      serviceName,
      [&] (const ndn::Buffer& request,
           StreamedResponseWriter<ndn::Buffer, ndn::Buffer>& writer) {
        handlerCalled = request.size() == 5;
        writer.publish(ndn::Buffer(reinterpret_cast<const uint8_t*>("event"), 5));
        writer.finish(ndn::Buffer(reinterpret_cast<const uint8_t*>("result"), 6),
                      StreamFinishReason::Eos);
      });
  environment.enableProductionIngressForTest();
  installEncryptedTargetedRequestPublisher(environment, serviceName, providerName);

  std::vector<std::string> events;
  std::string result;
  std::vector<StreamedInvocationError> errors;
  const ndn::Buffer request(reinterpret_cast<const uint8_t*>("hello"), 5);
  auto options = testStreamOptions();
  options.mode = InvocationMode::Targeted;
  auto handle = environment.user().RequestServiceStreaming<ndn::Buffer,
                                                            ndn::Buffer,
                                                            ndn::Buffer>(
      providerName, serviceName, request, options,
      [&] (const ndn::Buffer& event) {
        events.emplace_back(reinterpret_cast<const char*>(event.data()), event.size());
      },
      [&] (const ndn::Buffer& response) {
        result.assign(reinterpret_cast<const char*>(response.data()), response.size());
      },
      [&] (const StreamedInvocationError& error) { errors.push_back(error); });
  BOOST_REQUIRE(handle);
  const auto terminal = [&] {
    const auto status = handle->status();
    return status == StreamedInvocationStatus::Completed ||
           status == StreamedInvocationStatus::Failed;
  };
  for (int round = 0; round < 12 && !terminal(); ++round) {
    environment.pumpUntil(terminal);
  }

  BOOST_CHECK(handlerCalled);
  BOOST_REQUIRE_EQUAL(events.size(), 1);
  BOOST_CHECK_EQUAL(events.front(), "event");
  BOOST_CHECK_EQUAL(result, "result");
  BOOST_CHECK(errors.empty());
  BOOST_CHECK(handle->status() == StreamedInvocationStatus::Completed);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
