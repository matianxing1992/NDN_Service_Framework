# Native multi-Provider epoch coordinator V1

This contract closes the implementation boundary identified by the Spec 175
audit. It is required for native I02, I03, and I15. It does not introduce a
second Request, a second placement protocol, or a second terminal owner.

## 1. Ownership and lifetime

The coordinator is request-scoped and is created only after the existing
Request/ACK/plan/Selection flow commits one `requestId`, `attemptEpoch`,
`planDigest`, `generationId`, and `streamEpoch`. It is a protocol state machine
distributed across the selected Providers, not a new network service.

Each selected Provider still owns exactly one complete role. The local handler
keeps one coordinator state for that role and the accepted attempt. It may
execute that role once for prefill and once for each decode epoch, but it may
not execute a different role or choose a new Provider. The final role alone
owns sampling, external events, End, and the terminal Response.

The coordinator is destroyed or fenced when the existing streamed invocation
is cancelled, fails, reaches its deadline, or completes its terminal handoff.
For an ordinary request that handoff releases request state. For a successful
conversation-enabled turn, it first transfers each exact committed role state
to the Provider conversation-state manager and publishes the role receipt; the
request-scoped coordinator is released only after aggregate commit/rollback.
A late epoch
must be rejected before ORT execution when any request, attempt, plan,
generation, stream epoch, role, Provider, or epoch field differs.

## 2. Epoch state

For every selected role, the coordinator retains only bounded local state:

```text
requestId, attemptEpoch, planDigest, generationId, streamEpoch
role, provider, inferenceEpoch, acceptedTokenEpoch
committed DecodeStateBundleV1 (or no state before prefill)
provider-local ProviderDecodeStateEntryV1 lease
```

`inferenceEpoch=0` is prefill. Epoch `e>0` consumes the exact feedback for
token epoch `e` and produces the next activation/state. `maxGeneratedTokens`
and the signed deadline bound the number of coordinator epochs. No epoch calls
the public service API, allocates another Request ID, repeats ACK discovery,
or commits another plan.

The lease is obtained automatically from the selected Provider's production
role session. For epoch `e>0` it must resolve the state committed by epoch
`e-1`; a caller or test harness cannot attach state outputs as the next role
inputs. The lease is pinned until the transition and its required publication
finish. Only inactive leases are eligible for deterministic bounded eviction.

## 3. Dependency operation allocation

The accepted capability must pre-authorize every activation and feedback edge
for the bounded epoch range. An operation index is not reused across epochs.
For a plan with `E` edge ordinals and a signed maximum of `K` generated tokens:

```text
operationIndex(prefill, edgeOrdinal) = base + edgeOrdinal
operationIndex(decodeEpoch e, edgeOrdinal) = base + (e + 1) * E + edgeOrdinal
```

The exact `base`, `E`, and `K` are part of the signed plan/capability digest.
The implementation must reject an epoch whose operation index is absent from
the capability instead of falling back to an unbound operation. A capability
with fewer operations than the requested maximum is a failed Selection, not a
runtime truncation.

Each generated DATA_V1 object includes the request, attempt, plan, group,
operation index, epoch, producer role/rank, consumer role, tensor digest, and
segment manifest. The epoch is also present in the exact planned Data name;
same-name reuse across epochs is forbidden.

Token feedback is an encoded TensorBundle, not the raw token scalar. Its
encoded size includes `input_ids` and the terminal marker and can vary with the
bundle codec. Therefore a feedback edge must leave `expectedBytes` unset (zero)
unless the exact encoded size is part of the signed plan; the authenticated
DATA_V1 manifest remains the byte-count authority. A raw token byte estimate
must never be used as the edge byte bound.

When a valid one-role plan makes TOKEN_FEEDBACK a self-edge, producer and
consumer are the same selected Provider process. That edge MUST use the bounded
process-local `DependencyIo` store, with the same request/attempt/plan/
generation/epoch key and single-consumption rule; it MUST NOT depend on an SVS
loopback fetch. This optimization never applies when producer and consumer are
different Providers. Cross-Provider activation and feedback remain signed,
manifest-validated DATA_V1 objects fetched by exact NDN name.

An activation edge may carry either its normal model tensor bundle or the
bounded terminal-control bundle described below. Its `expectedBytes` must
therefore also remain zero unless the plan explicitly seals both encodings.
The authenticated DATA_V1 manifest and the capability's `maxBytes`/
`maxSegments` bounds remain authoritative; zero does not disable integrity or
size enforcement.

## 4. Per-epoch execution

Every role uses the existing `NativeProviderRuntime`/`ProviderRoleWorker` once
for the current epoch and calls the checked-in native runner's unary `run()`
for one ONNX state transition. The multi-Provider coordinator does **not** call
`OnnxRuntimeModelRunner::runStreamed()` because that adapter hook owns an entire
single-role token loop and cannot consume activation from another Provider at
each epoch. `runStreamed()` remains a valid one-Provider compatibility seam.

Before invoking the runner, the Provider session supplies the exact committed
local state and the current external input: the protected prompt at prefill,
the new token at Stage 0 decode, or the current upstream activation at a later
stage. Dependency Data never carries another Provider's decode state. Every
activation/feedback dependency does carry `GenerationEpochLineageV1` inside
the encrypted DATA_V1 plaintext for the common logical token-prefix digest/
count and the exact position commitment. The coordinator verifies this metadata against its
request-scoped generation state; it MUST NOT hash role-local activation bytes
and use that digest as the model-token prefix identity. After the runner
returns, the adapter validates a complete candidate state. It becomes committed
only after the current output is accepted for downstream publication; otherwise
it is discarded and the predecessor remains committed.

The epoch sequence is:

1. Stage 0 obtains the protected prompt for prefill, or the exact final-role
   token-feedback object for a later epoch, and advances the canonical logical
   token-prefix lineage by exactly the admitted prompt or one feedback token.
2. Each nonfinal role fetches only its declared activation for this epoch,
   validates the DATA_V1 manifest plus the common prefix/count/position
   commitment, runs one state transition, and publishes one activation with the
   same authenticated logical lineage for its downstream role.
3. The final role fetches the final activation, runs one state transition,
   samples once, and constructs the token event.
4. Core admits the external event through the existing bounded writer. Only
   after admission does the final role commit the token/prefix and publish the
   internal feedback object for the next epoch.
5. On EOS/stop/max, the final role publishes a terminal token-feedback marker
   on the already authorized feedback edge to Stage 0. Stage 0 does not run
   ORT for that drain epoch; it publishes a terminal activation marker on its
   existing downstream activation edge. Every intermediate stage likewise
   verifies and forwards the marker without model execution, then terminates
   locally. This is bounded in-band drain control, not a new public Request or
   a broadcast edge.
6. The final role admits End and the one final Response through the existing
   terminal owner. No upstream role may publish an external event, End, or
   Response. The final Response may be delivered after its terminal feedback
   publication while upstream roles finish the bounded drain.

An execution or publication failure leaves the current local decode-state
   candidate uncommitted. After an externally admitted event, the current
   attempt terminates or follows the explicit replacement contract; it never
   silently rewinds the stream epoch.

For CUDA qualification, the runner keeps the complete state in device-resident
buffers through persistent I/O binding. A per-token complete-state host
materialization/re-upload is invalid even if token output is correct. Evidence
records state residence, logical bytes, host-transfer bytes/time, and lifecycle
counters without recording tensor contents.

For every production runner call, evidence also records the actual new-input
extent, logical prefix extent already represented by the committed state, and
machine-derived prefix work avoided. The registered CPU semantic control runs
the same input once through cached incremental execution and once through a
full-prefix reference, requires exact output parity, and rejects any reported
hit that invokes the full-prefix path. CUDA timing is deliberately deferred to
G5 so a noisy CPU latency comparison cannot establish or refute cache
effectiveness.

### 4a. Conversation turn entry and exit

A later conversation turn creates a new request-scoped coordinator; it never
reuses the previous coordinator object. Before epoch 0, the automatic planner
verifies the parent aggregate checkpoint, protected User-side transcript, exact
same Provider-role map, and every role receipt. Each Provider independently
acquires its `ConversationStateEntryV1` and reports state readiness. No role may
start until the all-role readiness barrier passes.

For `APPEND_DELTA`, the adapter proves that canonical tokenization of the new
complete transcript equals the parent token sequence plus one exact appended
suffix. Epoch 0 consumes that suffix and the promoted Provider-local role state;
it is `DELTA_PREFILL`, not a second full prefill. Its successful output becomes
the first request-scoped `ProviderDecodeStateEntryV1` candidate for this turn.
Every later epoch follows Sections 3--4 unchanged.

At terminal, every role commits a `ConversationStateCandidate`, signs its
receipt, and retains the prior committed conversation entry until the aggregate
compare-and-swap finishes. Aggregate success promotes all candidates and
releases the parent; failure discards candidates and leaves the parent usable.
The application result/checkpoint futures remain pending until this outcome.
No tensor or local state handle is sent through activation, feedback, receipt,
event, or Response Data.

## 5. Required implementation seams

The implementation must provide a reusable native helper (or equivalent
request-scoped object) with these observable operations:

```text
begin(request/attempt/plan/generation/stream binding)
runPrefill(role, inputs)
runEpoch(epoch, role, inputs)
acquireCommittedState(role, predecessorEpoch)
commitCandidateState(role, epoch)
discardCandidateState(role, epoch, reason)
releaseState(role, reason)
publishActivation(epoch, edge, bundle)
publishTokenFeedback(tokenEpoch, feedback)
admitTokenEvent(event)
finish(reason, finalPayload)
beginConversationTurn(checkpoint, expectedParentEpoch, mode)
awaitConversationStateReady(role, deadline)
promoteConversationCandidate(role, successorEpoch)
commitConversationCheckpoint(expectedParentEpoch)
rollbackConversationCandidate(reason)
cancel/fail(reason)
```

The helper may be implemented inside `NativeProviderHandler` and
`ProviderRoleWorker`, but the epoch loop must not be hidden inside a test-only
runner or a Python oracle. It must expose machine-readable lineage for
prefill, each role execution, activation fetch/publication, feedback, event,
End, and Response. Sensitive prompt, answer, logits, keys, and state tensors
remain absent from logs.

The helper must reach the Provider-owned store from the same production call
used by `executeRoleAsync`. A direct store-method test, a direct adapter loop,
or a harness assignment from `*_out` to `*_in` is prerequisite evidence only.

The Provider handler supplies one complete sealed static identity template per
role from the validated artifact/projection/runtime authority. The coordinator
must reject a missing template before prefill. It then supplies an exact
predecessor and candidate `DecodeStateIdentityV1` to the runtime on every
transition. A free-form runner metadata map, generated placeholder digest,
session ID, raw activation digest, or loop counter alone is not an identity
authority.

## 6. Formal-case boundary

I02 and I03 must use this coordinator through the production C++ Request/ACK/
Selection/`NativeProviderHandler` path with the checked-in tiny ONNX graphs and
fresh processes. I15 must run the same path after an ACK capability/residency
permutation and prove that the role-to-Provider map changes while the token
oracle and request ID remain unchanged.

I01-I03/I15 must also prove one automatic state commit after prefill, one exact
hit and atomic successor commit for every later role epoch, zero state-bundle
objects on dependency edges, and bounded cleanup for every selected Provider.

I16/I17 must create a fresh coordinator for the second turn, restore one exact
conversation entry per role, perform delta prefill over only the appended token
suffix, and commit one successor checkpoint with exact full-transcript oracle
parity. I18--I20 own tiering, mutation/fallback, conflict, and cancellation
boundaries. None may call a test-only state feedback loop.

The following are prerequisites only and cannot be registered as I02/I03/I15:

- the Python one-/two-/four-role oracle;
- a direct `OnnxRuntimeModelRunner::runStreamed()` test;
- a deterministic native two-role runner;
- a generic one-Provider Core stream fault test.

## 7. Fail-closed checks

The coordinator must reject:

- missing or duplicate capability operation indexes;
- an epoch/name/operation mismatch;
- activation or feedback from the wrong Provider or role;
- a second prefill, second sampling decision, or second terminal claim;
- a token event admitted without the matching feedback lineage;
- a terminal activation that is not an authenticated one-byte boolean marker
  inside an encoded tensor bundle;
- a late old-attempt event or response;
- a `runStreamed()` multi-role fallback that hides missing per-epoch edges;
- a decode epoch that silently creates zero state after a miss or mismatch;
- a static identity field that is absent or supplied only by unvalidated
  free-form runner metadata;
- a prefix identity derived from role-local activation/tensor bytes instead of
  the authenticated common model-token lineage;
- a static `positionDigest`, missing state/predecessor epoch, noncontiguous
  predecessor, or cache-incarnation epoch that changes inside one lineage;
- any runner invocation after predecessor lookup or complete-identity
  validation fails;
- caller/harness-provided state feedback on a formal production-path case;
- an external SVS loopback dependency for a one-role TOKEN_FEEDBACK self-edge,
  or a process-local substitute for any cross-Provider edge;
- eviction of a pinned entry or a candidate overwriting committed state before
  output/publication acceptance;
- CUDA qualification that round-trips the complete decode state through host
  memory on each token.
