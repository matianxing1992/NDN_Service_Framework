#ifndef NDN_SERVICE_FRAMEWORK_INVOCATION_STREAM_HPP
#define NDN_SERVICE_FRAMEWORK_INVOCATION_STREAM_HPP

#include "common.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ndn_service_framework {

namespace tlv {
enum : uint32_t {
  StreamRequestOptionsType = 0xF661,
  InvocationEventMessageType = 0xF662,
  StreamCompletionType = 0xF663,
  StreamGenerationIdType = 0xF664,
  StreamEpochType = 0xF665,
  StreamEventKeyGrantType = 0xF666,
  StreamMaxEventsType = 0xF667,
  StreamInterestWindowType = 0xF668,
  StreamInterestLifetimeMsType = 0xF669,
  StreamMaxEventRetriesType = 0xF66A,
  StreamPublisherQueueType = 0xF66B,
  StreamCallbackQueueType = 0xF66C,
  StreamReorderCapacityType = 0xF66D,
  StreamRetentionMsType = 0xF66E,
  StreamCompletionGraceMsType = 0xF66F,
  StreamMaxEventWireBytesType = 0xF670,
  StreamAllowReplacementType = 0xF671,
  StreamMaxReplacementsType = 0xF672,
  StreamBindingDigestType = 0xF673,
  StreamCursorType = 0xF674,
  StreamEventTypeType = 0xF675,
  StreamPayloadDigestType = 0xF676,
  StreamPublishedAtUsType = 0xF677,
  StreamTerminalType = 0xF678,
  StreamEndEventType = 0xF679,
  StreamFinalCursorType = 0xF67A,
  StreamFinishReasonType = 0xF67B,
  StreamApplicationEventCountType = 0xF67C,
  StreamGeneratedTokenCountType = 0xF67D,
  StreamTranscriptDigestType = 0xF67E,
  StreamFinalResultDigestType = 0xF67F,
  StreamEndEventNameType = 0xF680,
  StreamInvocationModeType = 0xF681,
  StreamEventKeyCommitmentType = 0xF682,
  StreamDeadlineEpochMsType = 0xF683,
  StreamAttemptEpochType = 0xF684,
  ConversationContinuationType = 0xF685,
  ConversationModeType = 0xF686,
  ConversationIdType = 0xF687,
  ConversationParentEpochType = 0xF688,
  ConversationContextEpochType = 0xF689,
  ConversationCheckpointType = 0xF68A,
  ConversationCheckpointDigestType = 0xF68B,
  ConversationTurnInputDigestType = 0xF68C,
  ConversationAllowFallbackType = 0xF68D,
  ConversationFallbackInputDigestType = 0xF68E,
  ConversationRoleReceiptType = 0xF68F,
  ConversationRoleReceiptDigestType = 0xF690,
  ConversationPlanRoleMapDigestType = 0xF691,
  ConversationStateResidencyType = 0xF692,
  ConversationStateReadyType = 0xF693,
  ConversationStateReasonType = 0xF694,
  ConversationStateBytesType = 0xF695,
  ConversationExpiresAtType = 0xF696,
  ConversationCommitStatusType = 0xF697,
  ConversationTurnCompletionType = 0xF698,
};
} // namespace tlv

using StreamDigest = std::array<uint8_t, 32>;
using StreamGenerationId = std::array<uint8_t, 16>;
using StreamNonce = std::array<uint8_t, 12>;

enum class InvocationMode : uint64_t {
  Normal = 0,
  Targeted = 1,
};

/** Optional request-scoped continuation contract for a multi-turn
 * conversation.  It is deliberately separate from StreamRequestOptions:
 * request-local decode state is never addressable by a later request, while
 * this block carries only an authenticated opaque parent checkpoint and
 * canonical input commitments.  No Provider, role, tensor, pointer, or path
 * is represented here.
 */
enum class ConversationInputMode : uint64_t {
  FullContext = 0,
  AppendDelta = 1,
};

struct ConversationContinuationOptions
{
  static constexpr uint64_t VERSION = 1;

  uint64_t version = VERSION;
  ConversationInputMode mode = ConversationInputMode::FullContext;
  std::array<uint8_t, 16> conversationId{};
  std::optional<uint64_t> parentContextEpoch;
  std::optional<ndn::Buffer> parentCheckpoint;
  StreamDigest turnInputDigest{};
  bool allowFullPrefillFallback = false;
  std::optional<StreamDigest> fallbackInputDigest;

  void validate() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& wire);
  bool operator==(const ConversationContinuationOptions& other) const;
};

enum class InvocationEventType : uint64_t {
  Application = 1,
  End = 2,
};

enum class StreamFinishReason : uint64_t {
  Eos = 1,
  StopSequence = 2,
  MaxTokens = 3,
  ApplicationComplete = 4,
  Deadline = 5,
  Cancelled = 6,
  Failed = 7,
};

enum class StreamedInvocationStatus : uint8_t {
  Created = 0,
  Requesting = 1,
  Selecting = 2,
  Streaming = 3,
  Draining = 4,
  Completed = 5,
  Failed = 6,
  Cancelled = 7,
};

enum class StreamedInvocationErrorCode : uint8_t {
  StreamUnsupported = 1,
  InvalidOptions = 2,
  Unauthorized = 3,
  AckTimeout = 4,
  SelectionFailed = 5,
  EventTimeout = 6,
  EventOutsideRetention = 7,
  EventOversize = 8,
  QueueDeadline = 9,
  InvalidName = 10,
  InvalidSignature = 11,
  DecryptionFailed = 12,
  BindingMismatch = 13,
  CursorMismatch = 14,
  TranscriptMismatch = 15,
  TerminalMismatch = 16,
  ProviderFailure = 17,
  Deadline = 18,
  Cancelled = 19,
  ApplicationCallbackFailed = 20,
  ReplacementUnavailable = 21,
};

struct StreamedInvocationError {
  StreamedInvocationError() = default;

  StreamedInvocationError(StreamedInvocationErrorCode errorCode,
                          std::string errorMessage,
                          ndn::Name invocationRequestId = {},
                          uint64_t invocationExpectedCursor = 0,
                          ndn::Name failedProviderName = {})
    : code(errorCode)
    , message(std::move(errorMessage))
    , requestId(std::move(invocationRequestId))
    , expectedCursor(invocationExpectedCursor)
    , providerName(std::move(failedProviderName))
  {
  }

  StreamedInvocationErrorCode code = StreamedInvocationErrorCode::EventTimeout;
  std::string message;
  ndn::Name requestId;
  uint64_t expectedCursor = 0;
  /** Provider whose authenticated response/event path produced the failure.
   * Empty means that no Provider has been established and replacement must
   * not guess one from the accepted plan. */
  ndn::Name providerName;
};

struct StreamedInvocationMetrics {
  uint64_t publishedEvents = 0;
  uint64_t deliveredEvents = 0;
  uint64_t retryCount = 0;
  uint64_t duplicateCount = 0;
};

/** Shared state behind the typed public streamed-invocation handle. */
struct StreamedInvocationSharedState {
  mutable std::mutex mutex;
  ndn::Name requestId;
  StreamedInvocationStatus status = StreamedInvocationStatus::Created;
  StreamedInvocationMetrics metrics;
  // The public handle can be cancelled from an application callback thread.
  // Keep a synchronous fence callback in addition to the Face-owned cleanup
  // callback so queued events cannot overtake cancellation.
  std::function<void()> cancelFence;
  std::function<void()> cancel;
  std::function<void(const ndn::Buffer&)> onEvent;
  std::function<void(const ndn::Buffer&)> onComplete;
  std::function<void(const StreamedInvocationError&)> onError;
};

template<typename EventT, typename ResponseT>
class StreamedInvocationHandle
{
public:
  explicit StreamedInvocationHandle(std::shared_ptr<StreamedInvocationSharedState> state)
    : state_(std::move(state))
  {
    if (!state_) {
      throw std::invalid_argument("streamed invocation handle requires state");
    }
  }

  StreamedInvocationHandle(const StreamedInvocationHandle&) = delete;
  StreamedInvocationHandle& operator=(const StreamedInvocationHandle&) = delete;

  const ndn::Name& requestId() const { return state_->requestId; }

  StreamedInvocationStatus status() const
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->status;
  }

  StreamedInvocationMetrics metrics() const
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->metrics;
  }

  void cancel()
  {
    std::function<void()> fence;
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->status != StreamedInvocationStatus::Completed &&
          state_->status != StreamedInvocationStatus::Failed) {
        state_->status = StreamedInvocationStatus::Cancelled;
        fence = state_->cancelFence;
      }
      callback = state_->cancel;
    }
    if (fence) fence();
    if (callback) callback();
  }

  const std::shared_ptr<StreamedInvocationSharedState>& sharedState() const
  {
    return state_;
  }

private:
  std::shared_ptr<StreamedInvocationSharedState> state_;
};

class StreamedResponseWriterCore
{
public:
  using PublishCallback = std::function<bool(const ndn::Buffer&, uint64_t&)>;
  using FinishCallback = std::function<bool(const ndn::Buffer&, StreamFinishReason)>;
  using FailCallback = std::function<bool(StreamedInvocationErrorCode,
                                          const std::string&)>;
  using CancelledCallback = std::function<bool()>;
  using RemainingCallback = std::function<std::chrono::milliseconds()>;

  StreamedResponseWriterCore(PublishCallback publish,
                             FinishCallback finish,
                             FailCallback fail,
                             CancelledCallback cancelled,
                             RemainingCallback remaining)
    : publish_(std::move(publish)), finish_(std::move(finish)),
      fail_(std::move(fail)), cancelled_(std::move(cancelled)),
      remaining_(std::move(remaining))
  {}

  bool publish(const ndn::Buffer& payload, uint64_t& cursor);
  bool finish(const ndn::Buffer& payload, StreamFinishReason reason);
  bool fail(StreamedInvocationErrorCode code, const std::string& message);
  bool isCancelled() const;
  std::chrono::milliseconds remainingDeadline() const;
  bool isTerminal() const;
  void invalidate();

private:
  mutable std::mutex mutex_;
  PublishCallback publish_;
  FinishCallback finish_;
  FailCallback fail_;
  CancelledCallback cancelled_;
  RemainingCallback remaining_;
  bool terminal_ = false;
  bool valid_ = true;
};

template<typename T>
inline ndn::Buffer serializeStreamValue(const T& value)
{
  if constexpr (std::is_same<T, ndn::Buffer>::value) {
    return value;
  }
  else {
    std::string wire;
    if (!value.SerializeToString(&wire)) {
      throw std::invalid_argument("stream value serialization failed");
    }
    return ndn::Buffer(reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
  }
}

template<typename EventT, typename ResponseT>
class StreamedResponseWriter
{
public:
  explicit StreamedResponseWriter(std::shared_ptr<StreamedResponseWriterCore> core)
    : core_(std::move(core))
  {
    if (!core_) throw std::invalid_argument("stream writer requires core");
  }
  StreamedResponseWriter(const StreamedResponseWriter&) = delete;
  StreamedResponseWriter& operator=(const StreamedResponseWriter&) = delete;

  bool publish(const EventT& event)
  {
    try {
      uint64_t cursor = 0;
      return core_->publish(serializeStreamValue(event), cursor);
    }
    catch (...) {
      return false;
    }
  }

  void finish(const ResponseT& response, StreamFinishReason reason)
  {
    if (!core_->finish(serializeStreamValue(response), reason)) {
      throw std::logic_error("stream finish rejected");
    }
  }

  void fail(StreamedInvocationErrorCode code, std::string message)
  {
    if (!core_->fail(code, message)) {
      throw std::logic_error("stream failure already claimed");
    }
  }

  bool isCancelled() const { return core_->isCancelled(); }
  std::chrono::milliseconds remainingDeadline() const
  {
    return core_->remainingDeadline();
  }

  const std::shared_ptr<StreamedResponseWriterCore>& core() const { return core_; }

private:
  std::shared_ptr<StreamedResponseWriterCore> core_;
};

struct StreamRequestOptions
{
  static constexpr uint64_t VERSION = 1;

  uint64_t version = VERSION;
  InvocationMode mode = InvocationMode::Normal;
  StreamGenerationId generationId{};
  uint64_t attemptEpoch = 1;
  uint64_t streamEpoch = 0;
  StreamDigest eventKeyCommitment{};
  // Absolute deadline sealed into the request so both endpoints derive the
  // same StreamBindingV1 digest.
  uint64_t deadlineEpochMs = 0;
  // Exact HybridMessageEnvelope wire. It is allowed only for an explicit
  // selection-free Targeted request; combined Request validation owns that
  // transport-path decision.
  std::optional<ndn::Block> eventKeyGrant;
  uint32_t maxEvents = 512;
  uint16_t interestWindow = 16;
  uint32_t interestLifetimeMs = 500;
  uint8_t maxEventRetries = 3;
  uint16_t publisherQueueCapacity = 64;
  uint16_t callbackQueueCapacity = 64;
  uint16_t reorderCapacity = 64;
  uint32_t retentionMs = 30000;
  uint32_t completionGraceMs = 5000;
  uint32_t maxEventWireBytes = 16384;
  bool allowReplacement = false;
  uint8_t maxReplacements = 0;

  void validate() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& wire);
  bool operator==(const StreamRequestOptions& other) const;
};

/** Public options are intentionally the same bounded wire contract. The
 * request API fills generation/commitment fields after allocating requestId. */
struct StreamedInvocationOptions : StreamRequestOptions {
  StreamedInvocationOptions() = default;
};

struct StreamEndEvent
{
  uint64_t finalCursor = 0;
  StreamFinishReason finishReason = StreamFinishReason::Failed;
  uint64_t applicationEventCount = 0;
  uint64_t generatedTokenCount = 0;
  StreamDigest transcriptDigest{};
  StreamDigest finalResultDigest{};
  std::string errorInfo;

  void validate() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& wire);
  bool operator==(const StreamEndEvent& other) const;
};

struct InvocationEventMessage
{
  static constexpr uint64_t VERSION = 1;

  uint64_t version = VERSION;
  StreamDigest bindingDigest{};
  uint64_t cursor = 0;
  InvocationEventType eventType = InvocationEventType::Application;
  ndn::Buffer payload;
  StreamDigest payloadDigest{};
  uint64_t publishedAtUs = 0;
  ndn::Buffer userToken;
  bool terminal = false;
  std::optional<StreamEndEvent> end;

  void setPayload(const ndn::Buffer& value);
  void validate() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& wire);
  bool operator==(const InvocationEventMessage& other) const;
};

struct StreamCompletion
{
  static constexpr uint64_t VERSION = 1;

  uint64_t version = VERSION;
  StreamDigest bindingDigest{};
  ndn::Name endEventName;
  uint64_t finalCursor = 0;
  StreamFinishReason finishReason = StreamFinishReason::Failed;
  uint64_t applicationEventCount = 0;
  StreamDigest transcriptDigest{};
  StreamDigest finalResultDigest{};

  void validate() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& wire);
  bool operator==(const StreamCompletion& other) const;
};

enum class StreamTerminalState : uint8_t {
  None = 0,
  Completed = 1,
  Failed = 2,
  Cancelled = 3,
};

/**
 * One terminal authority is shared by the user and provider views of an
 * invocation.  Claiming a terminal is atomic; only the first claimant wins.
 * Cancellation is intentionally idempotent at the lifecycle layer.
 */
class StreamTerminalAuthority
{
public:
  bool claim(StreamTerminalState state);
  bool cancel();
  void fence();
  bool isFenced() const;
  bool isTerminal() const;
  StreamTerminalState state() const;

private:
  mutable std::mutex mutex_;
  StreamTerminalState state_ = StreamTerminalState::None;
  bool fenced_ = false;
};

enum class StreamUserLifecycleState : uint8_t {
  Created = 0,
  Requesting = 1,
  Selecting = 2,
  Streaming = 3,
  Draining = 4,
  Completed = 5,
  Failed = 6,
  Cancelled = 7,
};

class StreamUserLifecycle
{
public:
  explicit StreamUserLifecycle(
    std::shared_ptr<StreamTerminalAuthority> authority =
      std::make_shared<StreamTerminalAuthority>());

  StreamUserLifecycleState state() const;
  const std::shared_ptr<StreamTerminalAuthority>& terminalAuthority() const;
  bool isTerminal() const;

  void beginRequest();
  void beginSelection();
  void beginStreaming();
  void beginDraining();
  void complete();
  void fail();
  void cancel();

  template<typename Callback, typename Value>
  bool dispatchCallback(Callback&& callback, const Value& value)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_ != StreamUserLifecycleState::Streaming &&
          state_ != StreamUserLifecycleState::Draining) {
        return false;
      }
      if (authority_->isFenced() || authority_->isTerminal()) {
        return false;
      }
    }
    try {
      std::forward<Callback>(callback)(value);
      return true;
    }
    catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_ == StreamUserLifecycleState::Streaming ||
          state_ == StreamUserLifecycleState::Draining) {
        if (authority_->claim(StreamTerminalState::Failed)) {
          state_ = StreamUserLifecycleState::Failed;
        }
        else {
          adoptExistingTerminal();
        }
      }
      return false;
    }
  }

private:
  void requireState(StreamUserLifecycleState expected) const;
  void adoptExistingTerminal();

private:
  mutable std::mutex mutex_;
  StreamUserLifecycleState state_ = StreamUserLifecycleState::Created;
  std::shared_ptr<StreamTerminalAuthority> authority_;
};

enum class StreamProviderLifecycleState : uint8_t {
  Prepared = 0,
  Active = 1,
  Ending = 2,
  Finished = 3,
  Fenced = 4,
  Failed = 5,
  Cancelled = 6,
};

class StreamProviderLifecycle
{
public:
  explicit StreamProviderLifecycle(
    std::shared_ptr<StreamTerminalAuthority> authority =
      std::make_shared<StreamTerminalAuthority>());

  StreamProviderLifecycleState state() const;
  const std::shared_ptr<StreamTerminalAuthority>& terminalAuthority() const;
  bool isTerminal() const;

  void activate();
  void beginEnding();
  void finish();
  void fail();
  void cancel();
  void fence();

private:
  void requireState(StreamProviderLifecycleState expected) const;
  void adoptExistingTerminal();

private:
  mutable std::mutex mutex_;
  StreamProviderLifecycleState state_ = StreamProviderLifecycleState::Prepared;
  std::shared_ptr<StreamTerminalAuthority> authority_;
};

/**
 * Per-invocation owner shared by the user and provider views in one runtime.
 * The owner deliberately contains one terminal authority and never allocates
 * a second request identifier. A deferred collaboration attaches this owner
 * to its already-created PendingCall; it is not a second collaboration.
 */
class StreamInvocationLifecycle
{
public:
  explicit StreamInvocationLifecycle(bool deferredCollaboration = false)
    : authority_(std::make_shared<StreamTerminalAuthority>()),
      user_(authority_),
      provider_(authority_),
      deferredCollaboration_(deferredCollaboration)
  {
  }

  StreamUserLifecycle& user() { return user_; }
  const StreamUserLifecycle& user() const { return user_; }
  StreamProviderLifecycle& provider() { return provider_; }
  const StreamProviderLifecycle& provider() const { return provider_; }
  const std::shared_ptr<StreamTerminalAuthority>& terminalAuthority() const
  {
    return authority_;
  }
  bool isDeferredCollaboration() const { return deferredCollaboration_; }

private:
  std::shared_ptr<StreamTerminalAuthority> authority_;
  StreamUserLifecycle user_;
  StreamProviderLifecycle provider_;
  bool deferredCollaboration_ = false;
};

/**
 * Bounded queue primitive shared by publisher, callback, and reorder paths.
 * It never allocates beyond the signed capacity.  waitPush propagates a
 * deadline as false rather than silently dropping an event.
 */
template<typename T>
class BoundedStreamQueue
{
public:
  explicit BoundedStreamQueue(size_t capacity)
    : capacity_(capacity)
  {
    if (capacity_ == 0) {
      throw std::invalid_argument("stream queue capacity must be positive");
    }
  }

  BoundedStreamQueue(const BoundedStreamQueue&) = delete;
  BoundedStreamQueue& operator=(const BoundedStreamQueue&) = delete;

  bool tryPush(T value)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || queue_.size() >= capacity_) {
      return false;
    }
    queue_.push_back(std::move(value));
    highWater_ = std::max(highWater_, queue_.size());
    notEmpty_.notify_one();
    return true;
  }

  bool waitPush(T value, std::chrono::steady_clock::time_point deadline)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!notFull_.wait_until(lock, deadline, [this] {
          return closed_ || queue_.size() < capacity_;
        })) {
      return false;
    }
    if (closed_) {
      return false;
    }
    queue_.push_back(std::move(value));
    highWater_ = std::max(highWater_, queue_.size());
    notEmpty_.notify_one();
    return true;
  }

  bool tryPop(T& value)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    value = std::move(queue_.front());
    queue_.pop_front();
    notFull_.notify_one();
    return true;
  }

  bool waitPop(T& value, std::chrono::steady_clock::time_point deadline)
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!notEmpty_.wait_until(lock, deadline, [this] {
          return closed_ || !queue_.empty();
        }) || queue_.empty()) {
      return false;
    }
    value = std::move(queue_.front());
    queue_.pop_front();
    notFull_.notify_one();
    return true;
  }

  void close()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    notEmpty_.notify_all();
    notFull_.notify_all();
  }

  bool isClosed() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  size_t capacity() const { return capacity_; }

  size_t highWaterMark() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return highWater_;
  }

private:
  const size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable notEmpty_;
  std::condition_variable notFull_;
  std::deque<T> queue_;
  size_t highWater_ = 0;
  bool closed_ = false;
};

struct StreamBinding
{
  ndn::Name requestId;
  ndn::Name requester;
  ndn::Name serviceName;
  ndn::Name producer;
  std::string producerBootId;
  uint64_t attemptEpoch = 0;
  StreamDigest planDigest{};
  StreamGenerationId generationId{};
  uint64_t streamEpoch = 0;
  StreamDigest eventKeyCommitment{};
  ndn::Buffer userToken;
  uint64_t policyEpoch = 0;
  uint64_t deadlineEpochMs = 0;

  void validate() const;
  ndn::Buffer canonicalBytes() const;
  bool operator==(const StreamBinding& other) const;
};

struct ParsedInvocationEventName
{
  ndn::Name producer;
  ndn::Name requester;
  ndn::Name serviceName;
  ndn::Name requestId;
  uint64_t attemptEpoch = 0;
  StreamDigest planDigest{};
  StreamGenerationId generationId{};
  uint64_t streamEpoch = 0;
  uint64_t cursor = 0;
};

StreamDigest initialStreamTranscriptDigest();

/**
 * Immutable signed Data produced for one streamed event.  The retained wire
 * is the retry identity: republication never re-encrypts or re-signs it.
 */
struct PublishedStreamEvent
{
  ndn::Name name;
  uint64_t cursor = 0;
  bool terminal = false;
  ndn::Buffer signedWire;
};

class ResponseMessage;

/**
 * Provider-side core writer.  It deliberately knows only the generic stream
 * contract; ServiceProvider supplies the final publication callback.  The
 * writer owns admission/cursor/transcript ordering, event encryption/signing,
 * and bounded exact-name retention so transport retries cannot create a new
 * event identity.
 */
class StreamEventPublisher
{
public:
  using PublishCallback = std::function<void(const PublishedStreamEvent&)>;

  StreamEventPublisher(StreamBinding binding,
                       StreamRequestOptions options,
                       ndn::Buffer eventKey,
                       std::shared_ptr<StreamInvocationLifecycle> lifecycle,
                       ndn::KeyChain& keyChain,
                       ndn::security::SigningInfo signingInfo,
                       PublishCallback publish);

  void start();

  std::optional<PublishedStreamEvent>
  publish(const ndn::Buffer& payload,
          std::chrono::steady_clock::time_point deadline);

  std::optional<StreamCompletion>
  finish(const ndn::Buffer& finalResult,
         StreamFinishReason reason,
         std::chrono::steady_clock::time_point deadline);

  bool fail(const std::string& errorInfo);

  /** Return the exact retained Data wire, without invoking publication. */
  std::optional<PublishedStreamEvent> satisfy(const ndn::Interest& interest);

  /** Republish the exact retained Data wire for an exact-name retry. */
  std::optional<PublishedStreamEvent> republish(const ndn::Name& name);

  size_t retainedCount() const;
  size_t highWaterMark() const;
  uint64_t nextCursor() const;
  const StreamDigest& transcriptDigest() const;

private:
  struct Retained
  {
    PublishedStreamEvent event;
    std::chrono::steady_clock::time_point expiresAt;
  };

  std::optional<PublishedStreamEvent>
  makeAndRetain(InvocationEventMessage event,
                std::chrono::steady_clock::time_point deadline);
  std::optional<PublishedStreamEvent>
  encodeAndSign(const InvocationEventMessage& event,
                uint64_t cursor) const;
  void reapExpiredLocked(std::chrono::steady_clock::time_point now);
  void emit(const PublishedStreamEvent& event) const;
  void releaseAdmission();
  void failProviderLocked();

private:
  StreamBinding binding_;
  StreamRequestOptions options_;
  ndn::Buffer eventKey_;
  std::shared_ptr<StreamInvocationLifecycle> lifecycle_;
  ndn::KeyChain& keyChain_;
  ndn::security::SigningInfo signingInfo_;
  PublishCallback publish_;
  mutable std::mutex mutex_;
  BoundedStreamQueue<InvocationEventMessage> admissionQueue_;
  std::map<std::string, Retained> retained_;
  uint64_t nextCursor_ = 1;
  uint64_t applicationEventCount_ = 0;
  StreamDigest transcriptDigest_ = initialStreamTranscriptDigest();
  bool started_ = false;
  bool finished_ = false;
  bool failed_ = false;
};

/**
 * User-side exact Data consumer.  It validates the signed event name and
 * binding before decryption, buffers only within the signed reorder bound,
 * and delivers each application event once.  Response completion is accepted
 * only after the observed End has closed the cursor.
 */
class StreamEventConsumer
{
public:
  using VerifyCallback = std::function<bool(const ndn::Data&)>;
  using EventCallback = std::function<void(const InvocationEventMessage&)>;
  using CompletionCallback = std::function<void(const ResponseMessage&)>;
  using ErrorCallback = std::function<void(const StreamedInvocationError&)>;
  using RetryCallback = std::function<void(const ndn::Name&)>;
  // Called only when the bounded retry state machine consumes retry budget.
  // Initial exact-interest prefetches use RetryCallback but must not be
  // reported as retransmissions.
  using RetryAccountingCallback = std::function<void(const ndn::Name&)>;

  StreamEventConsumer(StreamBinding binding,
                      StreamRequestOptions options,
                      ndn::Buffer eventKey,
                      std::shared_ptr<StreamInvocationLifecycle> lifecycle,
                      VerifyCallback verify,
                      EventCallback onEvent,
                      CompletionCallback onComplete,
                      ErrorCallback onError,
                      RetryCallback onRetry,
                      RetryAccountingCallback onRetryAccounting = {});

  ~StreamEventConsumer();

  void start();
  /**
   * Express the initial bounded exact-name window without consuming the
   * retry budget.  The caller wires the callback to the transport after the
   * consumer is installed, so events published immediately after Selection
   * cannot be lost before the first inactivity timer fires.
   */
  void prefetchWindow();
  bool accept(const ndn::Data& data);
  bool acceptResponse(const ResponseMessage& response);
  void onInactivityTimeout(std::chrono::steady_clock::time_point now);
  void onRetryTimeout(const ndn::Name& timedOutName,
                      std::chrono::steady_clock::time_point now);
  void reject(StreamedInvocationErrorCode code, const std::string& message);

  uint64_t expectedCursor() const;
  size_t reorderSize() const;
  uint8_t retryCount() const;
  size_t callbackHighWaterMark() const;

private:
  bool decodeAndValidate(const ndn::Data& data, InvocationEventMessage& event,
                         StreamedInvocationErrorCode& code,
                         std::string& message) const;
  bool bufferOrDeliver(InvocationEventMessage event);
  bool deliverReady();
  void fail(StreamedInvocationErrorCode code, const std::string& message);
  void requestGapRetry();
  bool dispatchApplicationCallback(const InvocationEventMessage& event,
                                   StreamedInvocationErrorCode& code,
                                   std::string& message);
  void callbackWorkerLoop();
  void stopCallbackWorker();

  struct CallbackTask
  {
    InvocationEventMessage event;
    std::shared_ptr<std::promise<void>> completion;
  };

private:
  StreamBinding binding_;
  StreamRequestOptions options_;
  ndn::Buffer eventKey_;
  std::shared_ptr<StreamInvocationLifecycle> lifecycle_;
  VerifyCallback verify_;
  EventCallback onEvent_;
  CompletionCallback onComplete_;
  ErrorCallback onError_;
  RetryCallback onRetry_;
  RetryAccountingCallback onRetryAccounting_;
  BoundedStreamQueue<CallbackTask> callbackQueue_;
  std::thread callbackThread_;
  mutable std::mutex mutex_;
  std::map<uint64_t, InvocationEventMessage> reorder_;
  // Exact names issued by the initial bounded window.  A future event must
  // not trigger a duplicate retry while its initial Interest is still in
  // flight; the initial timeout is the first retry trigger for that cursor.
  std::set<uint64_t> initialInterestCursors_;
  std::set<uint64_t> timedOutCursors_;
  std::optional<uint64_t> retryInFlightCursor_;
  std::optional<InvocationEventMessage> observedEnd_;
  ndn::Name observedEndName_;
  // A valid encrypted Response may arrive before the exact End Data (most
  // visible for a zero-event stream).  Retain that Response until End closes
  // the cursor instead of rejecting it and leaking the invocation.
  std::shared_ptr<ResponseMessage> pendingResponse_;
  uint64_t expectedCursor_ = 1;
  uint8_t retryCount_ = 0;
  std::chrono::steady_clock::time_point nextRetryAt_{};
  bool started_ = false;
  bool prefetchStarted_ = false;
  bool failed_ = false;
  bool complete_ = false;
};

StreamDigest computeStreamSha256(ndn::span<const uint8_t> bytes);
StreamDigest computeStreamBindingDigest(const StreamBinding& binding);
/** Digest for the pre-Selection portion of a streamed key grant binding. */
StreamDigest computeStreamGrantBindingDigest(const StreamBinding& binding);
StreamDigest initialStreamTranscriptDigest();
StreamDigest advanceStreamTranscriptDigest(
  const StreamDigest& previous, uint64_t cursor, InvocationEventType eventType,
  const StreamDigest& payloadDigest);
StreamNonce deriveInvocationEventNonce(
  const StreamDigest& eventKey, const StreamDigest& bindingDigest, uint64_t cursor);
ndn::Buffer makeInvocationEventAssociatedData(
  const ndn::Name& dataName, const StreamDigest& bindingDigest, uint64_t cursor);
bool isMatchingStreamCompletion(
  const StreamCompletion& completion, const ndn::Name& observedEndEventName,
  const InvocationEventMessage& observedEndEvent,
  ndn::span<const uint8_t> finalResponsePayload);

ndn::Name makeInvocationEventName(const StreamBinding& binding, uint64_t cursor);
std::optional<ParsedInvocationEventName>
parseInvocationEventName(const ndn::Name& name);

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_INVOCATION_STREAM_HPP
