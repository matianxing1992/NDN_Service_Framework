# Wire Protocol Contract: Invocation Events V1

## 1. TLV assignments

These values are frozen for Spec 175. Implementation first verifies that the
current enum still ends at `0xF635`. If concurrent work has created a collision,
the implementer allocates the next contiguous free private range and updates
this table, the contract gate, test vectors, and traceability together before
using the values; a collision must never be resolved silently in code alone.

| TLV | Value |
|---|---:|
| `StreamRequestOptionsType` | `0xF636` |
| `InvocationEventMessageType` | `0xF637` |
| `StreamCompletionType` | `0xF638` |
| `StreamGenerationIdType` | `0xF639` |
| `StreamEpochType` | `0xF63A` |
| `StreamEventKeyGrantType` | `0xF63B` |
| `StreamMaxEventsType` | `0xF63C` |
| `StreamInterestWindowType` | `0xF63D` |
| `StreamInterestLifetimeMsType` | `0xF63E` |
| `StreamMaxEventRetriesType` | `0xF63F` |
| `StreamPublisherQueueType` | `0xF640` |
| `StreamCallbackQueueType` | `0xF641` |
| `StreamReorderCapacityType` | `0xF642` |
| `StreamRetentionMsType` | `0xF643` |
| `StreamCompletionGraceMsType` | `0xF644` |
| `StreamMaxEventWireBytesType` | `0xF645` |
| `StreamAllowReplacementType` | `0xF646` |
| `StreamMaxReplacementsType` | `0xF647` |
| `StreamBindingDigestType` | `0xF648` |
| `StreamCursorType` | `0xF649` |
| `StreamEventTypeType` | `0xF64A` |
| `StreamPayloadDigestType` | `0xF64B` |
| `StreamPublishedAtUsType` | `0xF64C` |
| `StreamTerminalType` | `0xF64D` |
| `StreamEndEventType` | `0xF64E` |
| `StreamFinalCursorType` | `0xF64F` |
| `StreamFinishReasonType` | `0xF650` |
| `StreamApplicationEventCountType` | `0xF651` |
| `StreamGeneratedTokenCountType` | `0xF652` |
| `StreamTranscriptDigestType` | `0xF653` |
| `StreamFinalResultDigestType` | `0xF654` |
| `StreamEndEventNameType` | `0xF655` |
| `StreamInvocationModeType` | `0xF656` |

Existing `VersionType`, `PayloadType`, `UserTokenType`, `PolicyEpochType`, and
hybrid-envelope TLVs are reused only with their existing meanings.

## 2. StreamRequestOptions

Canonical field order:

```text
StreamRequestOptionsType
  VersionType                    = 1
  StreamInvocationModeType       = 0 Normal | 1 Targeted
  StreamGenerationIdType         = 16 opaque bytes
  StreamEpochType                = nonzero uint64
  StreamEventKeyGrantType        = protected 32-byte key grant envelope
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

All fields are required when the block is present. The block occurs at most once
in `RequestMessage`, after the existing request capabilities/input blocks and
before final encoding. Request decoding rejects missing, duplicate, out-of-order,
unknown, noncanonical, or out-of-range fields. An old peer that does not
recognize the block must reject the streamed request as unsupported; the sender
must not retry it as unary.

`Targeted` mode requires exactly one explicit Provider. The runtime consumes an
existing valid one-time Targeted token/binding for the selection-free path. On a
cache miss, that same streamed invocation uses the existing
`TargetedBootstrapRequest` and one-Provider ACK/Selection flow; if another refill
is already in flight, it uses the current bounded one-Provider normal path.
`Normal` mode uses the existing ACK/Selection path. `StreamInvocationModeType`
records the caller's mode while the existing `RequestMode` records the effective
transport path. Neither mode may silently downgrade the invocation to unary.

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
  nonnegative-number components; attempt may be 0, epoch/cursor may not.
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

Construct a `StreamBindingV1` canonical block with fields in this order:

```text
requestId, requester, serviceName, producer, producerBootId,
attemptEpoch, planDigest, generationId, streamEpoch,
SHA256(userToken), policyEpoch, deadlineEpochMs
```

Then:

```text
bindingDigest = SHA256("NDNSF-STREAM-BINDING-V1" || canonicalBlock)
```

Names are encoded as canonical NDN Name wire, numbers as 8-byte big-endian,
digests as 32 bytes, and generation ID as 16 bytes. There is no JSON or
platform-native integer encoding in this digest.

## 7. Encryption and nonce

The `StreamEventKeyGrantType` field is plaintext only inside the existing
ABE-protected `RequestMessage` content; it is never exposed in an outer name,
unprotected block, log, or evidence manifest. Consequently, its confidentiality
domain is the set of Providers authorized to decrypt that service request, not
only the Provider eventually selected. Event acceptance is narrower: only the
final Provider identity and boot epoch in the accepted binding may sign valid
event Data. A selected-Provider-only post-Selection grant protocol would be a
different wire contract and is out of scope for version 1.

- Algorithm: AES-256-GCM.
- Key: exact 32 bytes from the authorized event-key grant.
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
4. event key lookup for the exact binding;
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
