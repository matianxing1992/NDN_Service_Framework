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
- The current normal dynamic API already has a service-only overload, while
  `AutomaticPlanningCoordinator` uses `begin_collaboration` followed by one
  ACK-closed `commit_plan`. The streamed DI path must extend those owners rather
  than require an application-supplied Provider list or send a second Request
  within one healthy attempt.
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

**Decision**: Keep `RequestService` unchanged and add a primary service-only
`RequestServiceStreaming<RequestT, EventT, ResponseT>` with one shared handle.
The same function name has a single-target overload for Targeted mode; Normal
mode never requires a Provider vector. For multi-role DI,
`AutomaticPlanningCoordinator.request_streaming` attaches the stream lifecycle
to the existing deferred `BeginCollaboration` request and
`CommitCollaborationPlan`; it never sends a second service Request within a
healthy attempt. The sole opt-in replacement is a new internal attempt Request,
not another public API call or a per-token Request. LLM token events are
application payloads over this generic Core contract.

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
- Require a Provider vector on the Normal API: rejected because it moves
  request-scoped discovery into the application and contradicts the model/task-
  first NDNSF-DI surface.
- Start `RequestServiceStreaming` after collaboration plan commit: rejected
  because it creates a second Request/control plane and terminal owner.

For replacement, reusing the old ACK snapshot, ProviderToken, plan, or
Selection is also rejected: those authorities are one-time and attempt-bound.
The bounded recovery path therefore creates one fresh internal Normal Request
with attempt 2, while preserving the public logical handle and generation ID.

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

**Decision**: Generate one 256-bit AES-GCM key per stream epoch. A Normal Request
contains only its digest commitment. After ACK closure and plan commit, the
final-role Provider receives a certificate-wrapped key envelope through its
Provider-specific Selection projection; no unselected ACK Provider receives a
decryptable grant. A selection-free Targeted request may carry the same key
wrapped only to its explicit target certificate. Each event is separately
encrypted and signed with its exact name/header as associated data. Replacement
creates a new key, commitment, grant, and stream epoch.

**Rationale**: Repeating ABE key wrapping for every token adds avoidable control
and wire overhead. A session key keeps per-event cost bounded, while Selection-
time least-privilege projection preserves the distinction between discovery
authority and execution authority. Exact names, AEAD, signatures, request
tokens, and attempt fencing preserve integrity and authorization.

**Alternatives considered**:

- ABE-encrypt every event: rejected as unnecessary repeated public-key work.
- Put a usable key grant in the Normal Request: rejected because every service-
  authorized candidate could decrypt it before Selection.
- Sign only the End/Response: rejected because individual events can be cached,
  reordered, replayed, or delivered before completion.
- Reuse one key across replacement attempts: rejected because it weakens fencing.

## Decision 4: One prefill, incremental decode, exact local decode-state identity

**Decision**: Prefill the prompt once per clean attempt. Each later epoch uses
the new token plus the complete provider-local `DecodeStateBundleV1` only after
all `DecodeStateIdentityV1` fields and component schemas match. For Qwen3.6 the
bundle includes both full-attention KV tensors and linear-attention
convolution/recurrent state. Persist ONNX Runtime sessions and bound buffers for
the generation. The production Provider role session—not the User or test
harness—owns exact lookup, candidate validation, atomic commit, pin/evict, and
cleanup. CUDA qualification keeps the complete state in device-resident buffers
through persistent I/O binding and measures a bounded same-artifact cached/full-
prefix control. Static identity fields come from the verified artifact,
accepted Provider projection, and loaded runtime. Dynamic identity commits the
canonical model-token prefix and adapter position state, plus explicit state and
predecessor inference epochs. Every role at one epoch shares the same logical
prefix digest/count; role-local activation bytes are not cache-prefix identity.

**Rationale**: Recomputing full context per token cannot meet interactive TPOT.
Loose cache keys risk accepting state from a different graph, split, token
prefix, position, state predecessor, runtime, authority, or provider boot. A
role-local activation digest also cannot prove which token sequence the model
has consumed. A host-materialized full-state round trip
can preserve correctness while erasing the performance benefit, and a manual
test feedback loop can hide a completely unwired Provider cache; both therefore
need independent rejection criteria.

**Alternatives considered**:

- Full-context decode every token: retained only as an oracle/diagnostic or
  bounded recovery recomputation, never as the healthy Spec175 path or G6/G7
  qualification subject.
- Cache by logical session and token count: rejected as insufficient proof.
- Caller/harness-managed `state_out -> state_in`: retained only as an adapter
  prerequisite; rejected as production or formal integration evidence.
- Per-token host `TensorBundle` materialization on CUDA: diagnostic-only;
  rejected for G5-G7 because it does not establish effective device reuse.
- Live cross-Provider decode-state transfer: deferred because it requires a separately
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
- Treat one stage as multiple Provider-owned pieces: rejected by the pipeline
  role-ownership invariant. The existing Spec 174 TensorGroup/rank-role protocol
  remains separate and unchanged; Spec 175 does not extend streaming to it.

## Decision 6: Deliver one token per event in version 1

**Decision**: One accepted cursor carries one token ID and its incremental text
delta. The End event may be terminal-only and does not require another token.
The Qwen3.6 subject uses its language-model-only text path with MTP/speculative
decoding disabled, so one decode epoch has one sampled token and one causal
feedback edge.

**Rationale**: This gives the simplest ordering, retry, and TTFT/TPOT evidence.
At the target 20 token/s, a 64-entry queue and 16-Interest window provide bounded
headroom. Micro-batching would trade latency for wire efficiency and needs data.

**Alternatives considered**:

- Fixed multi-token chunks: deferred until measured per-event overhead justifies
  the extra buffering latency.
- Qwen MTP/speculative decoding: deferred because it introduces a different
  acceptance, rollback, event, and feedback contract; it cannot be enabled as a
  performance-only switch under the V1 experiment.
- Vision encoder/projector deployment: excluded because the registered workload
  is text-only and including unused multimodal components would change memory,
  staging, and runtime claims without testing them.
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

**Decision**: Run static/contract, unit, CPU integration, and host/CPU MiniNDN
before building the final SIF. After those gates pass, build one SIF and run its
native preflight. Then keep MiniNDN, Mininet, Open vSwitch, NLSR, topology, and
process supervision on the host while launching NFD and all NDNSF/ORT
application processes from that exact SIF inside the host-created namespaces.
Tiger verifies the local SIF hash, runs it directly under Slurm without
MiniNDN, and does not rebuild or materialize another image.

**Rationale**: Most protocol and network failures must be found before any SIF
build; import, path, ABI, and bundle failures are checked once at the final
local SIF boundary. Separating the host emulator from the application image
avoids turning MiniNDN/Mininet/OVS/NLSR packaging into a false release
requirement. Tiger is reserved for CUDA and distributed hardware evidence.

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

## Decision 12: Make conversation continuation explicit and keep state Provider-local

**Decision**: Add an optional, authenticated conversation checkpoint above the
existing request-scoped state contract. The first turn sends full context. A
later turn sends appended input plus the opaque parent checkpoint, receives a
fresh Request/generation, reuses only the exact prior one-to-one Provider-role
placement, performs delta prefill, and enters the same automatic decode loop.
Each Provider retains only its complete role state. Paused state may move
between GPU and bounded host RAM, while model weights remain GPU resident. The
coordinator returns a successor checkpoint only after every role commits the
same context epoch and logical prefix.

**Rationale**: Recomputing the complete transcript for every turn defeats the
purpose of KV/recurrent-state reuse. Reusing state by prompt equality,
`conversationId`, or one role's hit is unsafe because model, plan, authority,
role, prefix, or Provider incarnation may differ. The aggregate checkpoint and
per-role receipts make continuation exact without carrying state tensors or
requiring the application to manage Provider addresses.

**Alternatives considered**:

- Send full context on every turn: retained as the compatibility and explicit
  fallback path, but rejected as the only API because it repeats prefix work.
- Let the application store/pass KV tensors: rejected because it exposes model-
  specific state, enlarges NDN traffic, and breaks Provider ownership.
- Replan onto different Providers and migrate state: deferred because it needs
  a separate protected state-transfer protocol and failure model.
- Keep all conversations permanently on GPU: rejected as unbounded. Version 1
  uses bounded GPU plus host RAM, deterministic inactive LRU, and no disk tier.
- Infer continuation from equal prompt bytes: rejected because it is ambiguous,
  privacy-sensitive, and not an authorization proof.
- Allow multiple concurrent successors: deferred; Version 1 uses linear
  compare-and-swap context epochs so two turns cannot silently fork or mix
  role state.

## Deferred decisions requiring a future Spec

- Cross-Provider TensorGroup/rank roles and collectives.
- Continuous batching across unrelated users.
- Speculative decoding and draft models.
- Live protected decode-state migration between Providers.
- Branching/merging conversation histories and cross-conversation shared-prefix
  caching.
- Disk/NVMe conversation-state spill and multi-tenant continuous batching.
- Event micro-batching/adaptive windowing.
- OpenAI-compatible HTTP/SSE server ownership and API compatibility.
