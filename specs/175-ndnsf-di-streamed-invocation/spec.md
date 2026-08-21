# Feature Specification: NDNSF-DI Streamed Invocation

**Feature Branch**: `Experimental`

**Created**: 2026-08-21

**Status**: Draft for implementation

**Input**: Extend the verified NDNSF-DI design so a selected multi-Provider ONNX plan can produce a local-LLM-like ordered token stream, while preserving one complete terminal Response, unary compatibility, request-scoped placement, security, and a staged unit/integration/MiniNDN/TigerCluster validation ladder.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Consume One Ordered Streamed Invocation (Priority: P1)

An application submits one service request, receives zero or more typed progress
events while the request is executing, and then receives exactly one complete
terminal result. An LLM application sees incremental token text before the full
answer is complete; a unary application continues to use the existing one-result
API without any behavior change.

**Why this priority**: Without a stable streamed-invocation contract, the
distributed LLM can only appear as many unrelated requests or one delayed final
response, neither of which behaves like a normal interactive LLM.

**Independent Test**: A deterministic in-process provider emits a known event
sequence followed by one result. The client observes the events in cursor order,
delivers each cursor once, obtains the complete final result, and the unchanged
unary test suite still passes.

**Acceptance Scenarios**:

1. **Given** an authorized streamed request and a healthy provider, **When** the
   provider emits five events and one final result, **Then** the application
   receives cursors 1 through 5 exactly once before the final result.
2. **Given** a request that legitimately completes without intermediate events,
   **When** the provider publishes the final result, **Then** the client returns
   that result without waiting for an event that will never exist.
3. **Given** an existing unary service, **When** it is invoked through the
   existing unary API, **Then** it still produces exactly one final response and
   exposes no streamed lifecycle.
4. **Given** an active streamed invocation, **When** the application cancels it,
   **Then** no later event or final result is delivered to the application.

---

### User Story 2 - Generate Tokens Through One Selected ONNX Plan (Priority: P1)

An LLM application performs one request/ACK/placement/Selection cycle, runs
prefill once, and then repeatedly decodes through the same selected pipeline.
The final role returns each sampled token both to the first role for the next
decode epoch and to the user as an ordered application event. Provider-local KV
state is reused only when its exact identity remains compatible.

**Why this priority**: Repeating discovery, placement, model preparation, and
prefill for every token would dominate latency and would not be true streaming
generation.

**Independent Test**: A CPU-only small canonical ONNX fixture with fixed token
inputs and seed runs one prefill plus multiple decode epochs across a fixed
one-to-one role/Provider plan. The emitted token IDs and final transcript match
the local oracle, and evidence proves that placement and prefill occurred once.

**Acceptance Scenarios**:

1. **Given** a selected multi-stage plan, **When** generation produces N tokens,
   **Then** there is one Request, one closed ACK snapshot, one committed plan,
   one Selection set, one prefill, N decode epochs, N ordered token events, and
   one complete terminal Response.
2. **Given** a valid provider-local KV entry for the exact accepted prefix,
   **When** the next token epoch starts, **Then** the provider reuses that entry
   and processes only the incremental input required by its role.
3. **Given** any mismatch in model, adapter, role split, prefix, position,
   precision/layout, runtime ABI, security domain, provider boot epoch, attempt,
   or generation identity, **When** reuse is considered, **Then** the KV entry is
   rejected and clean computation or a new plan is used.
4. **Given** EOS, an accepted stop sequence, maximum generated-token count,
   deadline, or cancellation, **When** that condition occurs, **Then** generation
   terminates with the corresponding reason without requiring the maximum token
   count to be reached for EOS or stop completion.

---

### User Story 3 - Recover Without Mixing Attempts or Duplicating Output (Priority: P2)

A user receives a correct stream despite Data duplication, bounded loss,
reordering, delayed packets, cancellation, or one bounded replan. Old-attempt
events and responses cannot enter the accepted transcript, and the framework
does not guess that execution is safe to repeat when that would violate
application semantics.

**Why this priority**: Streaming exposes partial progress over time. Recovery
must preserve both NDN delivery semantics and the verified-delivery attempt,
plan, authorization, and final-result invariants established by Spec 174.

**Independent Test**: Deterministic fault injection duplicates, reorders, drops,
delays, replays, and tampers with events; cancels an invocation; and replaces a
provider once. The user either reconstructs the exact oracle transcript under
the accepted attempt or terminates with a specific error, never a mixed or
silently incomplete result.

**Acceptance Scenarios**:

1. **Given** duplicate and reordered event Data, **When** all required cursors
   become available before the deadline, **Then** callbacks occur once in cursor
   order and the final transcript digest matches.
2. **Given** a missing cursor within the retained window, **When** its retry
   budget remains, **Then** the user re-expresses the exact Interest and resumes
   ordered delivery after the gap is filled.
3. **Given** a stale or replayed event from another attempt, plan, generation, or
   stream epoch, **When** it arrives, **Then** it is rejected before application
   delivery.
4. **Given** a final-role failure and no exactly compatible KV checkpoint,
   **When** bounded replacement is enabled for the application request, **Then**
   a new attempt recomputes from the prompt plus the committed accepted-token
   prefix; it does not silently migrate incompatible KV state.
5. **Given** a non-idempotent application that has not opted into replacement,
   **When** the selected provider fails after execution may have begun, **Then**
   the request terminates rather than being re-executed automatically.

---

### User Story 4 - Qualify Before Spending TigerCluster Resources (Priority: P3)

A developer can prove the complete streamed path with unit tests, CPU-only
integration tests, and a representative CPU MiniNDN topology before promoting
one immutable SIF to TigerCluster. Tiger then verifies real ONNX Runtime CUDA
execution and measures interactive generation without using a different code or
artifact path.

**Why this priority**: Previous distributed-inference work lost time when ABI,
bundle, path, or protocol defects reached Tiger before local gates exercised the
same path.

**Independent Test**: Each gate consumes the same versioned contract and emits
a manifest. Tiger submission is refused unless all lower gates pass and the
candidate SIF passes the native runtime preflight.

**Acceptance Scenarios**:

1. **Given** any failed unit, integration, MiniNDN, security-negative, or SIF
   preflight gate, **When** promotion is requested, **Then** Tiger submission is
   refused with the failed gate named.
2. **Given** all local gates pass, **When** the exact SIF hash is promoted,
   **Then** Tiger verifies that hash, runs the same workload contract, proves
   CUDA ONNX Runtime execution with zero CPU fallback, and emits correctness and
   latency evidence.
3. **Given** a functional run whose throughput is below the performance target,
   **When** results are summarized, **Then** it is reported as functional but not
   performance-qualified; no 20-token/s claim is made.

### Edge Cases

- A final Response arrives before one or more already-declared event cursors.
- An End event is duplicated, arrives after cancellation, or disagrees with the
  terminal Response's cursor count or transcript digest.
- The provider completes before emitting any event.
- An event is validly signed but belongs to a different request, attempt, plan,
  generation, provider boot epoch, or stream epoch.
- A cursor gap is outside the provider retention window.
- An event payload is larger than the configured event wire-size limit.
- The application callback throws, blocks, or consumes more slowly than tokens
  are produced.
- The final-stage output queue reaches capacity.
- EOS is generated before the requested maximum token count.
- A stop string crosses token boundaries and requires incremental decoding.
- Unicode text is split across tokenizer pieces.
- The selected provider restarts and loses its KV state while old signed Data
  remains retrievable.
- One pipeline role fails before publishing its activation, after publishing
  its activation, or after the final role samples a token.
- Cancellation races with an event publication or the terminal Response.
- A malicious peer replays a valid ciphertext under a different cursor name.
- A unary response payload happens to contain fields resembling a stream event.
- Two concurrent invocations use the same model and prompt prefix but different
  authority or attempt identities.
- The event-encryption epoch key cannot be obtained or rotated before expiry.

## Requirements *(mandatory)*

### Functional Requirements

#### Public Invocation Contract

- **FR-001**: The framework MUST retain the current unary invocation contract:
  one request yields exactly one complete terminal Response or one terminal
  failure, with no required stream object or event callback.
- **FR-002**: The framework MUST add one application-neutral streamed invocation
  contract in which one request yields zero or more ordered typed events followed
  by exactly one complete terminal Response or one terminal failure.
- **FR-003**: The streamed contract MUST expose the immutable request identifier,
  event consumption, terminal-result retrieval, cancellation, status, and
  bounded metrics through one invocation handle.
- **FR-004**: The C++ and Python user/provider surfaces MUST express the same
  lifecycle and defaults; the Python user surface MUST support asynchronous
  iteration without requiring application-managed NDN Interests, and a Python
  Provider MUST publish through the Core-owned writer rather than a Python-only
  stream state machine.
- **FR-005**: LLM token generation MUST be an application use of the generic
  streamed contract, not a framework-level LLM-only request API or LLM-only wire
  message.
- **FR-006**: Targeted and normal selection MAY both create a streamed
  invocation, but they MUST share one event/result lifecycle rather than create
  separate Targeted-stream state machines. Targeted mode MUST reuse the existing
  one-time token pool and Targeted bootstrap/refill behavior: a cached token uses
  the selection-free fast path; a cache miss makes that same streamed invocation
  follow `TargetedBootstrapRequest`; an already in-flight refill permits the
  current bounded one-Provider normal path. None of these paths may create a new
  public API or silently downgrade the streamed request to unary.
- **FR-007**: A streamed invocation MUST have exactly one terminal owner and MUST
  reject duplicate terminal claims.
- **FR-008**: Application callback failures MUST be contained, recorded, and
  converted to the configured cancellation/failure behavior without corrupting
  framework state.

#### Event Naming, Delivery, and Completion

- **FR-009**: Each event MUST be published as signed NDN Data under a
  deterministic producer-owned exact name derived from request, accepted
  attempt, plan, generation, stream epoch, and monotonically increasing cursor.
- **FR-010**: The framework MUST use exact-name Interests for event retrieval and
  MUST NOT require a separate predictive name-mapping publication for invocation
  events.
- **FR-011**: An event MUST bind its request identifier, attempt epoch, plan
  digest, generation identifier, stream epoch, cursor, event type, producer,
  payload digest, and terminal flag in authenticated content.
- **FR-012**: Cursor numbering MUST begin at 1 and increase by exactly 1 within
  one stream epoch; cursor 0 is reserved and invalid on the wire.
- **FR-013**: The consumer MUST pre-express only a bounded exact-Interest window,
  maintain an ordered reorder buffer, suppress duplicate application delivery,
  and re-express a missing cursor only within a bounded retry/deadline budget.
- **FR-014**: Network delivery MAY be at least once, but application delivery
  MUST be at most once per accepted `(attempt, streamEpoch, cursor)` and in
  strictly increasing cursor order.
- **FR-015**: The publisher MUST retain a bounded event window until terminal
  completion plus the declared grace period; an unrecoverable cursor outside the
  window MUST fail explicitly rather than skip silently.
- **FR-016**: An explicit authenticated End event MUST declare the final cursor,
  finish reason, generated-token count, transcript digest, and final-result
  digest reference. It MAY carry the last application event but MUST NOT replace
  the complete terminal Response.
- **FR-017**: The terminal Response MUST remain authoritative, contain the
  complete application result required by Spec 174, and bind the accepted End
  event identity and transcript digest.
- **FR-018**: A terminal Response received before all declared event cursors MUST
  be held until the gaps close or the invocation fails; it MUST NOT cause the
  application to observe an incomplete successful transcript.
- **FR-019**: FEC MUST be disabled by default for invocation events. Exact-name
  retry, caching, and bounded retention are the baseline recovery mechanisms.
- **FR-020**: One event's encoded wire size and one invocation's queued event
  count MUST have explicit limits; oversize or overflow MUST produce bounded
  backpressure or a declared failure and MUST NOT drop an accepted event.

#### Incremental Generation and Role Ownership

- **FR-021**: One LLM generation MUST use one Request, one closed ACK snapshot,
  one accepted placement plan, and one Selection set for the initial attempt; it
  MUST NOT repeat discovery or placement per generated token.
- **FR-022**: The runtime MUST perform prompt prefill once per clean attempt and
  MUST perform subsequent decode epochs using only the new token input plus
  exact compatible provider-local KV state.
- **FR-023**: The final pipeline role MUST own sampling, incremental text
  decoding, End-event creation, and terminal-response assembly as part of its
  existing complete role; these functions MUST NOT create an additional
  placement role.
- **FR-024**: The final role MUST publish two logically separate outputs for a
  nonterminal token: an internal feedback object for the first role and an
  external application event for the user. The names, authorization audiences,
  retention, and acceptance checks of these objects MUST remain distinct.
- **FR-025**: Every ordinary pipeline role MUST have exactly one Provider and
  every selected Provider MUST own exactly one complete role. Cross-Provider
  tensor parallelism remains out of scope until a separately specified
  TensorGroup/rank-role protocol exists.
- **FR-026**: `PreSplitFirstStrategy` MUST remain the default placement strategy;
  alternative placement strategies MUST be explicit opt-ins and MUST preserve
  the same role and event contracts.
- **FR-027**: Sampling parameters, tokenizer identity, model/adapter identity,
  split identity, maximum tokens, EOS/stop rules, deterministic seed, and
  deadline MUST be sealed into the accepted request/plan identity.
- **FR-028**: EOS and stop completion MUST be valid before maximum-token count;
  maximum-token completion MUST be distinguishable from EOS, stop, deadline,
  cancellation, and failure.
- **FR-029**: Incremental text decoding MUST preserve tokenizer state across
  token boundaries and MUST correctly produce Unicode-safe text deltas and
  stop-sequence matching.
- **FR-030**: The deployed runtime MUST use canonical ONNX artifacts, ONNX
  Runtime, and a standalone tokenizer boundary; PyTorch and Transformers MUST
  remain outside the deployed SIF and runtime dependency closure.

#### KV Identity, Recovery, and Fencing

- **FR-031**: KV reuse MUST require exact agreement on model and graph semantics,
  adapter, runner, role split/layers, accepted prefix digest and length, position
  state, precision/layout, runtime ABI, security domain, provider boot/cache
  epoch, request attempt, and generation identity.
- **FR-032**: A KV mismatch or missing cache entry MUST never be treated as a
  partial match; it MUST cause clean recomputation or a new accepted plan.
- **FR-033**: Provider replacement MUST be bounded to at most one replacement per
  streamed invocation in this feature and MUST be disabled unless the application
  request declares that recomputation is permitted.
- **FR-034**: Replacement MUST create a new attempt and stream epoch. Data from
  the old attempt MAY remain retrievable but MUST be fenced from accepted
  application delivery and final-result assembly.
- **FR-035**: Without an exactly compatible protected KV checkpoint, replacement
  MUST recompute from the original prompt plus the committed accepted-token
  prefix. Cross-Provider live KV migration is out of scope.
- **FR-036**: Cancellation, deadline, failed authorization, terminal error, and
  replacement MUST stop new decode work, fence callbacks and publication, release
  bounded queues, and zeroize protected plaintext/KV as required by Spec 174.
- **FR-037**: A retry or replacement MUST NOT re-execute a non-idempotent
  application after execution may have begun unless the application explicitly
  opted into the corresponding recomputation contract.
- **FR-038**: Stale, duplicate, cross-attempt, cross-plan, cross-generation, and
  post-cancellation events and responses MUST be rejected before application
  callbacks.

#### Security and Resource Control

- **FR-039**: Request, ACK, Selection, internal activation/feedback, external
  event, End event, and terminal Response paths MUST preserve NDNSF permission,
  ABE-backed service-semantic authorization, UserToken, ProviderToken,
  signature, replay, and provider-permission invariants.
- **FR-040**: Invocation-event confidentiality MUST use one bounded
  stream/session protection epoch established through the authorized control
  path; the event-key grant MUST remain inside the existing ABE-protected request
  domain and the framework MUST NOT repeat an ABE bootstrap for every event.
  Only the final Provider named by the accepted binding may produce accepted
  events, although every Provider authorized to decrypt that service request is
  within the key-confidentiality domain.
- **FR-041**: Every event MUST still be individually signed and protected by
  authenticated encryption bound to its exact name and lineage.
- **FR-042**: The publisher queue, consumer Interest window, reorder buffer,
  retained-event window, callback queue, and concurrent streamed-invocation
  count MUST be bounded and observable.
- **FR-043**: The baseline queue policy MUST apply backpressure to generation
  before overflow. Dropping an accepted token event, silently changing cursor
  numbering, or continuing unbounded generation is forbidden.
- **FR-044**: Logs and evidence MUST exclude plaintext prompts, decoded answers,
  logits, token encryption keys, and KV tensors by default; hashes, sizes,
  cursors, timings, identities, and terminal reasons are sufficient.

#### Compatibility, Observability, and Validation

- **FR-045**: Existing unary C++/Python APIs, Targeted behavior, streaming media
  facade, and Spec 174 complete-response semantics MUST remain source- and
  behavior-compatible unless an independently reviewed migration is added.
- **FR-046**: The implementation MUST reuse the existing verified-delivery
  attempt/plan/generation fencing, terminal-response guard, exact collaboration
  Data transport, and bounded scheduler primitives instead of creating parallel
  sources of truth.
- **FR-047**: The implementation MUST emit bounded timestamps for request
  creation, ACK closure, plan commit, Selection acceptance, preparation,
  prefill, each decode epoch, internal feedback publication/fetch, external
  event publication/fetch/delivery, terminal Response, cancellation, retry,
  and backpressure.
- **FR-048**: Evidence MUST distinguish implemented, wired, executed, measured,
  and performance-qualified states; a lower evidence level MUST NOT be reported
  as a higher one.
- **FR-049**: Unit tests MUST cover message/name encoding, lifecycle transitions,
  ordering, duplicate suppression, gap retry, terminal consistency, early
  completion, KV compatibility, security negatives, backpressure, and unary
  regression.
- **FR-050**: CPU-only integration tests MUST execute real signed event Data and
  the complete Request-to-final-Response path with a deterministic small ONNX
  model, including multi-role generation and injected loss/reorder/duplicate,
  cancellation, stale-attempt, and replacement cases.
- **FR-051**: CPU MiniNDN tests MUST run the real controller, user, repository,
  NFD/SVS path, and two- and four-Provider role plans with a deterministic small
  ONNX model and fixed workload seed.
- **FR-052**: MiniNDN MUST demonstrate both an unchanged unary workload and a
  streamed LLM workload, and MUST prove through packet/evidence lineage that
  internal activation, token feedback, external events, and final Response use
  the declared Interest/Data paths.
- **FR-053**: TigerCluster submission MUST be gated on all lower tests and on an
  immutable locally built SIF whose hash, Python ABI, native extensions, ONNX
  Runtime provider, linked libraries, runtime assets, workload, and relative
  paths pass the project native preflight.
- **FR-054**: TigerCluster qualification MUST use the same signed workload and
  contract as local gates, require CUDA ONNX Runtime with zero CPU fallback, and
  separate cold preparation from warm measured generations.
- **FR-055**: Every test or experiment run MUST record code commit, SIF/model/
  tokenizer/adapter/workload digests, topology, Provider-role mapping, runtime
  versions, seed, commands, timeouts, result counts, failures, and evidence
  hashes in a machine-readable manifest.
- **FR-056**: A performance report MUST include time to first token, per-token
  latency distribution, steady-state tokens per second, total latency, success
  and exactness, retransmissions/gaps, queue/backpressure, KV reuse/recompute,
  resource utilization, and CPU-fallback count.
- **FR-057**: Tiger failures MUST be reproduced by the smallest lower-cost gate
  that can exercise the same boundary before another full campaign is submitted.

### Key Entities

- **Streamed Invocation**: One accepted service request with immutable request,
  attempt, plan, generation, stream-epoch, security, deadline, and lifecycle
  identity.
- **Invocation Event**: One signed, encrypted, cursor-addressed application
  progress item with exact lineage and payload digest.
- **End Event**: The final stream event declaring why emission stopped, the last
  cursor, token count, transcript digest, and terminal-result reference.
- **Terminal Response**: The sole authoritative complete application result for
  the invocation.
- **Generation Specification**: Sealed model, tokenizer, adapter, sampling,
  stop, deadline, role, and deterministic-workload parameters.
- **KV Identity**: Exact compatibility key for provider-local incremental state.
- **Event Window**: Bounded publisher retention, consumer Interest, reorder,
  retry, and callback state for one stream epoch.
- **Qualification Manifest**: Immutable provenance and measurements for one
  local or Tiger gate.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: In deterministic unit and integration cases, every accepted event
  cursor is delivered to the application exactly once and in order, with zero
  stale-attempt or post-cancellation deliveries.
- **SC-002**: Every successful streamed invocation produces exactly one End event
  and one complete terminal Response whose cursor count and transcript digest
  agree.
- **SC-003**: A fixed-seed small-model generation produces the same token IDs and
  finish reason as its local ONNX oracle in one-Provider, two-Provider, and
  four-Provider CPU validation cases.
- **SC-004**: Evidence for an N-token healthy generation shows one discovery and
  placement cycle, one prefill, N incremental decode epochs, and no per-token
  service request or plan commit.
- **SC-005**: All declared loss, reorder, duplicate, replay, tamper, timeout,
  cancellation, queue-pressure, and one-replacement cases either recover to the
  exact accepted transcript or terminate with the specified reason; none returns
  a silent partial success.
- **SC-006**: The complete existing unary and Targeted regression suites pass
  unchanged after streamed invocation is enabled.
- **SC-007**: CPU integration and MiniNDN gates complete without PyTorch,
  Transformers, GPU, or CPU fallback masquerading as a GPU result.
- **SC-008**: No Tiger job is submitted unless its manifest identifies passing
  lower gates and the promoted SIF hash; every completed Tiger result points to
  that same hash.
- **SC-009**: A functional Tiger run produces a nonempty, ordered token stream,
  exact final result, zero CPU fallback, one terminal Response, and complete
  TTFT/TPOT/resource evidence.
- **SC-010**: A Tiger run is labeled `PERFORMANCE_PASS` only when its warm
  steady-state median output rate is at least 20.0 token/s, its p95 inter-token
  interval is at most 75 ms, and all correctness criteria pass. Otherwise it is
  labeled `FUNCTIONAL_PASS_PERFORMANCE_MISS` with the measured bottleneck.
- **SC-011**: At least three independent warm Tiger processes are measured after
  one excluded cold preparation run; all seeds, run counts, failures, and
  dispersion are reported without discarding valid negative runs.
- **SC-012**: All high-impact API, wire, security, recovery-default, topology,
  workload, measurement, and verdict decisions are fixed before implementation.
  If a lower-level implementation detail is genuinely omitted, the implementer
  may choose the smallest solution consistent with these decisions and current
  code, and MUST record the rationale and closing test in the same task.

## Assumptions

- Spec 174 verified-delivery invariants and the current one-Provider/one-complete-
  role placement baseline remain authoritative prerequisites.
- Cross-Provider tensor parallelism, continuous batching across unrelated user
  requests, speculative decoding, live cross-Provider KV migration, and
  OpenAI-compatible HTTP/SSE service hosting are out of scope for this feature.
  An optional application gateway may translate the generic event API to SSE
  without changing Core protocol semantics.
- The first implementation publishes one application token event per cursor.
  Event micro-batching is deferred until measurements justify it.
- Bounded provider replacement is disabled by default and may be enabled only
  by an application request that permits clean recomputation.
- The first performance target applies to one active generation, not a
  multi-tenant continuous-batching server.
- Offline export may use model-specific tooling, but the canonical artifacts
  and deployed runtime remain ONNX-only.
- The deterministic small-model fixture is committed or content-addressed and
  small enough for repeatable CPU integration and MiniNDN execution.
