# API Contract: Request-Scoped Streamed Invocation V1

## Compatibility rule

No existing unary, Targeted, media-stream, or collaboration signature changes.
All new behavior is opt-in by calling the new API or by carrying
`StreamRequestOptions` in a low-level `RequestMessage`.

## C++ user API

```cpp
enum class InvocationMode
{
  Normal,
  Targeted,
};

struct StreamedInvocationOptions
{
  InvocationMode mode = InvocationMode::Normal;
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
};

enum class StreamedInvocationStatus
{
  Created,
  Requesting,
  Selecting,
  Streaming,
  Draining,
  Completed,
  Failed,
  Cancelled,
};

enum class StreamedInvocationErrorCode
{
  StreamUnsupported,
  InvalidOptions,
  Unauthorized,
  AckTimeout,
  SelectionFailed,
  EventTimeout,
  EventOutsideRetention,
  EventOversize,
  QueueDeadline,
  InvalidName,
  InvalidSignature,
  DecryptionFailed,
  BindingMismatch,
  CursorMismatch,
  TranscriptMismatch,
  TerminalMismatch,
  ProviderFailure,
  Deadline,
  Cancelled,
  ApplicationCallbackFailed,
  ReplacementUnavailable,
};

struct StreamedInvocationError
{
  StreamedInvocationErrorCode code;
  std::string message;       // bounded diagnostic, no plaintext payload
  ndn::Name requestId;
  uint64_t expectedCursor = 0;
};

template<typename EventT, typename ResponseT>
class StreamedInvocationHandle
{
public:
  const ndn::Name& requestId() const;
  StreamedInvocationStatus status() const;
  StreamedInvocationMetrics metrics() const;
  void cancel();

  StreamedInvocationHandle(const StreamedInvocationHandle&) = delete;
  StreamedInvocationHandle& operator=(const StreamedInvocationHandle&) = delete;
};

template<typename RequestT, typename EventT, typename ResponseT>
std::shared_ptr<StreamedInvocationHandle<EventT, ResponseT>>
ServiceUser::RequestServiceStreaming(
  const std::vector<ndn::Name>& providers,
  const ndn::Name& serviceName,
  const RequestT& request,
  const StreamedInvocationOptions& options,
  std::function<void(const EventT&)> onEvent,
  std::function<void(const ResponseT&)> onComplete,
  std::function<void(const StreamedInvocationError&)> onError,
  size_t strategy = ndn_service_framework::tlv::FirstResponding);
```

### User-side invariants

- A nonempty provider vector is required by the typed overload, matching the
  current explicit-provider dynamic API. `Targeted` requires exactly one
  Provider and reuses the current one-time token pool: a valid cached pair is
  consumed by the selection-free path; a cache miss makes that same invocation
  a `TargetedBootstrapRequest`; an already in-flight refill uses the current
  bounded one-Provider normal path. All three retain the streamed lifecycle.
- Exactly one of callback consumption or Python async-iterator consumption may
  claim the event cursor.
- `onComplete` is invoked once only after End, cursor closure, transcript digest,
  final-result digest, and `ResponseMessage` validation.
- `onError` is invoked at most once and never after `onComplete`.
- `cancel()` is idempotent. Before terminal state it fences future callbacks and
  publishes the existing cancellation/tombstone mechanism where applicable.
- Handle destruction does not silently cancel; request lifetime is owned by the
  ServiceUser shared state. Applications must call `cancel()` for early stop.

## C++ provider API

```cpp
enum class StreamFinishReason
{
  Eos,
  StopSequence,
  MaxTokens,
  ApplicationComplete,
};

template<typename EventT, typename ResponseT>
class StreamedResponseWriter
{
public:
  bool publish(const EventT& event);
  void finish(const ResponseT& response, StreamFinishReason reason);
  void fail(StreamedInvocationErrorCode code, std::string message);
  bool isCancelled() const;
  std::chrono::milliseconds remainingDeadline() const;

  StreamedResponseWriter(const StreamedResponseWriter&) = delete;
  StreamedResponseWriter& operator=(const StreamedResponseWriter&) = delete;
};

template<typename RequestT, typename EventT, typename ResponseT>
void ServiceProvider::addStreamingHandler(
  const ndn::Name& serviceName,
  std::function<void(const RequestT&,
                     StreamedResponseWriter<EventT, ResponseT>&)> handler);
```

### Provider-side invariants

- `publish` serializes before queue admission. Serialization, size, cancellation,
  or deadline failure returns `false` and leaves cursor/token epoch unchanged.
- A successful `publish` owns the cursor; later transport retries reuse the same
  signed Data bytes and name.
- `finish` admits one End event and publishes one complete Response. A second
  `finish` or `fail` throws `std::logic_error` and publishes nothing.
- `fail` transitions to the sole terminal failure and may publish a bounded End
  failure event plus failed Response when authorization remains valid.
- Writer methods invoked after handler return or terminal transition fail closed.
- Provider handler execution remains off the Face thread.

## NDNSF-DI provider bridge

The existing `ProviderRuntimeContext` gains only:

```python
def publish_event(self, payload: bytes, *, event_type: str = "application") -> int:
    """Publish one opaque event and return its committed 1-based cursor."""

def finish_stream(self, payload: bytes, *, finish_reason: str) -> None:
    """Publish End plus the sole complete terminal Response."""

def stream_cancelled(self) -> bool:
    """Return the current fenced/cancelled state."""
```

`finish_stream` delegates to the same terminal guard used by
`publish_final_response`. Calling both for one invocation is a duplicate terminal
claim and fails. Nonstreamed handlers continue to call
`publish_final_response(payload)` unchanged.

## Python user API

```python
@dataclass(frozen=True)
class StreamedInvocationOptions:
    mode: InvocationMode = InvocationMode.NORMAL
    max_events: int = 512
    interest_window: int = 16
    interest_lifetime_ms: int = 500
    max_event_retries: int = 3
    publisher_queue_capacity: int = 64
    callback_queue_capacity: int = 64
    reorder_capacity: int = 64
    retention_ms: int = 30000
    completion_grace_ms: int = 5000
    max_event_wire_bytes: int = 16384
    allow_replacement: bool = False
    max_replacements: int = 0

invocation = user.request_service_streaming(
    providers,
    service_name,
    request,
    *,
    options=StreamedInvocationOptions(),
    strategy=SelectionStrategy.FIRST_RESPONDING,
    event_decoder=None,
    response_decoder=None,
    on_event=None,
    on_complete=None,
    on_error=None,
)
```

The returned `StreamedInvocation` has:

```python
request_id: str
status: StreamedInvocationStatus
metrics: StreamedInvocationMetrics

def __aiter__(self) -> AsyncIterator[object]: ...
async def result(self) -> object: ...
async def cancel(self) -> None: ...
```

### Python rules

- `request` may be protobuf-compatible or exact bytes, using the current dynamic
  serializer rules.
- `event_decoder` and `response_decoder` receive verified opaque payload bytes.
- If no decoder is supplied, bytes are returned.
- Registering `on_event` and then entering `async for`, or the reverse, raises
  `RuntimeError("streamed invocation already has an event consumer")`.
- `result()` may be awaited before, during, or after event iteration; it resolves
  only after successful closure or raises `StreamedInvocationError`.
- `cancel()` is safe after completion and never waits beyond the local bounded
  cancellation fence.

## Python provider API

The public Python Provider exposes the same native writer used by the C++ API:

```python
def generate(request: bytes, writer: StreamedResponseWriter) -> None:
    if not writer.publish(b"event-1"):
        return
    writer.finish(b"complete-result", StreamFinishReason.APPLICATION_COMPLETE)

provider.add_streaming_handler("/service/name", generate)

# Authenticated context variant, matching the existing unary provider style.
provider.add_streaming_context_handler(
    "/service/name",
    lambda context, request, writer: generate(request, writer),
)
```

`StreamedResponseWriter` is a thin native binding with `publish(bytes)`,
`finish(bytes, reason)`, `fail(code, message)`, `is_cancelled`, and
`remaining_deadline_ms`. The handler runs synchronously on the existing Provider
worker pool, never the Face thread. Returning without a terminal call fails that
invocation as `ProviderFailure`; an exception is contained and fails it as
`ApplicationCallbackFailed`. The writer becomes invalid when the handler returns
or reaches terminal state. `provider.streaming_handler(service)` is the decorator
form of `add_streaming_handler`; no Python-owned cursor, queue, or publication
thread is permitted.

## Explicitly absent APIs

- No `RequestLlmStreaming` or framework `TokenEvent` API.
- No `RequestServiceStreamingTargeted` duplicate. Targeted behavior is selected
  through the common request options/provider binding used by Core.
- No legacy `Direct` alias.
- No public mapping/FEC configuration on request-scoped invocation events.
- No automatic replacement toggle hidden in environment variables.
