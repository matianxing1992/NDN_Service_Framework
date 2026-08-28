# Data Model: NDNSF-DI Streamed Invocation

## 1. StreamedInvocationOptionsV1

Signed request options. Values are immutable after Request publication.

| Field | Type | Required | Default / validation |
|---|---|---:|---|
| `version` | uint64 | yes | exactly 1 |
| `mode` | enum | yes | `Normal`; `Targeted` requires one Provider and existing cached/bootstrap/refill behavior |
| `generationId` | 128-bit opaque hex | yes | nonzero, user generated |
| `attemptEpoch` | uint64 | yes | 1 for the initial Request; 2 only for the one authorized Normal recovery Request |
| `streamEpoch` | uint64 | yes | nonzero, user generated |
| `eventKeyCommitment` | SHA-256 | yes | digest of one framework-generated 32-byte event key |
| `eventKeyGrant` | provider-wrapped bytes | conditional | absent for Normal/bootstrap/fallback Request; present only for selection-free Targeted and wrapped to its explicit target |
| `maxEvents` | uint32 | yes | 512; 1..4096 |
| `interestWindow` | uint16 | yes | 16; 1..64 |
| `interestLifetimeMs` | uint32 | yes | 500; 100..5000 |
| `maxEventRetries` | uint8 | yes | 3; 0..8 |
| `publisherQueueCapacity` | uint16 | yes | 64; 1..1024 |
| `callbackQueueCapacity` | uint16 | yes | 64; 1..1024 |
| `reorderCapacity` | uint16 | yes | 64; window..1024 |
| `retentionMs` | uint32 | yes | 30000; 1000..300000 |
| `completionGraceMs` | uint32 | yes | 5000; 0..30000 |
| `maxEventWireBytes` | uint32 | yes | 16384; 512..65536 |
| `allowReplacement` | bool | yes | false |
| `maxReplacements` | uint8 | yes | 0; exactly 1 only when replacement enabled |

Validation is atomic. Unknown critical fields, duplicated fields, invalid ranges,
or a replacement combination other than `(false,0)` or `(true,1)` reject the
request as `STREAM_OPTIONS_INVALID`. Targeted mode rejects `(true,1)` in this
feature. The generation ID remains stable across a replacement; request ID,
attempt epoch, stream epoch, key, ACK closure, and plan do not.

For `TOKEN_STREAMING`, the sealed V3 `generation_contract.generation_id` must
repeat this exact 128-bit value. It is not regenerated or derived from the
request/attempt by a Provider. A Provider may retain the legacy derived value
only when parsing an older diagnostic projection that omitted the field; live
conversation/readiness paths require the explicit value.

### GenerationRecoveryV1 and accepted-plan binding

Attempt 2 carries one immutable recovery record containing the stable logical
generation ID, original and recovery request IDs, original input-manifest
digest, prior plan digest, the absolute failed Provider identity, and the
committed token IDs/count/digest. The failed Provider comes from the validated
Core error binding; it is not inferred from the old placement.

`PlacementPlanCoreV3.requestContractDigest` is `SHA256` of the exact attempt-2
Request payload bytes. Every Provider Selection projection repeats this value.
The Provider recomputes it from the decrypted Request before assembly or model
execution. Changing the prompt, recovery record, prefix, or any other Request
byte therefore invalidates the accepted plan even when the remaining placement
fields are unchanged.

## 2. StreamBindingV1

Immutable lineage shared by request, event consumer, provider writer, End event,
and final Response.

| Field | Type | Rule |
|---|---|---|
| `requestId` | Name | exact V2 request ID for this attempt |
| `requester` | Name | identity that issued Request |
| `serviceName` | Name | unified service name |
| `producer` | Name | selected final-role Provider |
| `producerBootId` | digest/string | exact selected boot epoch |
| `attemptEpoch` | uint64 | 1 for the initial attempt; 2 for the sole replacement |
| `planDigest` | SHA-256 | accepted plan or Targeted binding digest |
| `generationId` | 128-bit opaque | from signed options |
| `streamEpoch` | uint64 | unique within attempt; changes on replacement |
| `eventKeyCommitment` | SHA-256 | exact signed Request commitment |
| `userToken` | bytes | exact request token echoed/verified |
| `policyEpoch` | uint64 | accepted authorization epoch |
| `deadlineEpochMs` | uint64 | immutable absolute deadline |

Equality is field-for-field. A partial match is a mismatch.

## 2a. EventKeyGrantV1

Logical validation record for one provider-specific encrypted envelope containing
the exact 32-byte event key. The identity/binding fields are authenticated by the
containing ProviderGrant or canonical Targeted binding; the wire envelope itself
reuses `HybridMessageEnvelope` rather than duplicating those fields.

| Field | Rule |
|---|---|
| `recipientProvider` | exact final-role Provider, or exact Targeted Provider on the selection-free path |
| `recipientCertificateDigest` | exact validated certificate advertised by that Provider |
| `requestId` / `attemptEpoch` | exact current invocation and attempt |
| `planDigest` | accepted plan or canonical Targeted binding digest |
| `producerRole` / `producer` | exact final role and Provider |
| `policyEpoch` / `streamEpoch` | exact accepted epochs |
| `eventKeyCommitment` | equals `SHA256(unwrappedKey)` and the signed Request commitment |
| `wrappedKey` | decrypts to exactly 32 bytes only for the recipient certificate |

For Normal, Targeted-bootstrap, and one-Provider fallback, this entity exists
only in the final Provider's Selection projection. Other selected Providers and
all unselected ACK Providers receive no envelope. For a selection-free Targeted
request it may be carried in the protected Request, but remains encrypted to the
one explicit target certificate. Plaintext key bytes are never serialized.

## 3. InvocationEventMessageV1

Canonical plaintext before stream-epoch encryption.

| Field | Type | Rule |
|---|---|---|
| `version` | uint64 | exactly 1 |
| `bindingDigest` | SHA-256 | digest of canonical `StreamBindingV1` |
| `cursor` | uint64 | 1..maxEvents, strictly monotonic at publisher |
| `eventType` | enum | `Application=1`, `End=2` |
| `payload` | bytes | opaque application event; empty only for End |
| `payloadDigest` | SHA-256 | digest of payload bytes |
| `publishedAtUs` | uint64 | evidence timestamp, not acceptance authority |
| `userToken` | bytes | exact token echoed from request |
| `terminal` | bool | true iff event type is End |
| `end` | EndEventV1 | present iff terminal |

`bindingDigest`, Data name, signer, AEAD associated data, cursor, and decoded
fields must agree before buffering. `publishedAtUs` is advisory and cannot
override lineage or deadline.

## 4. EndEventV1

| Field | Type | Rule |
|---|---|---|
| `finalCursor` | uint64 | equals containing event cursor |
| `finishReason` | enum | EOS, STOP_SEQUENCE, MAX_TOKENS, APPLICATION_COMPLETE, DEADLINE, CANCELLED, FAILED |
| `applicationEventCount` | uint64 | count excluding End |
| `generatedTokenCount` | uint64 | application-supplied; <= maxEvents-1 |
| `transcriptDigest` | SHA-256 | digest chain over accepted application events |
| `finalResultDigest` | SHA-256 | digest of complete final application payload |
| `errorCode` | enum/string | empty on successful reasons; bounded on failure |

Transcript digest chain:

```text
D0 = SHA256("NDNSF-STREAM-V1")
Di = SHA256(D(i-1) || cursor || eventType || payloadDigest)
```

The End event itself is not added to the application transcript chain.

## 5. StreamCompletionV1

Optional block in the authoritative `ResponseMessage`.

| Field | Type | Rule |
|---|---|---|
| `bindingDigest` | SHA-256 | exact accepted binding |
| `endEventName` | Name | exact full End Data name |
| `finalCursor` | uint64 | equals End |
| `finishReason` | enum | equals End |
| `applicationEventCount` | uint64 | equals End |
| `transcriptDigest` | SHA-256 | equals End and local consumer chain |
| `finalResultDigest` | SHA-256 | equals End and Response payload digest |

Successful application completion is delivered only after all equality checks
pass and cursors 1..`finalCursor` have been accepted in order.

## 6. StreamedInvocationState

User-side states:

```text
Created -> Requesting -> Selecting -> Streaming -> Draining -> Completed
                    \          \          \          \-> Failed
                     \          \          \-----------> Cancelled
                      \----------\----------------------> Failed
```

| State | Accepted actions |
|---|---|
| `Created` | serialize request and allocate binding seed |
| `Requesting` | collect/validate ACKs or Targeted binding |
| `Selecting` | accept one committed plan and final event producer |
| `Streaming` | fetch/buffer/deliver application events |
| `Draining` | End and/or Response seen; close remaining declared gaps |
| `Completed` | read result/metrics only |
| `Failed` | read terminal error/metrics only |
| `Cancelled` | read cancellation/metrics only |

All terminal states are absorbing. Cancellation is idempotent from a terminal
state and fencing from any nonterminal state.

For multi-role DI these states are a delivery view over the existing deferred
collaboration, not a second request state machine. `requestId` is allocated once
before `begin_collaboration`; ACK closure and `commit_collaboration_plan` advance
that same shared state into `Selecting/Streaming`, and End/Response compete for
the existing single terminal claim.

Provider writer states:

```text
Prepared -> Active -> Ending -> Finished
    \          \          \------> Fenced
     \----------\----------------> Failed
```

Cursor commitment occurs only after bounded queue admission. `finish` first
admits End, then claims/publishes the final Response exactly once.

## 7. StreamedInvocationMetrics

All counters are per invocation and bounded.

| Field | Meaning |
|---|---|
| `eventsPublished` | application plus End accepted by publisher queue |
| `eventsFetched` | valid event Data fetches including duplicates |
| `eventsDelivered` | application callbacks/iterator values only |
| `duplicateEvents` | valid duplicate cursor Data suppressed |
| `staleEvents` | lineage-failed Data |
| `gapRetries` | exact cursor Interest re-expressions |
| `maxPublisherQueueDepth` | high-water mark |
| `maxCallbackQueueDepth` | high-water mark |
| `maxReorderDepth` | high-water mark |
| `backpressureUs` | cumulative provider admission wait |
| `ttftUs` | Request publication to first application event delivery |
| `interEventUs` | bounded per-cursor intervals or summary histogram |
| `terminalLatencyUs` | Request publication to complete Response delivery |
| `finishReason` | accepted terminal reason |

## 8. GenerationTokenEventV1

Application payload; Core treats the bytes as opaque.

| Field | Type | Rule |
|---|---|---|
| `tokenId` | int64 | sampled vocabulary token |
| `tokenEpoch` | uint64 | 1-based generated-token count |
| `textDelta` | UTF-8 bytes/string | incremental decoder output, may be empty |
| `cumulativeTokenCount` | uint64 | equals tokenEpoch |
| `finishHint` | enum | NONE, EOS, STOP_SEQUENCE, MAX_TOKENS |
| `samplingDigest` | SHA-256 | sealed sampling configuration identity |

## 9. QwenGenerationSpecV2

Extends the current generation spec without changing plan ownership.

Required additions:

- tokenizer and adapter digests;
- exact chat-template digest and `thinkingMode=disabled`;
- sampling digest and canonical parameters;
- `decodeMode=single-token-autoregressive`, `modality=text-only`, and
  `mtpEnabled=false`;
- EOS token set and ordered stop strings;
- deterministic seed;
- accepted-token prefix digest/count;
- `StreamBindingV1` digest;
- prefill/decode runner and I/O-layout identities;
- explicit replacement permission and maximum;
- conversation-enabled `maxCheckpointFinalizeTokens=32`; the exact computed
  finalization suffix count is recorded at terminal promotion and may not
  exceed this sealed V1 cap;
- exact event and terminal-result contracts.

### Streamed collaboration terminal ownership

Each streamed `CollaborationRoleSpec` carries a planner-owned
`terminalResponseOwner` boolean. Exactly one selected role sets it. The owner
publishes the user-facing Event/End/terminal Response sequence; selected
non-owner roles complete local execution and release their Provider-side
pending state without publishing a terminal Response. The bit is included in
the deferred plan commit digest and is not an application-provided runtime
override.

## 10. DecodeStateIdentityV1 and DecodeStateBundleV1

`DecodeStateIdentityV1` binds the complete provider-local incremental state,
not only transformer KV tensors. `DecodeStateBundleV1` is an opaque adapter-owned
bundle whose schema digest is part of the identity. For Qwen3.6 the bundle
contains full-attention KV tensors and linear-attention convolution/recurrent
state; omission of either component forces clean recomputation and cannot pass
incremental qualification.

### 10.0 State-scope distinction

The request-local and conversation-scoped records are separate stores and
separate accounting domains:

| Record | Store key | Created by | Reuse rule |
|---|---|---|---|
| `ProviderDecodeStateEntryV1` | Full `DecodeStateIdentityV1`, including the current `requestId`/attempt/generation | Prefill and subsequent decode epochs of one Request | Only the next authenticated epoch of that same Request; a new Request cannot look it up directly |
| `ConversationStateEntryV1` | `conversationId` + `contextEpoch` + exact model/role/Provider/prefix/security identity | Atomic promotion of the complete finalized request-local KV/recurrent/convolution bundle after all selected roles commit | Only an explicit `APPEND_DELTA` turn with a valid aggregate checkpoint and exact prefix extension |

Promotion is an ownership transition, not a relaxed cache lookup. The
conversation entry must represent the complete canonical turn prefix before it
is published; a request-local hit, public prompt equality, or `conversationId`
alone never authorizes cross-request reuse. Metrics, quotas, residency, and
eviction counters for the two records MUST remain distinguishable.
The checkpoint, receipt set, and transcript are control/commitment records;
they are not the retained model-state bytes. A later Request is a
conversation-scoped cache hit only after every selected Provider has located
or prefetched its complete retained state and the runner consumes that state
for suffix-only prefill. If the state is absent, the result is the explicit
full-context fallback or continuation failure, not a synthetic cache hit.

For avoidance of ambiguity, a conversation entry is produced only after a
completed Request has finalized and atomically committed every selected role.
The next turn is a different Request: its `requestId`, `generationId`, and
request-local store entry are fresh, while its `parentCheckpoint` and expected
context epoch authorize restoration of the previously promoted
`ConversationStateEntryV1`. The restored entry supplies the parent prefix;
only the newly tokenized suffix is prefetched/evaluated. This is the required
cross-request path, distinct from carrying state from one decode epoch to the
next inside a single Request.

The Provider implements this boundary through an explicit fresh-request lease:
`acquire_for_request(APPEND_DELTA, freshRequestId, role, receipt)` performs the
exact conversation-store lookup (and, when needed, a single-flight
host-to-GPU prefetch), then pins the complete state while suffix prefill runs.
`release_for_request(...)` drops that pin. Both operations reject the origin
Request ID and never consult the request-local map, so a request-local hit
cannot be misreported as a conversation hit. Conversation-hit counters are
incremented only after the exact entry is GPU-ready and pinned.

The Qwen3.6 qualification manifest describes only the language-model-only text
subgraph. It records the source model revision and explicitly proves that the
vision encoder/projector and MTP/speculative heads are outside the execution
artifact closure. A manifest that merely omits these fields is not equivalent
to disabling them.

The separate immutable workload manifest contains two prompt IDs, canonical
message bytes, prompt SHA-256 values, the pinned chat-template/tokenizer
digests, derived input-token IDs/counts, `thinkingMode=disabled`, Greedy
sampling, `maxGeneratedTokens=64`, `maxEvents=65`, and
`requestDeadlineMs=120000`. Operational evidence records only the IDs, digests,
and lengths, not the plaintext messages.

Identity values have three provenance classes:

- **sealed static/runtime authority**: artifact, graph, adapter, runner,
  role/split/layers, precision/layout/schema/components, runtime ABI, security
  domain, Provider/boot identity, and cache-incarnation epoch come from
  validated artifacts, the accepted Provider projection, and the loaded
  runtime;
- **authenticated request**: request, attempt, and generation identity come
  from the accepted collaboration contract; and
- **coordinator-derived dynamic**: logical prefix digest/count, position
  digest, state inference epoch, and predecessor inference epoch come from the
  admitted prompt/token lineage and the committed local entry.

No class may be populated from an application-supplied cache key or a generated
fallback digest. `prefixDigest` commits the canonical model token IDs consumed
by the model, not role-local activation bytes. All roles at one inference epoch
therefore have the same `prefixDigest` and `prefixTokenCount`, while
`roleName`, split/layer fields, and state components keep their entries
distinct.

| Field | Comparison |
|---|---|
| `modelDigest` | exact |
| `graphSemanticDigest` | exact |
| `artifactDigest` | exact |
| `adapterDigest` | exact |
| `tokenizerDigest` | exact |
| `runnerDigest` | exact |
| `roleName` | exact |
| `roleSplitDigest` | exact |
| `layerRange` | exact |
| `prefixDigest` | exact canonical consumed-token prefix; common across roles in the epoch |
| `prefixTokenCount` | exact number of consumed model tokens; common across roles in the epoch |
| `positionDigest` | exact adapter-certified position/cache-position commitment for the prefix and role |
| `precision` | exact |
| `layoutDigest` | exact |
| `stateSchemaDigest` | exact |
| `stateComponentDigests` | exact ordered set of required component kinds/layouts |
| `runtimeAbiDigest` | exact |
| `securityDomainDigest` | exact |
| `providerIdentity` | exact |
| `providerBootId` | exact |
| `stateInferenceEpoch` | exact; 0 for state produced by prefill, then one per decode |
| `predecessorInferenceEpoch` | absent for prefill; exactly `stateInferenceEpoch - 1` for decode |
| `cacheEpoch` | exact Provider cache incarnation; constant within one lineage and changed only by cache reset/invalidation |
| `requestId` | exact for version 1 |
| `attemptEpoch` | exact |
| `generationId` | exact |

Version 1 intentionally forbids direct cross-request sharing of this
request-local entry even when public prompt bytes match. Sections 10d--10g
define an additive V1 conversation checkpoint and a separate retained entry
that authorize an exact new-turn transition; they do not relax any field
comparison inside `DecodeStateIdentityV1` for the original turn.

## 10a. ProviderDecodeStateEntryV1

Provider-owned runtime record that makes `DecodeStateBundleV1` automatic in the
production role-execution path. The identity and lifecycle metadata are
inspectable; tensor contents and device handles are adapter-private and never
serialized into operational evidence or NDN Data.

| Field | Type | Rule |
|---|---|---|
| `identity` | `DecodeStateIdentityV1` | exact complete match; explicitly includes state and predecessor epochs |
| `committedInferenceEpoch` | uint64 | equals `identity.stateInferenceEpoch`; 0 after prefill, then advances by one atomic commit |
| `expectedNextInferenceEpoch` | uint64 | exactly committed epoch + 1 |
| `storageKind` | enum | `HostMaterialized` for CPU gates or `DeviceResident` for CUDA qualification |
| `deviceId` / `executionProvider` | string | exact runner binding; immutable for one entry |
| `logicalBytes` | uint64 | sum of all required state components |
| `hostTransferBytesIn/Out` | uint64 | measured per epoch; zero for complete-state transfers in qualified CUDA decode |
| `actualNewInputExtent` | uint64 | actual new token count at Stage 0 or adapter-defined current activation extent at later roles |
| `representedPrefixTokenCount` | uint64 | logical model-token prefix already represented by the committed predecessor |
| `prefixWorkAvoided` | uint64 | machine-derived full-prefix reference extent minus actual cached runner extent; for a conversation continuation at request epoch zero this includes the exact promoted parent prefix, while later epochs count the committed request-local predecessor |
| `pinned` | bool | true while transition/publication is in flight; pinned entries are not evictable |
| `candidate` | opaque optional | at most one adapter-owned uncommitted successor |
| `lastAccessSequence` | uint64 | deterministic inactive-entry LRU order |
| `createdAt` / `lastCommitAt` | monotonic time | bounded lifecycle/cleanup evidence |

State transitions:

```text
Absent --prefill success/publication accepted--> Committed(epoch 0)
Committed(e-1) --exact lookup--> Candidate(e)
Candidate(e) --output validated/publication accepted--> Committed(e)
Candidate(e) --execution/publication failure--> Committed(e-1)
Committed --inactive capacity pressure--> Evicted
Any --cancel/deadline/failed terminal/attempt fence/boot change--> Released
Committed --successful conversation terminal--> ConversationStateCandidate
ConversationStateCandidate --all-role checkpoint commit--> ConversationStateEntryV1
ConversationStateCandidate --aggregate rollback--> Released (parent entry unchanged)
Committed --successful non-conversation terminal--> Released
```

An epoch greater than zero cannot transition from `Absent` by synthesizing zero
state. It records a miss and enters only the registered recompute/new-plan path,
and the runner call count remains zero for that rejected transition. The store
key may be compact, but successful lookup compares the full identity; key
equality alone never authorizes reuse.

The conversation-terminal transition may not blindly move the last decode
entry. First, the adapter compares its represented logical prefix with the
canonical completed-turn transcript. If accepted non-EOS output or sealed
turn-finalizing template tokens are still missing, it runs bounded
`CHECKPOINT_FINALIZE` state-only transitions. These transitions update state
and prefix identity but sample and emit nothing. Only an exact finalized prefix
may become `ConversationStateCandidate`; direct lookup of this request-local
entry by a later request is invalid.

## 10b. NativeEpochCoordinatorStateV1

Request-scoped native state for a selected multi-Provider plan. This is attached
to the existing deferred collaboration and is not a second Request or
placement object.

| Field | Type | Rule |
|---|---|---|
| `requestId` / `attemptEpoch` | Name/uint64 | exact accepted collaboration binding |
| `planDigest` / `generationId` | SHA-256/opaque | exact committed plan and generation |
| `streamEpoch` | uint64 | exact current stream epoch |
| `role` / `provider` | string/Name | one complete role and its selected Provider |
| `inferenceEpoch` | uint64 | 0 for prefill; increments once per decode transition |
| `operationBase` / `edgeCount` | uint64 | signed DATA_V1 operation allocation |
| `maxGeneratedTokens` | uint32 | signed upper bound; no implicit extension |
| `committedStateIdentity` | `DecodeStateIdentityV1` | absent before prefill, exact thereafter |
| `providerStateEntry` | reference | exact local `ProviderDecodeStateEntryV1`; never an NDN object |
| `acceptedTokenEpoch` | uint64 | advances only after external event admission |

The operation index for edge ordinal `j` at prefill is
`operationBase + j`; at decode epoch `e` it is
`operationBase + (e + 1) * edgeCount + j`. Each operation and exact Data name
is bound to its epoch, producer/consumer role, tensor digest, request, attempt,
plan, generation, and stream epoch. Missing, repeated, or mismatched entries
are terminal errors. The final role alone may update the accepted token epoch
and claim End/Response.

## 10c. GenerationEpochLineageV1

Bounded authenticated metadata carried inside the encrypted DATA_V1 plaintext
for every cross-Provider activation or TOKEN_FEEDBACK dependency. A one-role
process-local self-edge carries the same object under the same validation rules.
It is not part of the public NDN name and contains no prompt/token sequence or
decode-state tensor.

| Field | Type | Rule |
|---|---|---|
| `requestId` / `attemptEpoch` | Name/uint64 | exact accepted collaboration |
| `planDigest` / `generationId` / `streamEpoch` | digest/opaque/uint64 | exact selected generation |
| `inferenceEpoch` | uint64 | epoch whose role transition is being authorized |
| `transitionKind` | enum | `PREFILL`, `DECODE`, or `CHECKPOINT_FINALIZE`; exact plan-bound phase |
| `logicalPrefixDigest` | SHA-256 | canonical model-token prefix already consumed by the candidate state |
| `logicalPrefixTokenCount` | uint32 | exact consumed-token count |
| `positionDigest` | SHA-256 | adapter-certified position/cache-position commitment |
| `producerRole` / `consumerRole` | string | exact planned edge endpoints |
| `operationIndex` | uint64 | exact pre-authorized DATA_V1 operation |

The complete object is included in the DATA_V1 AEAD plaintext and manifest
binding. The consumer verifies it before Provider-state lookup. A digest/count/
position mismatch is terminal for the attempt and the runner is not called.
The same logical prefix digest/count is expected at every role in one epoch;
role and operation fields remain edge-specific.

## 10d. ConversationTranscriptRecordV1 and ConversationContinuationV1

`ConversationTranscriptRecordV1` is protected User-side state stored through
the existing envelope-key-backed `RuntimeJournal`. It is required to validate
an appended turn without resending the old transcript.

| Field | Type | Rule |
|---|---|---|
| `conversationId` / `contextEpoch` | opaque ID/uint64 | exact committed conversation version |
| `requesterIdentity` / `serviceName` / `securityDomainDigest` | Name/digest | exact authorization scope |
| `applicationMessages` | canonical encrypted bytes | complete committed structured message lineage; never logged or included in evidence |
| `tokenizerDigest` / `chatTemplateDigest` | SHA-256 | exact canonicalization authority |
| `canonicalTokenIds` | encrypted uint32 vector | exact complete model-token prefix represented by Provider state |
| `prefixDigest` / `prefixTokenCount` | digest/count | exact commitment to `canonicalTokenIds` |
| `providerRoleReceipts` | ordered encrypted signed-byte set | complete validated role receipts needed to verify the aggregate checkpoint; contains no state tensors or local addresses |
| `checkpointDigest` / `planRoleMapDigest` | digest | exact aggregate Provider-state binding |
| `createdAt` / `expiresAt` | authenticated time | no later than checkpoint expiry |

The record is encrypted/authenticated at rest by the requester's journal key and
is not sent on a healthy delta turn. A new process may resume only after opening
and validating the exact record. Missing key/record, digest mismatch, or
truncation selects only explicit full-context fallback/failure.

Optional application-level continuation input carried by one new turn. Absence
means the existing request-scoped full-context behavior.

| Field | Type | Rule |
|---|---|---|
| `conversationId` | 128-bit random opaque value | stable index across turns; not an authorization token |
| `mode` | enum | `FULL_CONTEXT` or explicit `APPEND_DELTA` |
| `expectedParentContextEpoch` | uint64 optional | required for `APPEND_DELTA`; absent for the first full-context turn |
| `parentCheckpoint` | `ConversationCheckpointV1` optional | required for `APPEND_DELTA`; exact authenticated bytes |
| `turnInputDigest` | SHA-256 | exact canonical full input or appended-input bytes supplied for this turn |
| `allowFullPrefillFallback` | bool | default false for `APPEND_DELTA`; never inferred |
| `fallbackFullInputDigest` | SHA-256 optional | required when fallback is allowed; plaintext remains in the encrypted application input |
| `requestContractDigest` | SHA-256 | seals this continuation object into the new Request/plan contract |

Every turn still creates a fresh request ID, attempt epoch, plan digest, and
generation ID. `conversationId` never replaces those identities. The API may
return the opaque checkpoint bytes to the application, but it must not expose
its internal Provider receipts or storage locations as caller-controlled
fields.

For `APPEND_DELTA`, `turnInputDigest` covers the caller's new application
message. The sealed adapter derives an `appendedCanonicalSuffix` that may add
chat-template/control tokens. Evidence records raw new-message tokens,
adapter-added suffix tokens, and their total; only that total suffix, never the
parent prefix, enters delta prefill.

### 10d.1 ConversationTurnBindingV1

Request-scoped Selection commitment shared by every selected Provider for the
one successor conversation epoch being created. It is present on both the
initial `FULL_CONTEXT` turn and later `APPEND_DELTA` turns; it contains no
model state, local path, endpoint list, or Provider-generated identity.

| Field | Type | Rule |
|---|---|---|
| `schema` / `version` | string/uint16 | `ndnsf-di-conversation-turn-binding-v1` / `1` |
| `conversationId` | opaque ID | exact application conversation; never authority by itself |
| `parentContextEpoch` / `successorContextEpoch` | uint64 | successor is exactly parent + 1; the first turn uses parent 0 |
| `serviceName` | Name | exact service of the accepted assignment |
| `planRoleMapDigest` | SHA-256 | canonical complete one-to-one Provider-role map |
| `requestContractDigest` | SHA-256 | exact current Request/plan authority |
| `retentionDeadlineMs` | timestamp | requester upper bound; a Provider receipt may expire earlier under Provider policy |
| `parentCheckpointDigest` | SHA-256 optional | empty only for parent epoch 0; required for every later turn |

`AutomaticPlanningCoordinator` creates this object only after ACK-driven
placement is final and projects the identical commitment into each selected
Provider's V3 Selection. For `APPEND_DELTA`, it first verifies the authenticated
parent checkpoint's conversation, epoch, service, unexpired lifetime, complete
role set, and exact retained role map. The native Provider rejects any mismatch
between this binding, the assignment service/request digest, and its role-local
`ConversationStateReferenceV1` before state lookup or model execution.

## 10e. ProviderConversationStateReceiptV1

Authenticated Provider statement that one complete role owns compatible state
for a committed conversation prefix.

| Field | Type | Rule |
|---|---|---|
| `conversationId` | opaque ID | exact continuation lineage |
| `parentContextEpoch` / `successorContextEpoch` | uint64 | successor is exactly parent + 1 |
| `originRequestId` / `originGenerationId` | Name/opaque | turn that produced the state; retained for audit, not matched to a later turn's fresh IDs |
| `serviceName` / `requesterIdentity` / `securityDomainDigest` | Name/digest | exact authorization scope |
| `modelDigest` / `graphSemanticDigest` / `adapterDigest` | digest | exact loaded model semantics |
| `roleName` / `roleSplitDigest` / `layoutDigest` | string/digest | exact one-Provider/one-complete-role binding |
| `planRoleMapDigest` | SHA-256 | exact complete one-to-one Provider-role map |
| `providerIdentity` / `providerBootId` / `cacheEpoch` | Name/opaque/uint64 | exact local owner and cache incarnation |
| `prefixDigest` / `prefixTokenCount` / `positionDigest` | digest/count/digest | exact common logical conversation prefix and role position |
| `stateSchemaDigest` / `stateComponentDigests` | digest/list | complete adapter-required state |
| `expiresAt` | authenticated timestamp | bounded by configured retention and Provider policy |
| `receiptDigest` / `signature` | digest/signature | canonical Provider-authenticated receipt |

The receipt contains no state tensors, pointers, paths, or reusable Provider
tokens. A later turn verifies it before any Provider-local lookup.

## 10f. ConversationCheckpointV1

Coordinator-authenticated aggregate commitment returned only after every role
receipt is accepted.

| Field | Type | Rule |
|---|---|---|
| `version` | uint16 | `1` |
| `conversationId` | opaque ID | exact conversation |
| `parentContextEpoch` / `contextEpoch` | uint64 | exact linear successor |
| `serviceName` / `requesterIdentity` / `securityDomainDigest` | Name/digest | exact authority scope |
| `modelContractDigest` / `planRoleMapDigest` | SHA-256 | exact model and same-placement contract |
| `logicalPrefixDigest` / `prefixTokenCount` | digest/count | common across every role receipt |
| `roleReceiptDigests` | ordered role->digest map | complete set; no duplicate or missing role |
| `issuedAt` / `expiresAt` | authenticated times | bounded lifetime; expiry is no later than the earliest included role-receipt expiry |
| `checkpointDigest` / `signature` | digest/signature | canonical requester/AutomaticPlanningCoordinator identity signature verifiable under the existing trust schema |

Checkpoint construction is transactional:

```text
ParentCommitted(e)
  -> collect every successor role receipt for e+1
  -> validate one common prefix and exact role map
  -> compare-and-swap parent e
  -> publish Checkpoint(e+1)
```

Failure before the compare-and-swap leaves the parent checkpoint valid and
discards candidate successor state. Two children of the same parent cannot both
commit. A checkpoint is opaque to the application and is never a Provider list.

## 10g. ConversationStateEntryV1 and residency

Provider-local role state retained across requests. Its compatibility key is:

```text
(conversationId, contextEpoch, service/security scope,
 model/graph/adapter/split/layout/state-schema digests,
 roleName, planRoleMapDigest, providerIdentity/providerBootId/cacheEpoch,
 logicalPrefixDigest/prefixTokenCount/positionDigest)
```

`originRequestId` and `originGenerationId` are audited but a resumed turn has
fresh values. The Provider verifies the authenticated checkpoint/receipt and
then derives a new request-scoped `DecodeStateIdentityV1` transition; it never
looks up by `conversationId` alone.

Creation is an ownership promotion from a finalized
`ProviderDecodeStateEntryV1`, not a cross-request lookup in the request store.
The promoted entry represents the complete prior conversation prefix, including
accepted assistant output and required turn-finalizing tokens. Device buffers
may be moved or retained by strong reference without copying, but the
conversation manager becomes their sole long-lived lifecycle owner only after
the all-role aggregate checkpoint commits.

| Field | Type | Rule |
|---|---|---|
| `receipt` | `ProviderConversationStateReceiptV1` | exact committed identity |
| `residencyTier` | enum | `GPU_RESIDENT`, `HOST_RESIDENT`, or `EVICTED` |
| `lifecycle` | enum | `IDLE`, `PREFETCHING`, `PINNED`, or `COMMITTING`; orthogonal to residency |
| `logicalBytes` / `allocatedBytes` | uint64 | separate state and allocator accounting |
| `deviceId` / `hostPoolId` | string optional | one active storage location per state generation |
| `pinCount` / `prefetchGeneration` | uint32/uint64 | active/committing state is protected; one in-flight prefetch |
| `lastAccessSequence` | uint64 | deterministic inactive-LRU order |
| `createdAt` / `lastUsedAt` / `expiresAt` | monotonic time | bounded retention, default 300 seconds |
| `transferBytes` / `transferLatency` | counters/duration | measured tier movement; contents never logged |

Allowed tier/lifecycle transitions:

```text
GPU_RESIDENT/IDLE --pause/capacity--> HOST_RESIDENT/IDLE
HOST_RESIDENT/IDLE --scheduled--> HOST_RESIDENT/PREFETCHING
  --copy complete--> GPU_RESIDENT/IDLE
GPU_RESIDENT/IDLE --execute--> GPU_RESIDENT/PINNED
  --successor commit--> GPU_RESIDENT/COMMITTING --> GPU_RESIDENT/IDLE
HOST_RESIDENT|GPU_RESIDENT / IDLE --inactive LRU/expiry/reset--> EVICTED/IDLE
HOST_RESIDENT/PREFETCHING --cancel/failure--> HOST_RESIDENT/IDLE or EVICTED/IDLE
```

The state manager has independent GPU and host byte/entry quotas. It never
evicts lifecycle `PINNED`/`COMMITTING` or dispatch-ready entries. Eviction and
invalidation release/zeroize adapter-owned host/device buffers under the trusted
Provider runtime policy. Model weights remain GPU resident;
only role-local conversation state moves. No disk tier or cross-Provider state
migration exists in this version. The coordinator starts turn execution only
after all selected roles report exact compatible `GPU_RESIDENT` readiness.

## 11. QualificationManifestV1

Required fields:

- schema/version and gate ID;
- start/end UTC timestamps and status;
- git commit/tree and dirty-path inventory;
- SIF, native library, Python extension, model, tokenizer, adapter, workload,
  and contract digests;
- Python/SOABI, glibc, Boost, ndn-cxx, NDN-SVS, NFD, MiniNDN, ONNX Runtime,
  CUDA/driver, Apptainer, and compiler versions;
- topology and exact one-to-one Provider-role map;
- per-role sealed static decode-state identity source digests plus per-epoch
  logical prefix/count, position, state/predecessor, and cache lineage;
- seed, prompt IDs, warmup/repetition counts, maximum tokens, timeouts, and
  stream options;
- request/event/End/Response counts and transcript/oracle verdicts;
- TTFT, per-token latency summary, total latency, tokens/s, retransmission,
  queue, decode-state hit/miss/recompute, actual input extent, represented
  prefix extent, prefix work avoided, CPU model-compute fallback, bounded shape-control execution,
  GPU/CPU/memory, and failure metrics;
- conversation ID hash, turn mode, parent/successor context epochs, checkpoint
  and role-receipt digests, per-role continuation hit/miss/reason, residency
  transitions, GPU/host resident bytes, prefetch/eviction/transfer timing and
  bytes, delta-prefill input tokens, fallback status, and commit/conflict result;
- command, working directory, environment allowlist, artifact paths, and SHA-256
  hashes of evidence files;
- final verdict: `FAIL`, `FUNCTIONAL_PASS_PERFORMANCE_MISS`, or
  `PERFORMANCE_PASS`.
