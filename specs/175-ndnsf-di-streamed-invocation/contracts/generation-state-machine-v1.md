# Generation Contract: Incremental ONNX Pipeline V1

## 1. Placement and ownership

- Default strategy: `PreSplitFirstStrategy`.
- Plan shape: ordered pipeline roles `Stage0..StageN-1`, where `N` is 1, 2, or
  4 in Spec 175 validation.
- Mapping is one-to-one: each role has one Provider; each selected Provider has
  one role; no Provider owns two roles; no role spans Providers.
- `StageN-1` is the final role and owns final logits, sampling, incremental text
  decoding, external events, End, and final Response.
- Tensor/rank groups are not valid plan objects in this contract.

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

- `attemptEpoch` starts at 0 and advances only for accepted replacement.
- `inferenceEpoch=0` is prefill over the full prompt.
- A successful prefill produces generated `tokenEpoch=1`.
- For `k>=1`, decode `inferenceEpoch=k` consumes sampled token `k` plus exact KV
  and produces sampled `tokenEpoch=k+1`.
- External Application event cursor equals `tokenEpoch` in version 1.
- End cursor equals `generatedTokenCount + 1`.
- Therefore `maxGeneratedTokens <= maxEvents - 1`.
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

## 6. Per-epoch pipeline order

For inference epoch `e`:

1. Stage0 waits for prompt input (`e=0`) or token feedback (`e>0`).
2. Every stage verifies attempt, plan, generation, dependency name, upstream
   role/Provider, tensor contract, and deadline.
3. The stage verifies/creates exact local KV identity, executes its persistent
   ONNX Runtime session, and commits new KV only after successful output.
4. Nonfinal stages publish one activation for the next stage.
5. The final stage samples once from final logits.
6. The final stage uses the incremental tokenizer state to compute a token event
   and determines EOS/stop/max.
7. The final stage admits the external event. Only after admission does it
   commit token epoch/prefix and publish internal feedback.
8. If nonterminal, the next decode epoch starts. If terminal, the final stage
   admits End and one complete Response and enters Draining.

No role polls an unspecified name and no token epoch creates a new service
Request or placement plan.

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
| `maxGeneratedTokens` | 64 in validation | 1..`maxEvents-1` |
| `eosTokenIds` | adapter manifest | nonempty bounded set |
| `stopStrings` | workload manifest | max 16 entries, each <=256 UTF-8 bytes |

All mandatory correctness and Tiger performance cases use Greedy mode. A
separate deterministic unit suite fixes logits and seed for
`SeededTopKTopP`; hardware-crossing exact token claims are not made for
stochastic floating-point logits.

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

## 9. KV state and atomic update

`KvStateIdentityV1` must match every field listed in `data-model.md`. Runtime
objects additionally hold the ORT session, named input/output bindings, device
buffers, and per-layer KV tensors. These are local opaque handles and are never
serialized into normal evidence.

Epoch update is transactional:

```text
verify old identity -> execute into candidate buffers -> validate output ->
admit downstream activation/event -> swap candidate KV into committed KV
```

Failure before swap leaves the prior committed KV usable for the same exact
attempt/epoch retry. Failure after an externally visible token commit terminates
or starts a new replacement attempt; it never rewinds that cursor inside the
same stream epoch.

## 10. Replacement and accepted prefix

- Default: disabled.
- Opt-in: exactly one replacement.
- Replacement input consists of the original protected prompt plus the ordered
  token IDs whose external events were committed before failure.
- The user includes the accepted-prefix digest/count in the new attempt plan.
- Every replacement role recomputes prefill over that complete prefix unless an
  exact protected checkpoint matches all KV fields.
- Old-attempt internal Data, external events, terminal Response, and callbacks
  remain fenced even if they arrive later.
- The new stream starts at cursor 1 under a new epoch; the application-level
  iterator suppresses already committed logical token prefix and delivers only
  the continuation after it verifies prefix equality. Evidence retains both
  physical streams and the logical transcript.

## 11. Failure codes

| Code | Trigger |
|---|---|
| `PROVIDER_LOST` | selected role becomes unreachable before deadline |
| `DEPENDENCY_MISSING` | exact required activation/feedback unavailable after budget |
| `DEPENDENCY_HASH_MISMATCH` | digest/size/contract mismatch |
| `KV_IDENTITY_MISMATCH` | any exact reuse field differs |
| `CACHE_MISS_FULL_CONTEXT_REQUIRED` | incremental input exists without compatible KV/prefix |
| `NO_COMPATIBLE_REPLACEMENT` | replacement allowed but no valid plan/authority |
| `REQUEST_DEADLINE` | absolute request deadline reached |
| `ATTEMPT_CANCELLED` | explicit cancellation/tombstone |
| `EVENT_BACKPRESSURE_DEADLINE` | event cannot enter bounded queue before deadline |
| `EVENT_TERMINAL_MISMATCH` | End and Response or local transcript disagree |

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

It is sealed by the existing Spec 174 result contract and carried by the one
authoritative `ResponseMessage` with `StreamCompletionV1`.
