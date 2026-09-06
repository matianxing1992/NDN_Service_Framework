# Generation Contract: Incremental ONNX Pipeline V1

## 1. Placement and ownership

- Default strategy: `PreSplitFirstStrategy`.
- Plan shape: ordered pipeline roles `Stage0..StageN-1`, where `N` is 1, 2, or
  4 in Spec 175 validation.
- Mapping is one-to-one: each role has one Provider; each selected Provider has
  one role; no Provider owns two roles; no role spans Providers.
- `StageN-1` is the final role and owns final logits, sampling, incremental text
  decoding, external events, End, and final Response.
- Tensor/rank groups are not valid plan objects in this streamed-generation
  contract. The existing Spec 174 TensorGroup/rank-role contract remains valid,
  unchanged, and outside Spec 175 validation.
- The Spec175 streaming entry point accepts only an ordinary rank-0 V3 plan.
  V2 compatibility plans, hybrid plans, tensor degree other than one, missing
  roles, and duplicate Provider ownership fail before Selection. The registered
  workload names `PreSplitFirstStrategy`; `LayerReuseFirstStrategy` is not a
  formal Spec175 subject.

The plan is committed through the same deferred collaboration Request that
created the stream handle. `commitPlan(binding)` means the existing
`CommitCollaborationPlan` owner plus Provider-specific Selection projection; it
does not launch a second service Request.

## 2. Generation state machine

States:

```text
Created -> Selecting -> Preparing -> Prefilling -> Decoding -> Draining -> Completed
              |             |            |            |           |
              +-------------+------------+------------+----------> Failed
                            |            |            |
                            +------------+------------+----------> Cancelled
                                         |
                                         +-> Rebuilding -> Preparing
```

Allowed calls:

| State | Operation | Next state / effect |
|---|---|---|
| Created | `beginSelection()` | Selecting |
| Selecting | `commitPlan(binding)` | Preparing |
| Preparing | `activate()` | Prefilling |
| Prefilling | `completePrefill()` | Decoding; no second prefill in attempt |
| Decoding | `commitToken(tokenId,eventCursor)` | increment token/prefix identity |
| Decoding | `beginDrain(reason)` | Draining |
| Decoding | `beginReplacement(reason)` | Rebuilding; attempt+1; old stream fenced |
| Rebuilding | `commitPlan(newBinding)` | Preparing |
| Draining | `claimTerminalResponse()` | once only |
| Draining | `complete()` | Completed after End/Response consistency |
| any nonterminal | `terminate(code)` | Failed |
| any nonterminal | `cancel()` | Cancelled |

Terminal states are absorbing. Invalid transitions throw before wire or callback
side effects.

## 3. Successful finish reasons

| Reason | Valid condition |
|---|---|
| `Eos` | sampled token belongs to sealed EOS set; generated count may be below max |
| `StopSequence` | incremental decoded suffix completes one sealed stop string; count may be below max |
| `MaxTokens` | generated count equals sealed `maxGeneratedTokens` |
| `ApplicationComplete` | non-LLM streamed application declares complete |

`complete()` without a reason is removed from the Qwen state machine. Existing
callers migrate to `beginDrain(reason)` plus terminal closure. `MaxTokens` before
the exact bound and EOS/stop without the corresponding evidence are errors.

## 4. Epoch conventions

- `attemptEpoch` starts at 1 and advances to 2 only for the accepted replacement.
- `inferenceEpoch=0` is prefill over the full prompt.
- A successful prefill produces generated `tokenEpoch=1`.
- For `k>=1`, decode `inferenceEpoch=k` consumes sampled token `k` plus the exact
  provider-local decode-state bundle and produces sampled `tokenEpoch=k+1`.
- External Application event cursor equals `tokenEpoch` in version 1.
- End cursor equals `generatedTokenCount + 1`.
- Therefore `maxGeneratedTokens <= maxEvents - 1`.
- A conversation-enabled plan additionally seals
  `maxCheckpointFinalizeTokens=32`. Finalization consumes the next unused
  inference epochs after terminal decode, never event cursors, and the exact
  `transitionKind=CHECKPOINT_FINALIZE` is authenticated in lineage metadata.
- No cursor is committed until the external event has passed bounded publisher
  queue admission.

## 5. Internal exact Data names

### Stage activation

```text
/<producerProvider>/NDNSF/DI/ACTIVATION/
  <requester-uri-component>/<service-name...>/<requestId>/
  <attemptEpoch>/<planDigest>/<generationId>/
  <fromRole>/<toRole>/<inferenceEpoch>
```

### Final-role token feedback

```text
/<finalProvider>/NDNSF/DI/TOKEN-FEEDBACK/
  <requester-uri-component>/<service-name...>/<requestId>/
  <attemptEpoch>/<planDigest>/<generationId>/
  <finalRole>/<firstRole>/<tokenEpoch>
```

Both are deterministic signed exact Data names. Large activation payloads use
the existing segmented exact collaboration publication/fetch path with the name
as the object root. Token feedback carries the token ID, accepted prefix digest,
position digest, and token epoch; it is not the external user event and is
encrypted for the collaboration scope.

The signed DATA_V1 capability must allocate a distinct operation index for each
activation/feedback edge in each bounded epoch. For `E` edge ordinals and a
maximum of `K` generated tokens, the canonical allocation is:

```text
prefill:       base + edgeOrdinal
decode epoch e: base + (e + 1) * E + edgeOrdinal
```

`base`, `E`, `K`, and, for a conversation-enabled plan, the additional fixed
finalization cap `F=32` are included in the plan/capability digest. The bounded
operation inventory covers prefill, at most `K-1` decode transitions, and at
most `F` state-only finalization transitions. Each transition consumes the next
unused inference epoch and its operation slot exactly once; terminal decode and
finalization cannot both consume the same slot. The operation
index and the epoch must both appear in the exact Data name and manifest;
reusing a name or operation index across epochs is invalid. A missing
operation is a failed invocation, not an implicit fallback to an unbound
COLLAB-LARGE object.

## 6. Per-epoch pipeline order

For inference epoch `e`:

1. Stage0 waits for prompt input (`e=0`) or token feedback (`e>0`).
2. Every stage verifies attempt, plan, generation, dependency name, upstream
   role/Provider, tensor contract, and deadline.
3. The stage verifies/creates its exact local decode-state identity, executes
   its persistent ONNX Runtime session, and commits the candidate decode-state
   bundle only after successful output.
4. Nonfinal stages publish one activation for the next stage.
5. The final stage samples once from final logits.
6. The final stage uses the incremental tokenizer state to compute a token event
   and determines EOS/stop/max.
7. The final stage admits the external event. Only after admission does it
   commit token epoch/prefix and publish internal feedback.
8. If nonterminal, the next decode epoch starts. If terminal, the feedback
   carries the authenticated terminal marker. Stage0 consumes it without an
   ORT step and forwards a terminal activation marker; each intermediate stage
   verifies and forwards that marker without an ORT step, then ends its local
   coordinator. This follows the existing pipeline edges and does not create a
   broadcast or second service Request.
9. The final stage admits End and one complete Response and enters Draining.
   The external terminal owner does not wait for the last internal marker to be
   consumed, but the native validation gate must prove that every selected
   Provider coordinator terminates without failure.

No role polls an unspecified name and no token epoch creates a new service
Request or placement plan. The native implementation runs one unary runner
transition per role and epoch. `NativeModelRunner::runStreamed()` is reserved
for the one-Provider compatibility path because it owns a complete token loop;
it cannot replace the cross-Provider epoch coordinator described in
`native-epoch-coordinator-v1.md`.

Because an activation edge carries either a normal model tensor bundle or the
terminal-control bundle, its fixed `expectedBytes` is unset in V1. The signed
DATA_V1 manifest supplies the exact encoded byte count and the sealed
capability still enforces `maxBytes` and `maxSegments`.

## 7. Sampling configuration V1

Sealed fields:

| Field | Default | Validation |
|---|---:|---|
| `mode` | `Greedy` | `Greedy` or `SeededTopKTopP` |
| `temperature` | 0.0 | 0 for Greedy; `(0,5]` otherwise |
| `topK` | 1 | 1..vocabulary size |
| `topP` | 1.0 | `(0,1]` |
| `repetitionPenalty` | 1.0 | `[0.1,2.0]` |
| `seed` | 1750001 | uint64; ignored only by Greedy |
| `decodeMode` | `single-token-autoregressive` | only V1 mode |
| `modality` | `text-only` | no vision input/encoder/projector |
| `mtpEnabled` | `false` | MTP/speculative decode is out of scope |
| `thinkingMode` | `disabled` | no implicit hidden-thinking template |
| `chatTemplateDigest` | workload manifest | canonical SHA-256, exact match |
| `maxGeneratedTokens` | 64 in validation | 1..`maxEvents-1` |
| `eosTokenIds` | adapter manifest | nonempty bounded set |
| `stopStrings` | workload manifest | max 16 entries, each <=256 UTF-8 bytes |

All mandatory correctness and Tiger performance cases use Greedy mode,
`thinkingMode=disabled`, the same pinned chat template, and one
sampled token per decode epoch. The final token feedback closes epoch E before
epoch E+1 begins; MTP/speculative token trees cannot bypass that causal edge in
V1. A
separate deterministic unit suite fixes logits and seed for
`SeededTopKTopP`; hardware-crossing exact token claims are not made for
stochastic floating-point logits.
The native final role executes these exact sealed values. A `samplingDigest`
without the corresponding validated parameters and implementation is not a
sampler; unsupported modes fail before the first token rather than silently
falling back to Greedy.

## 8. Incremental tokenizer contract

State contains exact tokenizer digest, accepted token IDs, decoded UTF-8 prefix,
and stop-matcher suffix bounded by the longest stop string. For each new token:

1. verify the expected token epoch and tokenizer digest;
2. apply the tokenizer's decoder semantics with retained state;
3. emit only the newly committed UTF-8 text delta;
4. update the bounded stop matcher across token boundaries;
5. never emit an invalid UTF-8 prefix or cut a code point;
6. on rollback/replacement, rebuild state from the committed accepted token list,
   not from uncommitted output.

The deployed adapter imports `tokenizers`, not `transformers`. Unit fixtures
include leading-space pieces, multibyte Unicode, empty deltas, EOS, and stop
strings crossing two and three token boundaries.
The production native final-role owner loads that standalone tokenizer and
owns this state. Token IDs with permanently empty `textDelta`, or a final
payload without decoded text when the oracle is nonempty, are diagnostic-only.

## 9. Provider-local decode state and atomic update

This section governs **request-local decode state** only: the state used by the
prefill/decode epochs of one Request, its authorized attempt, and its
generation. It is released at terminal cleanup unless the explicit promotion
transaction below transfers the finalized candidate. A later Request MUST NOT
look up this state by prompt equality, `conversationId`, or any re-keyed old
request identity. The separate **conversation-scoped state** used for
cross-request continuation is defined by the authenticated checkpoint and
context-epoch rules in Section 10; its hit/promotion/residency evidence is
reported independently.

`DecodeStateIdentityV1` must match every field listed in `data-model.md`.
`DecodeStateBundleV1` is model-adapter-defined but complete: for Qwen3.6 it
contains full-attention KV tensors plus the convolution/recurrent state required
by linear-attention layers. Runtime objects additionally hold the persistent ORT
session, named input/output bindings, and device buffers. These are local opaque
handles and are never serialized into normal evidence.

The identity is not a caller-selected cache key. Before prefill, the Provider
handler materializes the sealed static fields from the verified artifact,
accepted projection, selected Provider/boot identity, and loaded runtime ABI;
missing fields reject the plan. Request/attempt/generation fields come from the
authenticated collaboration. The epoch coordinator derives all remaining
dynamic fields.

`prefixDigest` commits a domain-separated canonical encoding of the model token
IDs consumed by the state. Prefill state commits the exact prompt-token sequence.
Each later state extends that sequence by exactly the one authenticated token
admitted through the preceding TOKEN_FEEDBACK lineage. A downstream role does
not hash its activation bytes to invent a prefix: the activation carries the
authenticated logical prefix digest/count and position commitment established
for the epoch, and every role verifies that common lineage before execution.
`positionDigest` commits the adapter-certified position/cache-position inputs
for the same prefix and role.

Prefill produces `stateInferenceEpoch=0` and has no
`predecessorInferenceEpoch`. Decode transition `e>0` produces
`stateInferenceEpoch=e` and requires
`predecessorInferenceEpoch=e-1`. These fields are distinct from `cacheEpoch`,
which identifies the Provider-local cache incarnation, remains constant within
one lineage, and changes only when the cache is reset or invalidated.

The selected Provider's production role session owns this state. Every
multi-Provider epoch reaches the same store through the normal
`NativeProviderSession`/role-execution path; neither the User, an integration
harness, nor a dependency object supplies the previous state. The store keeps
one committed bundle and at most one candidate per exact role/attempt identity,
pins the committed entry during execution/publication, and compares the full
identity including the immediately preceding inference epoch. A decode cache
miss is never converted into zero-filled state, and the model runner is not
called for a rejected predecessor.

Epoch update is transactional:

```text
verify old identity -> execute into candidate buffers -> validate output ->
admit downstream activation/event -> swap candidate bundle into committed bundle
```

Failure before swap leaves the prior committed bundle usable for the same exact
attempt/epoch retry. Failure after an externally visible token commit terminates
or starts a new replacement attempt; it never rewinds that cursor inside the
same stream epoch.

On CUDA, committed and candidate state uses persistent device-resident buffers
and ONNX Runtime I/O binding. The healthy path may copy token IDs, shape-control
scalars, and planned activations as required, but it does not materialize the
complete state bundle on the host between epochs. CPU tests may use materialized
`TensorBundle` bytes. Operational evidence records state identity, logical
bytes, residence, host-transfer bytes/time, hit/miss/commit/eviction counts, and
cleanup outcome, never tensor contents.
The actual distributed coordinator and `ProviderRoleWorker` must retain the
opaque adapter-owned device-state transaction across calls. Setting a
single-runner-only flag or proving zero copies in `runStreamed()` does not prove
this contract when the multi-Provider path invokes per-epoch `run()`.

A reported hit is valid only when the production runner receives the exact
committed predecessor plus the bounded new token/current activation and does not
execute the already represented prefix. Evidence records the actual input
extent, represented prefix extent, and machine-derived prefix work avoided for
each transition. The CPU oracle compares this cached branch with an otherwise
identical full-prefix reference for exact output and work-shape parity; it does
not use elapsed time as a CPU acceptance threshold. CUDA qualification adds the
registered alternating-order timing comparison and device-residency proof.

Cancellation, deadline, failed terminal completion, accepted-attempt replacement,
and Provider boot-ID change fence and release the matching request-scoped state.
For a successful conversation-enabled turn, terminal completion instead
first finalizes each exact committed role state to the canonical completed-turn
prefix, then promotes it into the conversation-state manager,
publishes its role receipt, and releases the request-scoped owner only after the
aggregate checkpoint commits or rolls back. A non-conversation success releases
state as before. Inactive capacity
eviction is deterministic and cannot evict a pinned entry. A replacement
Provider recomputes from the protected prompt and committed prefix; V1 never
migrates live state between Providers.

## 10. Replacement and accepted prefix

- Default: disabled.
- Opt-in: exactly one replacement.
- Replacement is one new internal Normal Request. It obtains fresh one-time
  tokens, ACK closure, plan, Selection, event key, and stream epoch; attempt-1
  authority is never replayed. The public handle and generation ID stay stable.
- Replacement eligibility requires the recoverable Core error to carry the
  absolute Provider identity from the validated event/response binding. The
  coordinator excludes exactly that Provider and never infers a failure owner
  from a prior role map.
- Replacement input consists of the original protected prompt plus the ordered
  token IDs whose external events were committed before failure.
- The user includes the accepted-prefix token IDs, digest, and count in the
  encrypted recovery Request. The new V3 plan core and each Provider projection
  bind the digest of those exact Request bytes, not only an advisory placement
  constraint.
- Every replacement role recomputes prefill over that complete prefix unless an
  exact protected checkpoint matches all decode-state fields and component
  schema digests.
- Old-attempt internal Data, external events, terminal Response, and callbacks
  remain fenced even if they arrive later.
- The new physical stream starts at cursor 1 under a new epoch and publishes
  only continuation tokens after the accepted prefix. The logical iterator
  appends them after the already committed prefix and never redelivers the
  prefix. Evidence retains both physical streams and the combined transcript.
- In every attempt, the terminal role alone owns external token events and
  End/Response. Stage0 may coordinate token feedback and forward the terminal
  marker, but it cannot publish application events or claim stream completion.

## 10a. Multi-turn conversation continuation

Conversation continuation is distinct from same-generation replacement. A
successful next turn has a fresh request ID, generation ID, plan commitment,
Selection authority, event stream, End, and terminal Response. It reuses only
the exact model state committed by the preceding conversation checkpoint; it
does not preserve the previous turn's request-scoped identity or stream.

Per-turn state machine:

```text
NEW_TURN
  FULL_CONTEXT -> full prefill -> normal decode loop
               -> CHECKPOINT_FINALIZE if terminal suffix is not represented
               -> all-role checkpoint commit
  APPEND_DELTA -> verify parent aggregate checkpoint
               -> verify exact prior Provider-role placement and every receipt
               -> request role-local state readiness
               -> [GPU hit | host prefetch -> GPU | unavailable]
               -> all-role readiness barrier
               -> delta prefill over appended tokens only
               -> normal decode loop
               -> CHECKPOINT_FINALIZE if terminal suffix is not represented
               -> compare-and-swap parent context epoch
               -> all-role successor checkpoint commit
```

Before any state lookup, the adapter canonicalizes the complete logical
transcript using the exact tokenizer and chat-template digests and proves:

```text
newCompleteTokenIds = parentCommittedTokenIds || appendedTokenIds
```

Only `appendedTokenIds` enter delta prefill. If the template/tokenizer changes
or the parent is not an exact prefix, reuse is rejected and only explicit full-
context fallback/failure remains. The resumed turn's `inferenceEpoch=0` is a
**delta-prefill transition**: it
consumes the appended input tokens and the parent checkpoint's complete
role-local state. Later epochs use the existing one-token autoregressive decode
contract. This is still one prefill transition per turn, but it is not a full
transcript prefill. Evidence distinguishes `FULL_PREFILL` and `DELTA_PREFILL`
and records actual input and represented-prefix extents.

The state produced when sampling stops is not assumed to represent the complete
turn. For example, the final accepted non-EOS token may have been sampled but
not fed back through every role. Before any role receipt, the adapter derives
the exact canonical completed-turn prefix and runs the bounded missing suffix
through `CHECKPOINT_FINALIZE`. This is a state-only pipeline transition: no
sampling, token event, cursor, End, or application callback is produced. If the
exact finalized prefix cannot be proven, checkpoint commit fails and the
request-local state cannot be reused by a later request. The sealed V1 maximum
is 32 finalization tokens; exceeding it is an explicit promotion failure, not a
reason to truncate the transcript or reuse partial state.

Each Provider verifies its own signed receipt, exact conversation-state key,
state component schema, and residence. It never downloads another role's state.
The coordinator admits execution only when all roles report the same parent
context epoch and logical prefix and the exact previous role map is selected.
One missing or incompatible role fails the barrier; the runtime cannot combine
resumed roles with newly zero-initialized roles.

Provider-local residency transitions are:

```text
GPU_RESIDENT --pause/capacity--> HOST_RESIDENT
HOST_RESIDENT --scheduled--> PREFETCHING --complete--> GPU_RESIDENT
GPU_RESIDENT --turn execution/commit--> PINNED --complete--> GPU_RESIDENT
HOST_RESIDENT|GPU_RESIDENT --expiry/reset/inactive LRU--> EVICTED
PREFETCHING --cancel/failure--> HOST_RESIDENT or EVICTED
```

State prefetch is single-flight, asynchronous, and deadline-bounded. The turn
waits at the role-readiness barrier rather than blocking the Face thread. Model
weights remain GPU resident; only the adapter-certified state bundle moves.
Pinned, committing, or dispatch-ready state cannot be evicted. Cancellation
releases the prefetch waiter and leaves the last committed checkpoint valid.

If the barrier cannot be satisfied, fallback is explicit. When the turn sealed
authenticated full context and `allowFullPrefillFallback=true`, the coordinator
records the miss reason and executes one full prefill on a new ordinary turn
state. Otherwise it returns `CONVERSATION_STATE_UNAVAILABLE` without invoking a
runner. A placement change always takes this fallback/failure path; Version 1
does not migrate state between Providers.

Successor commit is linear. The coordinator compares-and-swaps the expected
parent `contextEpoch`, accepts every role receipt for exactly `epoch+1`, and
only then exposes the aggregate checkpoint. Competing children of one parent
cannot both commit. Failure before aggregate commit discards all candidate
successors and preserves the parent checkpoint.

## 11. Failure codes

| Code | Trigger |
|---|---|
| `PROVIDER_LOST` | selected role becomes unreachable before deadline |
| `DEPENDENCY_MISSING` | exact required activation/feedback unavailable after budget |
| `DEPENDENCY_HASH_MISMATCH` | digest/size/contract mismatch |
| `DECODE_STATE_IDENTITY_MISMATCH` | any exact reuse field or component schema differs |
| `CACHE_MISS_RECOMPUTE_REQUIRED` | incremental input exists without compatible decode state/prefix |
| `NO_COMPATIBLE_REPLACEMENT` | replacement allowed but no valid plan/authority |
| `REQUEST_DEADLINE` | absolute request deadline reached |
| `ATTEMPT_CANCELLED` | explicit cancellation/tombstone |
| `EVENT_BACKPRESSURE_DEADLINE` | event cannot enter bounded queue before deadline |
| `EVENT_TERMINAL_MISMATCH` | End and Response or local transcript disagree |
| `CONVERSATION_CHECKPOINT_INVALID` | checkpoint authentication, expiry, requester/service/security, model/plan, prefix, or receipt-set validation fails |
| `CONVERSATION_STATE_UNAVAILABLE` | one or more exact role states are missing, evicted, restarted, or incompatible and full-prefill fallback is not authorized |
| `CONVERSATION_ROLE_STATE_INCOMPLETE` | selected roles do not all report one compatible parent epoch/prefix before execution |
| `CONVERSATION_PREFETCH_TIMEOUT` | host-resident state cannot reach the execution tier before the request deadline |
| `CONVERSATION_STATE_CONFLICT` | another turn already committed the same parent context epoch |

No failure code is converted to a successful partial Response.

## 12. Final result

The final application payload contains at least:

- model/tokenizer/adapter/sampling identities;
- prompt token count;
- complete generated token ID list;
- complete decoded text;
- successful finish reason;
- generated token count;
- transcript digest;
- accepted attempt/plan/generation identities.
- optional successor conversation checkpoint and context epoch, present only
  after every selected role committed the same successful turn state.

It is sealed by the existing Spec 174 result contract and carried by the one
authoritative `ResponseMessage` with `StreamCompletionV1`.
