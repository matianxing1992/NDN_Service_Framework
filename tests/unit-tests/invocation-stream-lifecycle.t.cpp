#include "tests/boost-test.hpp"

#include "ndn-service-framework/InvocationStream.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"

#include <chrono>
#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(Spec175InvocationStreamLifecycle)

BOOST_AUTO_TEST_CASE(UserLifecycleAcceptsOnlyTheRegisteredTransitions)
{
  StreamUserLifecycle lifecycle;
  BOOST_CHECK(lifecycle.state() == StreamUserLifecycleState::Created);
  BOOST_CHECK_THROW(lifecycle.beginSelection(), std::logic_error);

  lifecycle.beginRequest();
  BOOST_CHECK(lifecycle.state() == StreamUserLifecycleState::Requesting);
  lifecycle.beginSelection();
  lifecycle.beginStreaming();
  lifecycle.beginDraining();
  lifecycle.complete();
  BOOST_CHECK(lifecycle.state() == StreamUserLifecycleState::Completed);
  BOOST_CHECK(lifecycle.isTerminal());
  BOOST_CHECK_NO_THROW(lifecycle.cancel());
  BOOST_CHECK_THROW(lifecycle.fail(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(ProviderLifecycleHasOneTerminalClaim)
{
  StreamProviderLifecycle lifecycle;
  BOOST_CHECK_THROW(lifecycle.finish(), std::logic_error);
  lifecycle.activate();
  lifecycle.beginEnding();
  lifecycle.finish();
  BOOST_CHECK(lifecycle.state() == StreamProviderLifecycleState::Finished);
  BOOST_CHECK(lifecycle.terminalAuthority()->state() == StreamTerminalState::Completed);
  BOOST_CHECK_THROW(lifecycle.finish(), std::logic_error);
  BOOST_CHECK_NO_THROW(lifecycle.cancel());
}

BOOST_AUTO_TEST_CASE(UserAndProviderShareTerminalAuthority)
{
  auto authority = std::make_shared<StreamTerminalAuthority>();
  StreamUserLifecycle user(authority);
  StreamProviderLifecycle provider(authority);

  user.beginRequest();
  user.beginSelection();
  user.beginStreaming();
  user.beginDraining();
  provider.activate();
  provider.beginEnding();
  provider.finish();

  BOOST_CHECK(user.terminalAuthority() == provider.terminalAuthority());
  BOOST_CHECK(user.terminalAuthority()->state() == StreamTerminalState::Completed);
  BOOST_CHECK_NO_THROW(user.complete());
  BOOST_CHECK(user.state() == StreamUserLifecycleState::Completed);
  BOOST_CHECK_THROW(provider.finish(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(CancelIsIdempotentAndFencesBothViews)
{
  auto authority = std::make_shared<StreamTerminalAuthority>();
  StreamUserLifecycle user(authority);
  StreamProviderLifecycle provider(authority);
  user.beginRequest();
  provider.activate();

  user.cancel();
  BOOST_CHECK(user.state() == StreamUserLifecycleState::Cancelled);
  BOOST_CHECK(provider.terminalAuthority()->state() == StreamTerminalState::Cancelled);
  BOOST_CHECK_NO_THROW(user.cancel());
  BOOST_CHECK_NO_THROW(provider.cancel());
  BOOST_CHECK_THROW(provider.beginEnding(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(ConcurrentCancelAndFailureHaveOneTerminalOutcome)
{
  StreamUserLifecycle lifecycle;
  lifecycle.beginRequest();
  std::atomic<bool> failureThrew{false};
  std::thread failure([&] {
    try {
      lifecycle.fail();
    }
    catch (const std::logic_error&) {
      failureThrew = true;
    }
  });
  std::thread cancellation([&] {
    lifecycle.cancel();
  });
  failure.join();
  cancellation.join();

  const auto state = lifecycle.state();
  BOOST_CHECK(state == StreamUserLifecycleState::Failed ||
              state == StreamUserLifecycleState::Cancelled);
  BOOST_CHECK(lifecycle.terminalAuthority()->isTerminal());
  BOOST_CHECK_NO_THROW(lifecycle.cancel());
  BOOST_CHECK(failureThrew.load() || state == StreamUserLifecycleState::Failed);
}

BOOST_AUTO_TEST_CASE(CallbackExceptionFailsBeforeAnyLaterDelivery)
{
  StreamUserLifecycle lifecycle;
  lifecycle.beginRequest();
  lifecycle.beginSelection();
  lifecycle.beginStreaming();
  bool called = false;
  BOOST_CHECK(lifecycle.dispatchCallback([&] (int value) {
    called = value == 7;
  }, 7));
  BOOST_CHECK(called);

  BOOST_CHECK(!lifecycle.dispatchCallback([] (int) {
    throw std::runtime_error("consumer callback failed");
  }, 8));
  BOOST_CHECK(lifecycle.state() == StreamUserLifecycleState::Failed);
  BOOST_CHECK(!lifecycle.dispatchCallback([] (int) {}, 9));
}

BOOST_AUTO_TEST_CASE(ProviderFenceStopsOldAttemptWithoutClaimingSuccess)
{
  auto authority = std::make_shared<StreamTerminalAuthority>();
  StreamUserLifecycle user(authority);
  StreamProviderLifecycle provider(authority);
  user.beginRequest();
  provider.activate();
  provider.fence();

  BOOST_CHECK(provider.state() == StreamProviderLifecycleState::Fenced);
  BOOST_CHECK(provider.terminalAuthority()->isFenced());
  BOOST_CHECK(provider.terminalAuthority()->state() == StreamTerminalState::None);
  BOOST_CHECK_THROW(user.beginSelection(), std::logic_error);
  BOOST_CHECK_NO_THROW(user.cancel());
  BOOST_CHECK(user.state() == StreamUserLifecycleState::Cancelled);
}

BOOST_AUTO_TEST_CASE(BoundedQueueNeverExceedsCapacityAndPropagatesDeadline)
{
  BoundedStreamQueue<int> queue(1);
  BOOST_CHECK(queue.tryPush(1));
  BOOST_CHECK(!queue.tryPush(2));
  BOOST_CHECK_EQUAL(queue.size(), 1);
  BOOST_CHECK_EQUAL(queue.highWaterMark(), 1);
  BOOST_CHECK(!queue.waitPush(
    2, std::chrono::steady_clock::now() + std::chrono::milliseconds(2)));

  int value = 0;
  BOOST_REQUIRE(queue.tryPop(value));
  BOOST_CHECK_EQUAL(value, 1);
  BOOST_CHECK(queue.waitPush(
    2, std::chrono::steady_clock::now() + std::chrono::milliseconds(2)));
  BOOST_CHECK_EQUAL(queue.highWaterMark(), 1);
  queue.close();
  BOOST_CHECK(!queue.tryPush(3));
  BOOST_REQUIRE(queue.tryPop(value));
  BOOST_CHECK_EQUAL(value, 2);
  BOOST_CHECK(!queue.tryPop(value));
}

BOOST_AUTO_TEST_CASE(BoundedQueueWaitUnblocksOnPopAndClose)
{
  BoundedStreamQueue<int> queue(1);
  BOOST_REQUIRE(queue.tryPush(7));
  bool pushed = false;
  std::thread producer([&] {
    pushed = queue.waitPush(
      8, std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  int value = 0;
  BOOST_REQUIRE(queue.tryPop(value));
  producer.join();
  BOOST_CHECK(pushed);
  BOOST_CHECK_EQUAL(queue.size(), 1);
  queue.close();
  BOOST_CHECK(!queue.waitPush(
    9, std::chrono::steady_clock::now() + std::chrono::milliseconds(2)));
}

BOOST_AUTO_TEST_CASE(StreamedResponseWriterCoreEnforcesSingleTerminalClaim)
{
  size_t published = 0;
  bool finished = false;
  bool queriedFromCallback = false;
  std::shared_ptr<StreamedResponseWriterCore> writer;
  writer = std::make_shared<StreamedResponseWriterCore>(
    [&] (const ndn::Buffer&, uint64_t& cursor) {
      cursor = ++published;
      queriedFromCallback = !writer->isCancelled();
      return true;
    },
    [&] (const ndn::Buffer&, StreamFinishReason reason) {
      finished = reason == StreamFinishReason::Eos;
      queriedFromCallback = queriedFromCallback && !writer->isCancelled();
      return true;
    },
    [] (StreamedInvocationErrorCode, const std::string&) { return true; },
    [] { return false; },
    [] { return std::chrono::milliseconds(37); });

  ndn::Buffer payload(reinterpret_cast<const uint8_t*>("event"), 5);
  uint64_t cursor = 0;
  BOOST_REQUIRE(writer->publish(payload, cursor));
  BOOST_CHECK_EQUAL(cursor, 1);
  BOOST_CHECK(queriedFromCallback);
  BOOST_CHECK_EQUAL(writer->remainingDeadline().count(), 37);
  BOOST_REQUIRE(writer->finish(payload, StreamFinishReason::Eos));
  BOOST_CHECK(finished);
  BOOST_CHECK(writer->isTerminal());
  BOOST_CHECK(!writer->publish(payload, cursor));
  BOOST_CHECK(!writer->finish(payload, StreamFinishReason::Eos));
}

BOOST_AUTO_TEST_CASE(StreamedResponseWriterCoreFailureIsTerminal)
{
  StreamedInvocationErrorCode observed = StreamedInvocationErrorCode::EventTimeout;
  std::string message;
  StreamedResponseWriterCore writer(
    [] (const ndn::Buffer&, uint64_t&) { return true; },
    [] (const ndn::Buffer&, StreamFinishReason) { return true; },
    [&] (StreamedInvocationErrorCode code, const std::string& text) {
      observed = code;
      message = text;
      return true;
    },
    [] { return false; },
    [] { return std::chrono::milliseconds(0); });

  BOOST_REQUIRE(writer.fail(StreamedInvocationErrorCode::ProviderFailure,
                            "provider rejected stream"));
  BOOST_CHECK(writer.isTerminal());
  BOOST_CHECK(observed == StreamedInvocationErrorCode::ProviderFailure);
  BOOST_CHECK_EQUAL(message, "provider rejected stream");
  BOOST_CHECK(!writer.fail(StreamedInvocationErrorCode::EventTimeout, "late"));
}

BOOST_AUTO_TEST_CASE(StreamGrantBindingExcludesOnlyFinalSelectionDigest)
{
  StreamBinding binding;
  binding.requestId = ndn::Name("/request/grant-binding");
  binding.requester = ndn::Name("/user/grant-binding");
  binding.serviceName = ndn::Name("/LLM/Test");
  binding.producer = ndn::Name("/provider/grant-binding");
  binding.producerBootId = "provider:/provider/grant-binding:7";
  binding.attemptEpoch = 1;
  binding.planDigest.fill(0x11);
  binding.generationId.fill(0x22);
  binding.streamEpoch = 3;
  binding.eventKeyCommitment.fill(0x33);
  binding.userToken = ndn::Buffer(
    reinterpret_cast<const uint8_t*>("token"), 5);
  binding.policyEpoch = 9;
  binding.deadlineEpochMs = 123456789;

  const auto grantDigest = computeStreamGrantBindingDigest(binding);
  auto differentPlan = binding;
  differentPlan.planDigest.fill(0x44);
  BOOST_CHECK(grantDigest == computeStreamGrantBindingDigest(differentPlan));

  auto differentProvider = binding;
  differentProvider.producer = ndn::Name("/provider/other");
  BOOST_CHECK(grantDigest != computeStreamGrantBindingDigest(differentProvider));
}

BOOST_AUTO_TEST_CASE(StreamEventPublisherEncryptsSignsRetainsAndRepublishesExactWire)
{
  ndn::KeyChain keyChain("pib-memory:spec175-publisher",
                         "tpm-memory:spec175-publisher");
  const ndn::Name provider("/test/provider/stream-publisher");
  const ndn::Name requester("/test/user/stream-consumer");
  const ndn::Name service("/LLM/Test");
  const ndn::Name requestId("/request-stream-publisher");
  const auto identity = keyChain.createIdentity(provider, ndn::RsaKeyParams(2048));
  const auto signingCertificate =
    keyChain.createKey(identity, ndn::EcKeyParams()).getDefaultCertificate();

  StreamBinding binding;
  binding.requestId = requestId;
  binding.requester = requester;
  binding.serviceName = service;
  binding.producer = provider;
  binding.producerBootId = "boot-stream-publisher";
  binding.attemptEpoch = 1;
  binding.planDigest.fill(0x11);
  binding.generationId.fill(0x22);
  binding.streamEpoch = 1;
  binding.userToken = ndn::Buffer(
    reinterpret_cast<const uint8_t*>("user-token"), 10);
  binding.policyEpoch = 7;
  binding.deadlineEpochMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 10000;

  ndn::Buffer eventKey(32, 0x42);
  binding.eventKeyCommitment = computeStreamSha256(
    ndn::span<const uint8_t>(eventKey.data(), eventKey.size()));

  StreamRequestOptions options;
  options.generationId = binding.generationId;
  options.streamEpoch = binding.streamEpoch;
  options.eventKeyCommitment = binding.eventKeyCommitment;
  options.deadlineEpochMs = binding.deadlineEpochMs;
  // One application event plus the authenticated End exactly exhausts the
  // two-cursor budget. maxEvents includes End rather than only payloads.
  options.maxEvents = 2;
  options.publisherQueueCapacity = 2;
  options.retentionMs = 1000;
  options.maxEventWireBytes = 4096;

  auto lifecycle = std::make_shared<StreamInvocationLifecycle>();
  std::vector<PublishedStreamEvent> published;
  StreamEventPublisher publisher(
    binding, options, eventKey, lifecycle, keyChain,
    ndn::security::signingByCertificate(signingCertificate),
    [&] (const PublishedStreamEvent& event) { published.push_back(event); });
  publisher.start();

  ndn::Buffer payload(reinterpret_cast<const uint8_t*>("token-1"), 7);
  const auto first = publisher.publish(
    payload, std::chrono::steady_clock::now() + std::chrono::seconds(1));
  BOOST_REQUIRE(first);
  BOOST_CHECK_EQUAL(first->cursor, 1);
  BOOST_CHECK_EQUAL(publisher.nextCursor(), 2);
  BOOST_CHECK_EQUAL(publisher.retainedCount(), 1);
  BOOST_CHECK(publisher.highWaterMark() <= options.publisherQueueCapacity);
  BOOST_CHECK_EQUAL(published.size(), 1);

  ndn::Data decoded;
  decoded.wireDecode(ndn::Block(first->signedWire));
  BOOST_CHECK(ndn::security::verifySignature(decoded, signingCertificate));
  HybridMessageEnvelope envelope;
  BOOST_REQUIRE(envelope.WireDecode(decoded.getContent().blockFromValue()));
  BOOST_CHECK_EQUAL(envelope.getMessageType(), "EVENT");
  const auto bindingDigest = computeStreamBindingDigest(binding);
  StreamDigest eventKeyDigest{};
  std::copy(eventKey.begin(), eventKey.end(), eventKeyDigest.begin());
  BOOST_CHECK_EQUAL(eventKeyDigest.size(), 32);
  const auto ad = makeInvocationEventAssociatedData(
    first->name, bindingDigest, first->cursor);
  ndn::Buffer plaintext;
  BOOST_REQUIRE(hybridAesGcmDecrypt(
    eventKey, envelope, ndn::span<const uint8_t>(ad.data(), ad.size()), plaintext));
  InvocationEventMessage decodedEvent;
  BOOST_REQUIRE(decodedEvent.wireDecode(ndn::Block(plaintext)));
  BOOST_CHECK_EQUAL_COLLECTIONS(decodedEvent.payload.begin(), decodedEvent.payload.end(),
                                payload.begin(), payload.end());

  ndn::Interest exact(first->name);
  exact.setCanBePrefix(false);
  const auto satisfied = publisher.satisfy(exact);
  BOOST_REQUIRE(satisfied);
  BOOST_CHECK_EQUAL_COLLECTIONS(satisfied->signedWire.begin(), satisfied->signedWire.end(),
                                first->signedWire.begin(), first->signedWire.end());
  const auto retried = publisher.republish(first->name);
  BOOST_REQUIRE(retried);
  BOOST_CHECK_EQUAL_COLLECTIONS(retried->signedWire.begin(), retried->signedWire.end(),
                                first->signedWire.begin(), first->signedWire.end());

  const auto completion = publisher.finish(
    payload, StreamFinishReason::Eos,
    std::chrono::steady_clock::now() + std::chrono::seconds(1));
  BOOST_REQUIRE(completion);
  BOOST_CHECK_EQUAL(completion->finalCursor, 2);
  BOOST_CHECK_EQUAL(published.size(), 3); // first publish, exact retry, End
  BOOST_CHECK(lifecycle->terminalAuthority()->state() == StreamTerminalState::Completed);
  BOOST_CHECK(!publisher.finish(
    payload, StreamFinishReason::Eos,
    std::chrono::steady_clock::now() + std::chrono::seconds(1)));

  ndn::Interest prefix(first->name);
  prefix.setCanBePrefix(true);
  BOOST_CHECK(!publisher.satisfy(prefix));
}

BOOST_AUTO_TEST_CASE(StreamEventPublisherCapacityBoundsInFlightPublication)
{
  ndn::KeyChain keyChain("pib-memory:spec175-publisher-capacity",
                         "tpm-memory:spec175-publisher-capacity");
  const ndn::Name provider("/test/provider/stream-publisher-capacity");
  const auto identity = keyChain.createIdentity(provider, ndn::RsaKeyParams(2048));
  const auto signingCertificate =
    keyChain.createKey(identity, ndn::EcKeyParams()).getDefaultCertificate();

  StreamBinding binding;
  binding.requestId = ndn::Name("/request-stream-publisher-capacity");
  binding.requester = ndn::Name("/test/user/stream-publisher-capacity");
  binding.serviceName = ndn::Name("/LLM/Test");
  binding.producer = provider;
  binding.producerBootId = "boot-stream-publisher-capacity";
  binding.attemptEpoch = 1;
  binding.planDigest.fill(0x11);
  binding.generationId.fill(0x22);
  binding.streamEpoch = 1;
  binding.userToken = ndn::Buffer(
    reinterpret_cast<const uint8_t*>("user-token"), 10);
  binding.policyEpoch = 7;
  binding.deadlineEpochMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 10000;

  ndn::Buffer eventKey(32, 0x42);
  binding.eventKeyCommitment = computeStreamSha256(
    ndn::span<const uint8_t>(eventKey.data(), eventKey.size()));

  StreamRequestOptions options;
  options.generationId = binding.generationId;
  options.streamEpoch = binding.streamEpoch;
  options.eventKeyCommitment = binding.eventKeyCommitment;
  options.deadlineEpochMs = binding.deadlineEpochMs;
  options.maxEvents = 8;
  options.publisherQueueCapacity = 1;
  options.retentionMs = 1000;
  options.maxEventWireBytes = 4096;

  auto lifecycle = std::make_shared<StreamInvocationLifecycle>();
  std::atomic<bool> firstPublishEntered{false};
  std::atomic<bool> releaseFirstPublish{false};
  StreamEventPublisher publisher(
    binding, options, eventKey, lifecycle, keyChain,
    ndn::security::signingByCertificate(signingCertificate),
    [&] (const PublishedStreamEvent& event) {
      if (event.cursor != 1) {
        return;
      }
      firstPublishEntered = true;
      while (!releaseFirstPublish.load()) {
        std::this_thread::yield();
      }
    });
  publisher.start();

  const ndn::Buffer payload(reinterpret_cast<const uint8_t*>("token"), 5);
  std::optional<PublishedStreamEvent> first;
  std::thread firstThread([&] {
    first = publisher.publish(
      payload, std::chrono::steady_clock::now() + std::chrono::seconds(1));
  });
  for (int attempt = 0; attempt < 1000 && !firstPublishEntered.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  BOOST_REQUIRE(firstPublishEntered.load());

  const auto second = publisher.publish(
    payload, std::chrono::steady_clock::now() + std::chrono::milliseconds(20));
  BOOST_CHECK(!second);
  BOOST_CHECK_EQUAL(publisher.highWaterMark(), 1U);
  BOOST_CHECK_EQUAL(publisher.nextCursor(), 2U);
  BOOST_CHECK(lifecycle->terminalAuthority()->state() == StreamTerminalState::Failed);

  releaseFirstPublish = true;
  firstThread.join();
  BOOST_REQUIRE(first);
}

BOOST_AUTO_TEST_CASE(StreamEventConsumerReordersDeduplicatesAndClosesOnMatchingResponse)
{
  ndn::KeyChain keyChain("pib-memory:spec175-consumer",
                         "tpm-memory:spec175-consumer");
  const ndn::Name provider("/test/provider/stream-consumer");
  const ndn::Name requester("/test/user/stream-consumer");
  const ndn::Name service("/LLM/Test");
  const ndn::Name requestId("/request-stream-consumer");
  const auto identity = keyChain.createIdentity(provider, ndn::RsaKeyParams(2048));
  const auto signingCertificate =
    keyChain.createKey(identity, ndn::EcKeyParams()).getDefaultCertificate();

  StreamBinding binding;
  binding.requestId = requestId;
  binding.requester = requester;
  binding.serviceName = service;
  binding.producer = provider;
  binding.producerBootId = "boot-stream-consumer";
  binding.attemptEpoch = 1;
  const std::string progressSelectionDigest = "selection-digest-progress";
  binding.planDigest = computeStreamSha256(
    ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(progressSelectionDigest.data()),
      progressSelectionDigest.size()));
  binding.generationId.fill(0x55);
  binding.streamEpoch = 1;
  binding.userToken = ndn::Buffer(
    reinterpret_cast<const uint8_t*>("user-token"), 10);
  binding.policyEpoch = 7;
  binding.deadlineEpochMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 10000;
  ndn::Buffer eventKey(32, 0x24);
  binding.eventKeyCommitment = computeStreamSha256(
    ndn::span<const uint8_t>(eventKey.data(), eventKey.size()));

  StreamRequestOptions options;
  options.generationId = binding.generationId;
  options.streamEpoch = binding.streamEpoch;
  options.eventKeyCommitment = binding.eventKeyCommitment;
  options.deadlineEpochMs = binding.deadlineEpochMs;
  options.maxEvents = 8;
  options.interestWindow = 4;
  options.reorderCapacity = 4;
  options.maxEventRetries = 3;
  options.interestLifetimeMs = 100;
  options.maxEventWireBytes = 4096;

  auto silentLifecycle = std::make_shared<StreamInvocationLifecycle>();
  auto silentOptions = options;
  silentOptions.maxEventRetries = 1;
  size_t silentErrors = 0;
  std::vector<ndn::Name> silentRetries;
  StreamEventConsumer silentConsumer(
    binding, silentOptions, eventKey, silentLifecycle,
    [] (const ndn::Data&) { return true; },
    [] (const InvocationEventMessage&) {},
    [] (const ResponseMessage&) {},
    [&] (const StreamedInvocationError&) { ++silentErrors; },
    [&] (const ndn::Name& name) { silentRetries.push_back(name); });
  silentConsumer.start();
  const auto silentTimeout = std::chrono::steady_clock::now();
  silentConsumer.onInactivityTimeout(silentTimeout);
  BOOST_CHECK_EQUAL(silentErrors, 0);
  BOOST_REQUIRE_EQUAL(silentRetries.size(), 1);
  BOOST_CHECK_EQUAL(silentRetries.front(), makeInvocationEventName(binding, 1));
  // The retry's real timeout callback is authoritative even if clock
  // granularity places it just before the estimated nextRetryAt_.
  silentConsumer.onRetryTimeout(
    makeInvocationEventName(binding, 1),
    silentTimeout + std::chrono::milliseconds(1));
  BOOST_CHECK_EQUAL(silentErrors, 0);
  BOOST_CHECK(silentLifecycle->user().state() !=
              StreamUserLifecycleState::Failed);
  BOOST_REQUIRE_EQUAL(silentRetries.size(), 2);
  // Before the first application event, a large-model assembly window may
  // outlive one bounded retry batch. The consumer keeps polling; the request
  // deadline remains the enclosing timeout authority.
  silentConsumer.onRetryTimeout(
    makeInvocationEventName(binding, 1),
    silentTimeout + std::chrono::milliseconds(2));
  BOOST_CHECK_EQUAL(silentErrors, 0);
  BOOST_CHECK_EQUAL(silentRetries.size(), 3);

  // An authenticated post-Selection RUNNING status re-arms the bounded retry
  // budget for the same Provider operation, including a repeated status while
  // a long assembly worker is busy. A mismatched request or another Provider
  // cannot keep a stream alive; the enclosing request deadline remains the
  // final bound.
  auto progressLifecycle = std::make_shared<StreamInvocationLifecycle>();
  auto progressOptions = silentOptions;
  progressOptions.maxEventRetries = 1;
  size_t progressErrors = 0;
  std::vector<ndn::Name> progressRetries;
  StreamEventConsumer progressConsumer(
    binding, progressOptions, eventKey, progressLifecycle,
    [] (const ndn::Data&) { return true; },
    [] (const InvocationEventMessage&) {},
    [] (const ResponseMessage&) {},
    [&] (const StreamedInvocationError&) { ++progressErrors; },
    [&] (const ndn::Name& name) { progressRetries.push_back(name); },
    {},
    progressSelectionDigest + ":terminal:assembly-progress");
  progressConsumer.start();
  const auto progressNow = std::chrono::steady_clock::now();
  progressConsumer.onInactivityTimeout(progressNow);
  BOOST_REQUIRE_EQUAL(progressRetries.size(), 1);

  SelectionExecutionStatus progressStatus;
  progressStatus.providerName = provider;
  progressStatus.serviceName = service;
  progressStatus.requestId = requestId;
  progressStatus.selectionDigest = progressSelectionDigest;
  CollaborationMemberStatus progressMember;
  progressMember.providerName = provider;
  progressMember.serviceName = service;
  progressMember.requestId = requestId;
  progressMember.selectionDigest = progressStatus.selectionDigest;
  progressMember.role = "terminal";
  progressMember.operationId = "selection-digest-progress:terminal:assembly-progress";
  progressMember.operation = "ensure-deployment";
  progressMember.state = "RUNNING";
  progressMember.epoch = 1;
  progressMember.sequence = 1;
  progressMember.progressKnown = true;
  progressMember.progress = 0.0;
  progressMember.detailsSchema = "ndnsf-di-preparation-progress-v1";
  progressStatus.memberStatuses.push_back(progressMember);
  BOOST_CHECK(progressConsumer.observeAuthenticatedProgress(progressStatus));

  progressConsumer.onRetryTimeout(
    makeInvocationEventName(binding, 1),
    progressNow + std::chrono::milliseconds(1));
  BOOST_CHECK_EQUAL(progressErrors, 0U);

  // Assembly proper continues the same authenticated progress epoch after
  // input/queue admission work. Its next sequence extends the operation.
  progressStatus.memberStatuses.front().sequence = 2;
  progressStatus.memberStatuses.front().progress = 0.5;
  BOOST_CHECK(progressConsumer.observeAuthenticatedProgress(progressStatus));

  // A runner rebuild/retry reuses the request-scoped counter rather than
  // restarting the same operation at sequence two.
  progressStatus.memberStatuses.front().sequence = 3;
  progressStatus.memberStatuses.front().progress = 0.75;
  BOOST_CHECK(progressConsumer.observeAuthenticatedProgress(progressStatus));

  // A second role hosted by the same Provider cannot extend the terminal
  // stream.  Provider/service/request/selection identity alone is not enough
  // to select the user-facing response owner.
  auto wrongRole = progressStatus;
  wrongRole.memberStatuses.front().role = "worker";
  wrongRole.memberStatuses.front().operationId =
    "selection-digest-progress:worker:assembly-progress";
  wrongRole.memberStatuses.front().sequence = 2;
  BOOST_CHECK(!progressConsumer.observeAuthenticatedProgress(wrongRole));

  // A collaboration terminal consumer accepts the same Selection-scoped
  // progress operation from each selected Provider.  The Provider and member
  // identities must still agree, and the operation must be one of the exact
  // role bindings supplied by the committed Selection.
  auto collaborationProgressLifecycle =
    std::make_shared<StreamInvocationLifecycle>();
  std::vector<ndn::Name> collaborationProgressRetries;
  const ndn::Name workerProvider("/test/provider/stream-worker");
  const std::string workerSelectionDigest = "selection-digest-worker";
  StreamEventConsumer collaborationProgressConsumer(
    binding, progressOptions, eventKey, collaborationProgressLifecycle,
    [] (const ndn::Data&) { return true; },
    [] (const InvocationEventMessage&) {},
    [] (const ResponseMessage&) {},
    [] (const StreamedInvocationError&) {},
    [&] (const ndn::Name& name) { collaborationProgressRetries.push_back(name); },
    {},
    progressSelectionDigest + ":terminal:assembly-progress",
    {{provider.toUri(), progressSelectionDigest,
      progressSelectionDigest + ":terminal:assembly-progress"},
     {provider.toUri(), progressSelectionDigest,
      progressSelectionDigest + ":worker:assembly-progress"},
     {workerProvider.toUri(), workerSelectionDigest,
      workerSelectionDigest + ":worker:assembly-progress"}});
  collaborationProgressConsumer.start();
  collaborationProgressConsumer.onInactivityTimeout(progressNow);
  BOOST_REQUIRE_EQUAL(collaborationProgressRetries.size(), 1);
  auto sameProviderWorkerStatus = progressStatus;
  sameProviderWorkerStatus.memberStatuses.front().role = "worker";
  sameProviderWorkerStatus.memberStatuses.front().operationId =
    progressSelectionDigest + ":worker:assembly-progress";
  BOOST_CHECK(collaborationProgressConsumer.observeAuthenticatedProgress(
    sameProviderWorkerStatus));
  auto workerProgressStatus = progressStatus;
  workerProgressStatus.providerName = workerProvider;
  workerProgressStatus.selectionDigest = workerSelectionDigest;
  workerProgressStatus.memberStatuses.front().providerName = workerProvider;
  workerProgressStatus.memberStatuses.front().selectionDigest = workerSelectionDigest;
  workerProgressStatus.memberStatuses.front().role = "worker";
  workerProgressStatus.memberStatuses.front().operationId =
    workerSelectionDigest + ":worker:assembly-progress";
  BOOST_CHECK(collaborationProgressConsumer.observeAuthenticatedProgress(
    workerProgressStatus));
  // Progress may move from the worker to the terminal Provider. Each exact
  // provider/selection/operation tuple has its own freshness state, while a
  // repeated valid status remains a liveness signal.
  BOOST_CHECK(collaborationProgressConsumer.observeAuthenticatedProgress(
    progressStatus));
  auto mixedProgressStatus = progressStatus;
  mixedProgressStatus.memberStatuses.front().sequence = 4;
  auto staleWorkerMember = sameProviderWorkerStatus.memberStatuses.front();
  staleWorkerMember.sequence = 1;
  mixedProgressStatus.memberStatuses.push_back(staleWorkerMember);
  BOOST_CHECK(collaborationProgressConsumer.observeAuthenticatedProgress(
    mixedProgressStatus));
  BOOST_CHECK(collaborationProgressConsumer.observeAuthenticatedProgress(
    workerProgressStatus));
  auto mismatchedWorkerMember = workerProgressStatus;
  mismatchedWorkerMember.memberStatuses.front().providerName = provider;
  BOOST_CHECK(!collaborationProgressConsumer.observeAuthenticatedProgress(
    mismatchedWorkerMember));

  auto wrongRequest = progressStatus;
  wrongRequest.requestId = ndn::Name("/other-request");
  BOOST_CHECK(!progressConsumer.observeAuthenticatedProgress(wrongRequest));
  auto wrongProvider = progressStatus;
  wrongProvider.providerName = ndn::Name("/other-provider");
  BOOST_CHECK(!progressConsumer.observeAuthenticatedProgress(wrongProvider));
  auto oldSelection = progressStatus;
  oldSelection.selectionDigest = "selection-digest-before-replacement";
  oldSelection.memberStatuses.front().selectionDigest = oldSelection.selectionDigest;
  BOOST_CHECK(!progressConsumer.observeAuthenticatedProgress(oldSelection));

  // A repeated authenticated RUNNING status remains a valid liveness signal
  // while the worker is busy and refreshes the bounded retry budget.
  progressConsumer.onRetryTimeout(
    makeInvocationEventName(binding, 1),
    progressNow + std::chrono::milliseconds(1));
  BOOST_REQUIRE_EQUAL(progressRetries.size(), 3);
  BOOST_CHECK(progressConsumer.observeAuthenticatedProgress(progressStatus));
  progressConsumer.onRetryTimeout(
    makeInvocationEventName(binding, 1),
    progressNow + std::chrono::milliseconds(2));
  BOOST_CHECK_EQUAL(progressErrors, 0U);
  BOOST_CHECK(progressLifecycle->user().state() != StreamUserLifecycleState::Failed);
  progressConsumer.onRetryTimeout(
    makeInvocationEventName(binding, 1),
    progressNow + std::chrono::milliseconds(3));
  BOOST_CHECK_EQUAL(progressErrors, 1U);
  BOOST_CHECK(progressLifecycle->user().state() == StreamUserLifecycleState::Failed);

  auto providerLifecycle = std::make_shared<StreamInvocationLifecycle>();
  std::vector<PublishedStreamEvent> wire;
  StreamEventPublisher publisher(
    binding, options, eventKey, providerLifecycle, keyChain,
    ndn::security::signingByCertificate(signingCertificate),
    [&] (const PublishedStreamEvent& event) { wire.push_back(event); });
  publisher.start();
  const auto makePayload = [] (const char* text) {
    return ndn::Buffer(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  };
  BOOST_REQUIRE(publisher.publish(
    makePayload("one"), std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  BOOST_REQUIRE(publisher.publish(
    makePayload("two"), std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  const auto completion = publisher.finish(
    makePayload("final"), StreamFinishReason::Eos,
    std::chrono::steady_clock::now() + std::chrono::seconds(1));
  BOOST_REQUIRE(completion);
  BOOST_REQUIRE_EQUAL(wire.size(), 3);

  auto consumerLifecycle = std::make_shared<StreamInvocationLifecycle>();
  std::vector<std::string> received;
  std::vector<ndn::Name> retries;
  size_t errors = 0;
  bool completed = false;
  StreamEventConsumer consumer(
    binding, options, eventKey, consumerLifecycle,
    [&] (const ndn::Data& data) {
      return ndn::security::verifySignature(data, signingCertificate);
    },
    [&] (const InvocationEventMessage& event) {
      received.emplace_back(reinterpret_cast<const char*>(event.payload.data()),
                            event.payload.size());
    },
    [&] (const ResponseMessage&) { completed = true; },
    [&] (const StreamedInvocationError&) { ++errors; },
    [&] (const ndn::Name& name) { retries.push_back(name); });
  consumer.start();

  // A completely silent Provider leaves no later event in the reorder buffer.
  // The exact timeout for the currently expected cursor must still request a
  // retry, while a speculative timeout elsewhere in the Interest window must
  // not consume retry budget.
  const auto retryNow = std::chrono::steady_clock::now();
  consumer.onRetryTimeout(makeInvocationEventName(binding, 1), retryNow);
  BOOST_REQUIRE_EQUAL(retries.size(), 1);
  BOOST_CHECK_EQUAL(retries.front(), makeInvocationEventName(binding, 1));
  BOOST_CHECK_EQUAL(consumer.retryCount(), 1);
  consumer.onRetryTimeout(
    makeInvocationEventName(binding, 2), retryNow + std::chrono::seconds(1));
  BOOST_CHECK_EQUAL(retries.size(), 1);
  BOOST_CHECK_EQUAL(consumer.retryCount(), 1);

  ndn::Data second;
  second.wireDecode(ndn::Block(wire[1].signedWire));
  BOOST_REQUIRE(consumer.accept(second));
  BOOST_CHECK_EQUAL(consumer.expectedCursor(), 1);
  BOOST_CHECK_EQUAL(consumer.reorderSize(), 1);
  BOOST_CHECK_EQUAL(retries.size(), 1);

  ndn::Data first;
  first.wireDecode(ndn::Block(wire[0].signedWire));
  BOOST_REQUIRE(consumer.accept(first));
  BOOST_CHECK_EQUAL(consumer.expectedCursor(), 3);
  const std::vector<std::string> expectedReceived{"one", "two"};
  BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(),
                                expectedReceived.begin(), expectedReceived.end());
  BOOST_REQUIRE(consumer.accept(first)); // duplicate is suppressed
  BOOST_CHECK_EQUAL(received.size(), 2);

  // A provider may publish Response before the exact End Data is fetched.
  // The consumer must retain that authenticated Response and complete only
  // after End closes the cursor.
  ResponseMessage response;
  response.setStatus(true);
  auto finalPayload = makePayload("final");
  response.setPayload(finalPayload, finalPayload.size());
  response.setStreamCompletion(*completion);
  BOOST_REQUIRE(consumer.acceptResponse(response));
  BOOST_CHECK(!completed);

  ndn::Data end;
  end.wireDecode(ndn::Block(wire[2].signedWire));
  BOOST_REQUIRE(consumer.accept(end));
  BOOST_CHECK(consumerLifecycle->user().state() == StreamUserLifecycleState::Completed);
  BOOST_CHECK(completed);
  BOOST_CHECK_EQUAL(errors, 0);
  BOOST_CHECK(consumerLifecycle->terminalAuthority()->state() ==
              StreamTerminalState::Completed);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
