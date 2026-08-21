# Research and Design Decisions: NDNSF-DI Streamed Invocation

## Scope of this artifact

This document resolves the architecture choices needed before implementation.
It records repository evidence, the selected decision, rationale, and rejected
alternatives. It does not claim that the new stream path is implemented or
measured.

## Current code facts

- `ServiceUser::RequestService` and `RequestServiceTargeted` currently terminate
  through one `ResponseMessage` callback; there is no request-scoped typed event
  handle.
- `ProviderRuntimeContext` already owns exact collaboration publication/fetch,
  a deadline-bound dependency interface, a one-terminal-response guard, and
  `publish_final_response`.
- `QwenGenerationSessionStateMachine` already tracks attempt epoch, token epoch,
  bounded replacement, deadline, cancellation, and one terminal-response claim.
  Its current `complete()` rejects successful completion before exactly
  `maxGeneratedTokens`, so EOS/stop completion needs an explicit correction.
- The existing predictive `StreamFacade` is designed for long-lived media-style
  sessions with mapping, retention, prefetch, and optional FEC. Those semantics
  are not the same as one service invocation with one terminal Response.
- The deployed Qwen boundary already uses standalone `tokenizers` and documents
  that Transformers belongs only to offline export/conformance.
- Spec 174 requires a complete final application result, exact attempt/plan/
  generation fencing, exact reuse compatibility, and one Provider per complete
  role.

## Decision 1: Add a generic streamed invocation beside unary invocation

**Decision**: Keep `RequestService` unchanged and add exactly
`RequestServiceStreaming<RequestT, EventT, ResponseT>` with one shared handle.
LLM token events are application payloads over this generic contract.

**Rationale**: Unary object detection and streamed generation have different
delivery lifecycles but share discovery, authorization, placement, execution,
and final-response semantics. One generic event type avoids contaminating Core
with LLM concepts.

**Alternatives considered**:

- Return an iterator from every `RequestService`: rejected because it changes
  existing unary API and error semantics.
- Add `RequestLlmStreaming`: rejected because Core is application-neutral.
- Send one normal service request per token: rejected because it repeats ACK,
  placement, Selection, preparation, and prefill.

## Decision 2: Use exact event names without predictive mapping

**Decision**: Event Data names are a deterministic function of producer,
requester, service, request, attempt, plan, generation, stream epoch, and cursor.
The user maintains a bounded exact-Interest window.

**Rationale**: All components are known after the plan is accepted. Exact names
permit NDN caching, retry, and signature validation without publishing a second
mapping object or applying media-specific prediction.

**Alternatives considered**:

- Reuse `StreamFacade`: rejected because its mapping/FEC/session contract would
  create a second lifecycle and terminal authority for one request.
- SVS-only event callbacks: rejected as the acceptance path because Sync is a
  notification mechanism and does not by itself provide exact ordered cursor
  recovery and final transcript closure.
- One forever-pending Interest: rejected because it is not the repository's
  current exact Data pattern and complicates caching/recovery.

## Decision 3: One stream protection epoch, individual signed events

**Decision**: Establish one 256-bit AES-GCM key through the authorized protected
request binding. Each event is separately encrypted and signed with its exact
name/header as associated data. Replacement creates a new key and stream epoch.

**Rationale**: Repeating ABE key wrapping for every token adds avoidable control
and wire overhead. A session key keeps per-event cost bounded while exact names,
AEAD, signatures, request tokens, and attempt fencing preserve integrity and
authorization.

**Alternatives considered**:

- ABE-encrypt every event: rejected as unnecessary repeated public-key work.
- Sign only the End/Response: rejected because individual events can be cached,
  reordered, replayed, or delivered before completion.
- Reuse one key across replacement attempts: rejected because it weakens fencing.

## Decision 4: One prefill, incremental decode, exact local KV identity

**Decision**: Prefill the prompt once per clean attempt. Each later epoch uses
the new token plus provider-local KV only after all `KvStateIdentityV1` fields
match. Persist ONNX Runtime sessions and bound buffers for the generation.

**Rationale**: Recomputing full context per token cannot meet interactive TPOT.
Loose cache keys risk accepting state from a different graph, split, position,
runtime, authority, or provider boot.

**Alternatives considered**:

- Full-context decode every token: retained only as explicit clean recomputation,
  not the normal path.
- Cache by logical session and token count: rejected as insufficient proof.
- Live cross-Provider KV transfer: deferred because it requires a separately
  protected layout/compatibility/migration protocol.

## Decision 5: Final role owns sampling and two distinct publications

**Decision**: The final role samples one token and publishes (a) internal token
feedback for the first role and (b) an external application event for the user.
It later publishes one complete final Response.

**Rationale**: Sampling belongs where final logits exist. Internal feedback and
external delivery have different consumers, keys, names, retention, and failure
semantics. Keeping them distinct makes packet lineage and authorization
testable. Sampling remains part of the final role, preserving one Provider per
complete role.

**Alternatives considered**:

- Add a separate sampling Provider/role: rejected because it adds placement and
  transport without need in the baseline.
- Let the user sample: rejected because logits are large and would add a full
  activation transfer for every token.
- Treat one stage as multiple Provider-owned pieces: rejected by the current
  role-ownership invariant; TensorGroup remains separate future work.

## Decision 6: Deliver one token per event in version 1

**Decision**: One accepted cursor carries one token ID and its incremental text
delta. The End event may be terminal-only and does not require another token.

**Rationale**: This gives the simplest ordering, retry, and TTFT/TPOT evidence.
At the target 20 token/s, a 64-entry queue and 16-Interest window provide bounded
headroom. Micro-batching would trade latency for wire efficiency and needs data.

**Alternatives considered**:

- Fixed multi-token chunks: deferred until measured per-event overhead justifies
  the extra buffering latency.
- Arbitrary byte stream: rejected because typed event boundaries and exact
  cursor recovery are required.

## Decision 7: Backpressure, never silent event loss

**Decision**: Queue admission precedes cursor/token-epoch commitment. A full
publisher or callback queue blocks boundedly until space, deadline, cancellation,
or declared failure. No drop mode exists in version 1.

**Rationale**: A dropped token would make the final transcript inconsistent and
could not be repaired if cursor advancement were already committed.

**Alternatives considered**:

- Drop oldest/newest: rejected for correctness.
- Unbounded queues: rejected for resource safety.
- Continue generation and reconstruct only from final Response: rejected because
  it falsely reports a successful live stream.

## Decision 8: Replacement is explicit and disabled by default

**Decision**: `allowReplacement=false`, `maxReplacements=0`. An application may
set `allowReplacement=true`, which fixes `maxReplacements=1` and authorizes clean
recomputation. A new attempt/stream epoch is mandatory.

**Rationale**: After selection, the framework cannot always know whether a
provider executed and only its output was lost. Default automatic re-execution
would be unsafe for non-idempotent work. LLM generation can opt into deterministic
recomputation from prompt plus accepted token prefix.

**Alternatives considered**:

- Always retry on response/event timeout: rejected because execution ambiguity
  remains.
- Never recover: rejected because explicitly idempotent generation can safely
  use bounded recomputation.

## Decision 9: End event plus authoritative final Response

**Decision**: End declares stream closure and transcript/result digests; the
existing final Response remains the authoritative complete application result.
Success is delivered only after all declared cursors and both digests agree.

**Rationale**: This preserves Spec 174 and lets the event consumer know the
finite cursor range without redefining Response semantics.

**Alternatives considered**:

- Make End the final result: rejected because it breaks unary/common result
  handling and Spec 174.
- Omit End and infer completion from Response: rejected because a response can
  race ahead of missing event cursors.

## Decision 10: Gate Tiger on the exact local SIF path

**Decision**: Run static/contract, unit, CPU integration, exact-SIF CPU MiniNDN,
and SIF native preflight before Tiger. Tiger verifies the local SIF hash and does
not rebuild or materialize another image.

**Rationale**: Most protocol, import, path, ABI, and bundle failures can and must
be found locally. Tiger is reserved for CUDA and distributed hardware evidence.

**Alternatives considered**:

- Build on Tiger: rejected because it reintroduces environment drift and
  expensive iterative failures.
- Use a host-built Python extension inside SIF: prohibited; the candidate SIF or
  ABI-identical sealed builder is the only container ABI authority.
- Run only unit tests locally: rejected because they do not exercise NFD/SVS,
  signing, exact Data, role processes, artifact paths, or SIF imports.

## Decision 11: Separate functional and performance verdicts

**Decision**: Correctness is mandatory. `PERFORMANCE_PASS` additionally requires
median warm steady-state >=20.0 token/s and p95 inter-token <=75 ms. A correct
slower run is `FUNCTIONAL_PASS_PERFORMANCE_MISS`, not a failed correctness run or
evidence for the performance claim.

**Rationale**: This prevents tuning or hiding negative runs to force a desired
claim and keeps bottleneck diagnosis honest.

**Alternatives considered**:

- Make 20 token/s a correctness gate: rejected because it conflates protocol
  validity with hardware/model performance.
- Report only mean throughput: rejected because TTFT and tail TPOT determine
  interactive behavior.

## Deferred decisions requiring a future Spec

- Cross-Provider TensorGroup/rank roles and collectives.
- Continuous batching across unrelated users.
- Speculative decoding and draft models.
- Live protected KV migration between Providers.
- Event micro-batching/adaptive windowing.
- OpenAI-compatible HTTP/SSE server ownership and API compatibility.
