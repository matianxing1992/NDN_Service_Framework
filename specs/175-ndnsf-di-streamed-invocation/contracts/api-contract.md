# API Contract: Request-Scoped Streamed Invocation V1 and Conversation Continuation V1

## Compatibility rule

No existing unary, Targeted, media-stream, or collaboration signature is removed
or changes behavior. Additive overloads/options preserve source compatibility.
All new behavior is opt-in by calling the new API or by carrying
`StreamRequestOptions` in a low-level `RequestMessage`. Normal streamed
invocation remains service-scoped and does not require a Provider list.

## KV-cache scope terminology

This contract uses two non-interchangeable cache scopes:

- **Request-local KV-cache** is the Provider-local decode state used only by
  the prefill/decode epochs of one Request (and its authorized attempt). Its
  key includes the Request/generation/attempt lineage and it is released at
  terminal cleanup unless an explicit conversation promotion owns the
  finalized candidate.
- **Conversation-scoped KV-cache** is Provider-local state retained after a
  successful turn for a later Request in the same authenticated conversation.
  It is looked up only through a valid `ConversationCheckpointV1`, context
  epoch, exact role/Provider placement, and exact prefix extension. It creates
  fresh Request and generation authority; it never reuses the old Request's
  tokens or cache key.

The two stores, counters, residency, quotas, and cleanup events MUST be
reported separately. Prompt equality, `conversationId` alone, or a request
cache-hit counter around full-prefix execution is not conversation reuse.

The normal multi-turn call sequence is explicit:

```text
R1: FULL_CONTEXT(input=complete first turn)
    -> request-local KV/recurrent/convolution state
    -> terminal finalization + all-role promotion
    -> opaque checkpoint C(epoch=1)

R2: APPEND_DELTA(input=new turn, parent_checkpoint=C, epoch=1)
    -> fresh request/generation authority
    -> Provider conversation-state restore
    -> suffix-only prefill + request-local state for R2
    -> opaque checkpoint C(epoch=2)
```

The checkpoint is the authorization and lineage proof for the second lookup;
it is not the cache bytes. If the Provider state is unavailable, the caller
must select the explicitly authorized full-context fallback or receive a
continuation error. Repeating the complete transcript is never counted as a
conversation-scoped cache hit.

The Provider-side continuation seam is explicit and request-bound:

```text
acquire_for_request(APPEND_DELTA, freshRequestId, role, receipt)
    -> exact ConversationStateEntryV1, GPU-ready and PINNED
    -> suffix-only prefill for the new Request
release_for_request(APPEND_DELTA, freshRequestId, receipt)
    -> unpin after the role has produced its request-local successor
```

`acquire_for_request` rejects `FULL_CONTEXT`, a missing or wrong parent epoch,
an origin Request ID, a different conversation/role, and an absent or expired
receipt before any model call. It may perform one bounded host-to-GPU prefetch,
but it never searches the request-local store. The returned conversation entry
contains the complete adapter-owned state; the checkpoint and receipt remain
commitment/authorization metadata only.

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

// maxEvents is the total cursor budget. The authenticated End event consumes
// one cursor, so at most maxEvents - 1 application events may be published.

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
  const ndn::Name& serviceName,
  const RequestT& request,
  const StreamedInvocationOptions& options,
  std::function<void(const EventT&)> onEvent,
  std::function<void(const ResponseT&)> onComplete,
  std::function<void(const StreamedInvocationError&)> onError,
  size_t strategy = ndn_service_framework::tlv::FirstResponding);

template<typename RequestT, typename EventT, typename ResponseT>
std::shared_ptr<StreamedInvocationHandle<EventT, ResponseT>>
ServiceUser::RequestServiceStreaming(
  const ndn::Name& provider,
  const ndn::Name& serviceName,
  const RequestT& request,
  const StreamedInvocationOptions& targetedOptions,
  std::function<void(const EventT&)> onEvent,
  std::function<void(const ResponseT&)> onComplete,
  std::function<void(const StreamedInvocationError&)> onError);
```

### User-side invariants

- The service-only overload requires `Normal` mode and discovers willing
  Providers through the current Request/ACK path. The single-provider overload
  requires `Targeted` mode and reuses the current one-time token pool: a valid
  cached pair is consumed by the selection-free path; a cache miss makes that
  same invocation a `TargetedBootstrapRequest`; an already in-flight refill uses
  the current bounded one-Provider normal path. All three effective paths retain
  the same streamed lifecycle. No Provider-vector overload is the Normal
  NDNSF-DI default or valid evaluation subject.
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

# max_events counts the authenticated End cursor. Reserve N + 1 for N
# application payload events.

invocation = user.request_service_streaming(
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
    target_provider=None,
)
```

`target_provider` is keyword-only. It MUST be absent in Normal mode and MUST be
one nonempty Provider identity in Targeted mode.

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

## NDNSF-DI model/task-first streaming bridge

```python
invocation = automatic_coordinator.request_streaming(
    model=model_ref,
    task=task_ref,
    input=application_input,
    timeout_ms=120_000,
    generation_options=generation_options,
    stream_options=StreamedInvocationOptions(),
)
```

This application surface accepts no Provider list, role map, artifact split, or
deployment. It extends the existing deferred collaboration rather than calling
the ordinary service API after planning:

```text
same requestId:
  begin_collaboration(stream intent)
    -> ACK closure
    -> AutomaticPlanningCoordinator placement
    -> commit_collaboration_plan(stream binding + final-role key grant)
    -> Provider-specific Selection
    -> events / End / terminal Response
```

The returned Python invocation composes the existing automatic inference handle
with the Core-owned streamed cursor/result state. Its request ID is exactly the
initial collaboration request ID. Within a healthy attempt there is no second
Request, ACK collection, plan commit, or terminal owner. An opt-in replacement
is internal to this same logical handle and creates fresh attempt-2 authority as
defined by the generation-state contract. The low-level collaboration API
gains optional stream options/event callbacks but preserves its existing unary
behavior when those options are absent.

The streamed collaboration plan marks exactly one `CollaborationRoleSpec` as
`terminalResponseOwner`. That selected Provider owns Event/End/Response
closure. Other selected Providers may publish internal activation or feedback
data, but their handlers must complete their local role without claiming the
shared terminal. The ownership bit is part of the plan commit digest.

In the native DI adapter boundary, the terminal role receives a
`RoleExecutionContext::StreamEventSink` through the existing bounded Provider
worker. An incremental ONNX adapter may call the sink once per admitted token
event; the sink accepts serialized application bytes only and does not expose
cursor or terminal authority. The C++ `NativeModelRunner::runStreamed(...)`
hook is optional: the default runner returns no streamed result and the worker
retains the unary compatibility fallback, while the stateful ONNX adapter
implements one persistent decode epoch per token and returns only the final
role payload after all events are admitted. Non-terminal roles receive no
sink, and event bytes are never encoded as dependency `TensorBundle` output.
The Core writer still assigns cursors, signs/retains Data, and owns End/Response.
The direct tiny-ONNX adapter test proves the runner contract, but until the
same hook is driven by the native multi-Provider Request/ACK/Selection loop it
remains adapter evidence rather than a formal G2 case.

Provider-local exact-forward caching is disabled for any role execution that
has this sink. Cached TensorBundle outputs cannot replay the external event
side effect, so using them for a streamed terminal role would be an observable
transcript loss rather than a valid retry optimization.

## Optional NDNSF-DI conversation continuation API

Conversation continuation is an additive NDNSF-DI model/task feature. It does
not add conversation semantics to generic Core `RequestServiceStreaming`, and
it does not change the default request-scoped behavior.

```python
class ConversationInputMode(Enum):
    FULL_CONTEXT = "full-context"
    APPEND_DELTA = "append-delta"

@dataclass(frozen=True)
class ConversationContinuation:
    conversation_id: str
    mode: ConversationInputMode = ConversationInputMode.FULL_CONTEXT
    parent_checkpoint: bytes | None = None
    expected_parent_context_epoch: int | None = None
    allow_full_prefill_fallback: bool = False
    fallback_full_input: ApplicationInput | None = None

invocation = automatic_coordinator.request_streaming(
    model=model_ref,
    task=task_ref,
    input=turn_input,
    timeout_ms=120_000,
    generation_options=generation_options,
    stream_options=StreamedInvocationOptions(),
    conversation=ConversationContinuation(...),  # optional
    canonical_token_ids=adapter_token_ids,        # required for APPEND_DELTA
)

result = await invocation.result()
checkpoint = await invocation.conversation_checkpoint()
```

Rules:

- Omitting `conversation` is exactly the current full-context request path.
- When `conversation` is supplied, `APPClient` registers the fresh turn before
  publishing the Request and binds the same request/generation identities into
  the returned handle. If planning or publication fails, that pending turn is
  aborted and no conversation checkpoint is exposed.
- The first conversation turn uses `FULL_CONTEXT`, no parent checkpoint, and no
  parent epoch. `APPEND_DELTA` requires both a parent checkpoint and its expected
  context epoch.
- `canonical_token_ids` is adapter-derived metadata used only to prove the
  complete-transcript prefix and delta suffix. It is not a Provider list, cache
  address, state tensor, or wire payload; omitting it for `APPEND_DELTA` is
  rejected before Request publication.
- `conversation_id` is application-generated high-entropy identity, not a cache
  key supplied to ONNX Runtime and not an authorization capability.
- `APPClient` persists the committed structured messages and canonical token
  IDs in its envelope-key-protected `RuntimeJournal`. The application does not
  resend that prefix on a healthy delta turn. A restarted client may resume only
  when the journal record and checkpoint validate together; otherwise it takes
  the explicit full-context fallback/failure path.
- `input` is the complete input for `FULL_CONTEXT` and only the appended input
  for `APPEND_DELTA`. If fallback is allowed, `fallback_full_input` carries the
  complete authenticated transcript input; a digest of both inputs is sealed
  into the request contract.
- The application-level appended input is normally only the new message. The
  sealed tokenizer/chat-template adapter derives the exact appended canonical
  token suffix, which may also contain role delimiters or control tokens. Delta
  prefill processes that suffix and reports its size separately from the raw
  new-message token count; it never reprocesses the parent prefix on a claimed
  continuation hit.
- For a conversation-enabled turn, `result()` and
  `conversation_checkpoint()` become application-visible together only after
  successful End/Response and transactional all-role checkpoint commit. Core
  may have validated terminal packets earlier, but callbacks/futures remain
  pending. The checkpoint call returns opaque authenticated bytes. On commit
  failure both raise the registered conversation error, the terminal payload is
  retained only as diagnostic evidence, and the parent checkpoint remains the
  last usable state.
- The caller cannot supply a Provider, role map, state location, state tensor,
  or cache address through this API. Reuse requires the exact previous
  Provider-role map; otherwise the coordinator executes the registered full-
  context fallback or reports `ConversationStateUnavailable`.
- Only one successor of a parent epoch may commit. A competing turn fails with
  `ConversationStateConflict`; no implicit branch is created.
- Before delta prefill, the adapter applies the exact tokenizer/chat template to
  the complete new transcript and proves that the parent committed token IDs are
  an exact prefix. If not, the turn takes only the explicit full-context
  fallback/failure path.
- A checkpoint is not a handle to the preceding Request's decode cache. At
  successful conversation completion, the runtime first makes every role's
  request-local state represent the exact completed-turn prefix, including a
  bounded state-only finalization suffix when necessary, then transactionally
  promotes ownership into conversation-scoped entries. Non-conversation calls
  release request-local state and cannot later be resumed.

The native DI-facing request adds the corresponding optional value object; it
does not modify the generic application-neutral streaming API:

```cpp
enum class ConversationInputModeV1 { FullContext, AppendDelta };

struct ConversationContinuationOptionsV1 {
  std::array<uint8_t, 16> conversationId;
  ConversationInputModeV1 mode = ConversationInputModeV1::FullContext;
  std::optional<ndn::Buffer> parentCheckpoint;
  std::optional<uint64_t> expectedParentContextEpoch;
  bool allowFullPrefillFallback = false;
  std::optional<ApplicationInput> fallbackFullInput;
};
```

The Python/native serializer computes the canonical input digests and request
binding; application code never constructs Provider receipts. ACK state-tier
hints are advisory only. The coordinator verifies the checkpoint and all role
receipts, commits the same one-to-one placement, and waits for every role's
state-ready result before admitting delta prefill.

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
  through the same function name's single-provider overload and common request
  options/provider binding used by Core.
- No multi-role DI path that calls `RequestServiceStreaming` after
  `CommitCollaborationPlan` within the same attempt; streamed DI extends the
  original collaboration. The sole opt-in recovery attempt is owned internally
  by `AutomaticPlanningCoordinator` and is not a second public API call.
- No legacy `Direct` alias.
- No public mapping/FEC configuration on request-scoped invocation events.
- No automatic replacement toggle hidden in environment variables.
- No Targeted replacement in protocol version 1.
- No raw KV-cache handle, state tensor, device pointer, local path, or Provider
  list in the conversation API.
- No conversation checkpoint that bypasses Request/ACK/Selection authorization.
- No live cross-Provider state migration, disk/NVMe state tier, implicit
  conversation branching/merging, or continuous batching in this version.
