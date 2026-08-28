# Wire Protocol Contract: Invocation Events V1

## 1. TLV assignments

These values are frozen for Spec 175. The T001 source census found that the
original draft range `0xF636..0xF657` is already owned by Stream mapping and FEC
types and that the current Stream enum extends through `0xF660`. The contract
therefore moves as one unit to the next contiguous free private range,
`0xF661..0xF684`. The executable contract gate verifies the complete source tree
before implementation and promotion; a later collision must never be resolved
silently in code alone.

| TLV | Value |
|---|---:|
| `StreamRequestOptionsType` | `0xF661` |
| `InvocationEventMessageType` | `0xF662` |
| `StreamCompletionType` | `0xF663` |
| `StreamGenerationIdType` | `0xF664` |
| `StreamEpochType` | `0xF665` |
| `StreamEventKeyGrantType` | `0xF666` |
| `StreamMaxEventsType` | `0xF667` |
| `StreamInterestWindowType` | `0xF668` |
| `StreamInterestLifetimeMsType` | `0xF669` |
| `StreamMaxEventRetriesType` | `0xF66A` |
| `StreamPublisherQueueType` | `0xF66B` |
| `StreamCallbackQueueType` | `0xF66C` |
| `StreamReorderCapacityType` | `0xF66D` |
| `StreamRetentionMsType` | `0xF66E` |
| `StreamCompletionGraceMsType` | `0xF66F` |
| `StreamMaxEventWireBytesType` | `0xF670` |
| `StreamAllowReplacementType` | `0xF671` |
| `StreamMaxReplacementsType` | `0xF672` |
| `StreamBindingDigestType` | `0xF673` |
| `StreamCursorType` | `0xF674` |
| `StreamEventTypeType` | `0xF675` |
| `StreamPayloadDigestType` | `0xF676` |
| `StreamPublishedAtUsType` | `0xF677` |
| `StreamTerminalType` | `0xF678` |
| `StreamEndEventType` | `0xF679` |
| `StreamFinalCursorType` | `0xF67A` |
| `StreamFinishReasonType` | `0xF67B` |
| `StreamApplicationEventCountType` | `0xF67C` |
| `StreamGeneratedTokenCountType` | `0xF67D` |
| `StreamTranscriptDigestType` | `0xF67E` |
| `StreamFinalResultDigestType` | `0xF67F` |
| `StreamEndEventNameType` | `0xF680` |
| `StreamInvocationModeType` | `0xF681` |
| `StreamEventKeyCommitmentType` | `0xF682` |
| `StreamDeadlineEpochMsType` | `0xF683` |
| `StreamAttemptEpochType` | `0xF684` |
| `ConversationContinuationType` | `0xF685` |
| `ConversationModeType` | `0xF686` |
| `ConversationIdType` | `0xF687` |
| `ConversationParentEpochType` | `0xF688` |
| `ConversationContextEpochType` | `0xF689` |
| `ConversationCheckpointType` | `0xF68A` |
| `ConversationCheckpointDigestType` | `0xF68B` |
| `ConversationTurnInputDigestType` | `0xF68C` |
| `ConversationAllowFallbackType` | `0xF68D` |
| `ConversationFallbackInputDigestType` | `0xF68E` |
| `ConversationRoleReceiptType` | `0xF68F` |
| `ConversationRoleReceiptDigestType` | `0xF690` |
| `ConversationPlanRoleMapDigestType` | `0xF691` |
| `ConversationStateResidencyType` | `0xF692` |
| `ConversationStateReadyType` | `0xF693` |
| `ConversationStateReasonType` | `0xF694` |
| `ConversationStateBytesType` | `0xF695` |
| `ConversationExpiresAtType` | `0xF696` |
| `ConversationCommitStatusType` | `0xF697` |
| `ConversationTurnCompletionType` | `0xF698` |

Existing `VersionType`, `PayloadType`, `UserTokenType`, `PolicyEpochType`, and
hybrid-envelope TLVs are reused only with their existing meanings.

A read-only source census on 2026-08-26 found no non-Spec use of
`0xF685..0xF698`. T029/T032 must add this range to the executable collision gate
and rerun it against the final source seal before implementation evidence is
accepted; a later collision requires a contract update, never an ad hoc code-
only reassignment.

## 2. StreamRequestOptions

Canonical field order:

```text
StreamRequestOptionsType
  VersionType                    = 1
  StreamInvocationModeType       = 0 Normal | 1 Targeted
  StreamGenerationIdType         = 16 opaque bytes
  StreamAttemptEpochType         = 1 initial | 2 sole replacement
  StreamEpochType                = nonzero uint64
  StreamEventKeyCommitmentType   = SHA-256(event key)
  StreamDeadlineEpochMsType      = absolute nonzero uint64 deadline
  StreamEventKeyGrantType        = optional; Targeted fast path only
  StreamMaxEventsType
  StreamInterestWindowType
  StreamInterestLifetimeMsType
  StreamMaxEventRetriesType
  StreamPublisherQueueType
  StreamCallbackQueueType
  StreamReorderCapacityType
  StreamRetentionMsType
  StreamCompletionGraceMsType
  StreamMaxEventWireBytesType
  StreamAllowReplacementType     = 0 | 1
  StreamMaxReplacementsType      = 0 | 1
```

All nonconditional fields are required when the block is present. The grant is
absent from every Normal, Targeted-bootstrap, and one-Provider fallback Request.
It is present only for a selection-free Targeted request and is an envelope
wrapped to that explicit Provider certificate; plaintext key bytes are never a
Request field. The block occurs at most once in `RequestMessage`, after the
existing request capabilities/input blocks and before final encoding. Request
decoding rejects missing, duplicate, out-of-order, unknown, noncanonical, or
out-of-range fields. An old peer that does not recognize the block must reject
the streamed request as unsupported; the sender must not retry it as unary.

The primary `Normal` API supplies only `serviceName` and uses the existing
service-scoped ACK/Selection path; no Provider vector is required. `Targeted`
mode requires exactly one explicit Provider. The runtime consumes an
existing valid one-time Targeted token/binding for the selection-free path. On a
cache miss, that same streamed invocation uses the existing
`TargetedBootstrapRequest` and one-Provider ACK/Selection flow; if another refill
is already in flight, it uses the current bounded one-Provider normal path.
`Normal` mode uses the existing ACK/Selection path. `StreamInvocationModeType`
records the caller's mode while the existing `RequestMode` records the effective
transport path. Neither mode may silently downgrade the invocation to unary.
Targeted mode rejects replacement options in version 1. A Normal replacement
is a distinct internal Request with attempt 2 and fresh ACK/Selection authority;
it never reuses an attempt-1 ProviderToken.

For Normal, Targeted-bootstrap, and one-Provider fallback paths, plan commit
adds `StreamEventKeyGrantType` only to the final-role Provider's existing
Provider-specific Selection projection. The envelope is wrapped to that
Provider certificate and binds the request ID, attempt, accepted plan, final
role/Provider identity, policy epoch, stream epoch, and
`StreamEventKeyCommitmentType`. Every other Provider projection omits it.

`StreamEventKeyGrantType` contains exactly one existing
`HybridMessageEnvelope`; after recipient-certificate decryption its plaintext is
exactly 32 bytes. The containing Provider-specific `ProviderGrant` supplies and
authenticates the request/attempt/plan/role/Provider/policy/expiry binding. The
Selection plan also carries the signed key commitment. A Targeted fast-path
Request uses its canonical Targeted binding in the same role. Extra plaintext,
multiple envelopes, an envelope outside these two authorized containers, or a
commitment mismatch is invalid.

## 2a. ConversationContinuationV1 and role-state control

The optional continuation block is an NDNSF-DI application extension carried
inside the encrypted application request. It is not a generic Core stream
option and is absent for existing request-scoped calls.

Canonical field order:

```text
ConversationContinuationType
  VersionType                         = 1
  ConversationIdType                  = 16 opaque bytes
  ConversationModeType                = 0 FULL_CONTEXT | 1 APPEND_DELTA
  ConversationParentEpochType         = optional uint64; required for APPEND_DELTA
  ConversationCheckpointType          = optional canonical authenticated bytes; required for APPEND_DELTA
  ConversationTurnInputDigestType     = SHA-256(canonical turn input)
  ConversationAllowFallbackType       = 0 | 1
  ConversationFallbackInputDigestType = optional SHA-256(full input); required iff fallback=1
```

The complete block is included in the request-contract digest already bound to
the accepted plan and every Provider Selection projection. The full fallback
input remains in the encrypted application input; the outer block carries only
its digest. Duplicate, unknown, out-of-order, noncanonical, or conditionally
missing fields are rejected before ACK admission.

ACKs may include one signed advisory state hint containing the checkpoint
digest, exact role, residency enum (`GPU_RESIDENT`, `HOST_RESIDENT`,
`PREFETCHING`, or `NONE`), logical bytes, and expiry. It is a placement input,
not proof of reuse. The caller does not receive or supply an endpoint list.
Selection for an `APPEND_DELTA` turn carries, inside each Provider-specific
projection, the aggregate checkpoint digest plus only that Provider-role's
receipt digest and expected parent epoch. No Provider receives another role's
private receipt bytes or state location.

Every conversation Selection, including the first `FULL_CONTEXT` turn, also
carries one canonical `ConversationTurnBindingV1` in its authenticated V3
projection:

```text
conversation_turn_binding
  schema                    = ndnsf-di-conversation-turn-binding-v1
  version                   = 1
  conversation_id
  parent_context_epoch
  successor_context_epoch   = parent_context_epoch + 1
  service_name
  plan_role_map_digest
  request_contract_digest
  retention_deadline_ms
  parent_checkpoint_digest  = empty only for parent epoch 0
```

The complete role map is committed by digest; it is not exposed as a public
caller-selected Provider list. `retention_deadline_ms` is an upper bound on the
successor state, not a promise that every Provider retains state until that
instant. A later-turn projection must also contain exactly one role-local
`ConversationStateReferenceV1`; the native Provider rejects missing,
noncanonical, expired, wrong-service, wrong-request, wrong-role-map, or
reference/binding-disagreeing input before any conversation-state lookup or
runner call.

After Selection and before model execution, every selected Provider returns a
signed and requester-encrypted `ConversationStateReadyV1` control Data binding
the new request/attempt/plan/generation, conversation/checkpoint/parent epoch,
role/Provider/boot/cache identity, exact receipt digest, residency, ready bit,
reason, and state bytes. The coordinator enters delta prefill only after all
roles report compatible ready state. A miss initiates only the explicit
full-prefill fallback or terminal failure.

On successful turn completion, each role publishes a signed and requester-
encrypted `ProviderConversationStateReceiptV1` under the exact name:

```text
/<provider>/NDNSF/DI/CONVERSATION-RECEIPT/
  <requester-uri-component>/<service...>/<requestId>/<attemptEpoch>/
  <planDigest>/<generationId>/<role>/<successorContextEpoch>
```

The receipt canonical fields are those frozen in `data-model.md` Section 10e.
Receipt publication is permitted only after the Provider has finalized its
request-local state to the exact canonical completed-turn prefix. Any
`CHECKPOINT_FINALIZE` input is authenticated internal pipeline data and produces
no external token event, cursor, End, or Response. The receipt commits the
finalized prefix and the origin request, but it never makes that request's
one-time tokens or request-local cache key reusable.
The coordinator fetches and validates the complete selected-role set, performs
the parent-epoch compare-and-swap, and locally returns one authenticated
aggregate `ConversationCheckpointV1`. The checkpoint is opaque to application
code. It contains receipt commitments but never state tensors, pointers,
filesystem paths, Provider tokens, or raw local cache names.

The terminal Response's encrypted NDNSF-DI application result may append this
separate nested block after its model result; the Core `StreamCompletionV1`
wire remains unchanged:

```text
ConversationTurnCompletionType
  VersionType                          = 1
  ConversationIdType
  ConversationParentEpochType
  ConversationContextEpochType
  ConversationPlanRoleMapDigestType
  ConversationRoleReceiptDigestType   # terminal role's own committed receipt
  ConversationCommitStatusType        = ROLE_STATE_COMMITTED
```

These fields prove only what the terminal role observed and committed. The
terminal role does not aggregate other Providers' receipts. The coordinator
fetches every exact receipt independently, returns a checkpoint only after the
complete role-set compare-and-swap succeeds, and withholds application-level
result/checkpoint resolution until then. A failed or incomplete aggregate
commit returns no successor checkpoint and leaves the parent usable.

## 3. Exact event Data name

One line grammar:

```text
EventName = Producer / "NDNSF" / "EVENT" /
            requester-uri-component / ServiceNameComponents / RequestId /
            AttemptNumber / PlanDigestHex / GenerationIdHex /
            StreamEpochNumber / CursorNumber
```

Rules:

- `Producer` is the selected final-role Provider identity prefix.
- `requester-uri-component` uses the same canonical escaped requester component
  convention as current ACK/Response V2 names.
- `ServiceNameComponents` is the unified service name without a split function.
- `RequestId` is the current request ID's terminal component, not a newly
  generated event request.
- `AttemptNumber`, `StreamEpochNumber`, and `CursorNumber` are canonical
  nonnegative-number components; attempt is exactly 1 or 2 and epoch/cursor are
  nonzero.
- `PlanDigestHex` is 64 lowercase hexadecimal characters. A cached-token
  selection-free Targeted request uses SHA-256 of the canonical Targeted binding
  `(provider, service, requestId, providerTokenDigest, policyEpoch)`. Normal,
  Targeted-bootstrap, and bounded one-Provider fallback requests use the
  accepted Selection plan digest.
- `GenerationIdHex` is exactly 32 lowercase hexadecimal characters representing
  the 16 request bytes.
- No extra suffix, segment, version, implicit digest, or ParametersSha256Digest
  component is accepted.

## 4. InvocationEventMessage

Canonical field order:

```text
InvocationEventMessageType
  VersionType                 = 1
  StreamBindingDigestType     = 32 bytes
  StreamCursorType            = uint64
  StreamEventTypeType         = 1 Application | 2 End
  PayloadType                 = opaque bytes (required, may be empty for End)
  StreamPayloadDigestType     = SHA-256(PayloadType value)
  StreamPublishedAtUsType     = uint64
  UserTokenType               = exact request UserToken
  StreamTerminalType          = 0 | 1
  StreamEndEventType          = present iff terminal=1
```

`StreamEndEventType` contains, in order:

```text
  StreamFinalCursorType
  StreamFinishReasonType
  StreamApplicationEventCountType
  StreamGeneratedTokenCountType
  StreamTranscriptDigestType
  StreamFinalResultDigestType
  ErrorInfoType                # required, empty on success
```

The encoded plaintext block must not exceed `maxEventWireBytes`. The encrypted
Data content contains one existing `HybridMessageEnvelope` whose plaintext is
the canonical `InvocationEventMessage` wire.

## 5. StreamCompletion in ResponseMessage

Canonical field order:

```text
StreamCompletionType
  VersionType                 = 1
  StreamBindingDigestType
  StreamEndEventNameType
  StreamFinalCursorType
  StreamFinishReasonType
  StreamApplicationEventCountType
  StreamTranscriptDigestType
  StreamFinalResultDigestType
```

The block is absent on unary responses and required on a streamed response. It
occurs exactly once after the existing payload and before Response encoding
finishes. It is protected and signed through the unchanged Response path.

## 6. Stream binding digest

Construct the `StreamBindingV1` canonical byte string by concatenating fields in
this order:

```text
requestId, requester, serviceName, producer, producerBootId,
attemptEpoch, planDigest, generationId, streamEpoch,
eventKeyCommitment, SHA256(userToken), policyEpoch, deadlineEpochMs
```

Then:

```text
bindingDigest = SHA256("NDNSF-STREAM-BINDING-V1" || canonicalBlock)
```

Each Name is its complete canonical NDN Name TLV wire and is therefore
self-delimiting. `producerBootId` is `uint64be(byteLength) || UTF-8 bytes`.
Numbers are exactly 8-byte big-endian, digests are exactly 32 bytes, and the
generation ID is exactly 16 bytes. The fields after `producerBootId` are fixed
width, and `SHA256(userToken)` replaces the variable-length token itself. There
is no enclosing TLV, JSON, separator text, NUL terminator, or platform-native
integer encoding in this digest. The hash input is the ASCII domain string
shown above followed immediately by this canonical byte string.

## 7. Encryption and nonce

The user generates the event key before Request publication but exposes only
`SHA256(eventKey)` in a Normal Request. After plan commit, only the final-role
Provider receives `StreamEventKeyGrantType` inside its Provider-specific
Selection projection and unwraps it with the certificate advertised and
validated through the existing ACK/Selection security path. Unselected ACK
Providers and nonfinal selected Providers receive no decryptable event grant. A
selection-free Targeted Request may contain the envelope because the single
target and its certificate are already fixed; another service-authorized
Provider still cannot unwrap it. No plaintext key, wrapped envelope, or unwrap
material appears in an outer name, log, or evidence manifest.

- Algorithm: AES-256-GCM.
- Key: exact 32 bytes whose SHA-256 equals the signed commitment and whose grant
  matches the accepted final Provider/binding.
- Nonce:

```text
H = HMAC-SHA256(eventKey,
                "NDNSF-EVENT-NONCE-V1" || bindingDigest || uint64be(cursor))
nonce = H[0:12]
```

- Associated data:

```text
DataName.wireEncode() ||
uint64be(InvocationEventMessageType) ||
bindingDigest || uint64be(cursor)
```

- A key is used for one `(bindingDigest, streamEpoch)` only. Cursor is never
  reused under that key. Replacement generates a new key and epoch.
- The final Provider signs the complete Data packet with its configured ECDSA
  signing certificate; event encryption credentials are not signing keys.

## 8. Consumer validation order

The consumer performs, in order:

1. pending exact Interest and Data name equality;
2. selected producer identity and trust-schema signature validation;
3. attempt/plan/generation/stream epoch/cursor name parsing and bounds;
4. selected-Provider grant/commitment and event key lookup for the exact binding;
5. AEAD authentication/decryption with exact associated data;
6. canonical TLV decode and wire-size bound;
7. binding digest, cursor, user token, payload digest, and terminal-shape checks;
8. stale/cancelled/deadline fence;
9. duplicate/reorder handling;
10. ordered application delivery.

Failure at steps 1-8 never inserts data into the reorder buffer or advances the
cursor.

## 9. Publisher atomicity

For cursor `c` the provider performs:

```text
serialize -> size check -> transcript candidate -> queue admission ->
cursor/token epoch commit -> encrypt/sign -> retain -> satisfy/publish
```

If any action before commit fails, cursor `c` remains unused. If encryption,
signing, retention, or publication fails after commit, the invocation fails and
does not issue cursor `c+1`; it does not renumber or skip.

## 10. Completion acceptance

Success requires all of:

- valid End at `finalCursor`;
- all Application cursors before End delivered in order;
- local application count and transcript digest equal End;
- valid final `ResponseMessage` and `StreamCompletion`;
- End name, binding, cursor, reason, counts, transcript digest, and final result
  digest equal Response completion;
- SHA-256 of Response application payload equals final result digest;
- one terminal claim and no active cancellation/deadline fence.

Any disagreement is `TERMINAL_MISMATCH` or `TRANSCRIPT_MISMATCH`, not partial
success.
