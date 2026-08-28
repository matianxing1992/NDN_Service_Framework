# Implementation Plan: NDNSF-DI Streamed Invocation

**Branch**: `Experimental` | **Date**: 2026-08-21 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from
`specs/175-ndnsf-di-streamed-invocation/spec.md`

## Summary

Add one generic streamed-invocation lifecycle beside the existing unary
`RequestService` lifecycle. A streamed request performs the existing NDNSF
Request/ACK/placement/Selection flow once, fetches ordered exact-name event Data
through a bounded window, and still terminates with one authoritative complete
`ResponseMessage`. NDNSF-DI uses that lifecycle to run one ONNX prefill followed
by incremental decode epochs with exact Provider-owned decode-state lookup,
atomic commit, bounded lifecycle, CUDA-resident reuse, final-role sampling,
internal token feedback, and external token events. The work is
accepted through unit, CPU integration, host/CPU MiniNDN, one final local SIF
build plus host-orchestrated exact-SIF application replay, and finally
TigerCluster CUDA gates. The SIF is not rebuilt while core or host MiniNDN
defects are still being repaired.

## Technical Context

**Language/Version**: C++17; Python 3.10 inside the qualified SIF/runtime
boundary; Bash and Slurm for deployment. Host Python is not SIF ABI authority.

**Primary Dependencies**: ndn-cxx; NDN-SVS `Experimental` built with Boost
1.71; NAC-ABE; NFD; pybind11; ONNX Runtime CPU/CUDA; standalone `tokenizers`;
Apptainer and Slurm. MiniNDN, Mininet, Open vSwitch, NLSR, and host networking
tools are G3/G4 host-harness dependencies, not SIF runtime dependencies.

**Storage**: Content-addressed canonical ONNX graph/initializer and adapter
artifacts; bounded in-memory invocation/event/decode state; provider Content Store
and configured event-retention window; JSON/JSONL manifests and evidence.

**Testing**: Boost.Test unit tests; C++ integration tests; pytest contract,
fixture, SIF, and real-MiniNDN gates; deterministic ONNX oracle comparison;
Tiger Slurm qualification.

**Target Platform**: Linux development host; CPU MiniNDN; locally built immutable
Apptainer SIF; UofM TigerCluster NVIDIA GPU nodes.

**Project Type**: C++ framework plus pybind11/Python application SDK,
distributed-inference adapters, experiment runners, and deployment packaging.

**Performance Goals**: Preserve unary latency behavior; reduce LLM control work
to one placement per generation; measure warm TTFT and TPOT; qualify
`PERFORMANCE_PASS` only at median steady-state output >=20.0 token/s and p95
inter-token interval <=75 ms with exact correctness and a passing bounded
cached-versus-full-prefix stage control.

**Constraints**: One Provider owns one complete role; `PreSplitFirstStrategy` is
default; no deployed PyTorch/Transformers; no per-token service request; one
terminal Response; no dropped accepted events; replacement disabled by default;
one bounded replacement only after explicit opt-in; no cross-Provider live decode-state
migration; hybrid models require their complete KV plus recurrent/convolution
decode-state bundle; healthy CUDA decode cannot perform a complete-state host round
trip or carry that state over NDN; the existing Spec 174 TensorGroup remains unchanged but streamed
TensorGroup generation is not a Spec 175 validation subject.

**Scale/Scope**: One active generation in the qualification workload; 1, 2, and
4 Provider CPU test plans plus the existing three-role Qwen3.6-27B Tiger plan;
default maximum 512 events; bounded queues/windows; small deterministic CPU model
locally and Qwen canonical ONNX artifacts on Tiger.

**Evidence separation**: The tiny CPU fixture proves protocol, placement, and
decode-state semantics only. The existing job-202864 27B full-context CUDA chain
proves only that its frozen stage files execute under CUDA ORT; because it uses
`useCache=false` and has no accepted decoded transcript, it is not a stateful
generation oracle. The current production transaction/store tests prove cache
ownership and atomicity, but their provisional role-local prefix derivation
does not yet prove complete identity; older manual fixture feedback proves
neither production wiring nor effective caching. G5
must establish the real 27B stateful stage contract, device residency, and
matched cache-effectiveness control. G6 is the first gate allowed to claim a
complete multi-Provider 27B stream.
Qwen3-0.6B CPU/CUDA runs are optional low-cost diagnostics for adapter and
control-flow wiring; they never substitute for the 27B CUDA subject or the
20-token/s result.

## Constitution Check

### Pre-design gate

| Principle | Result | Design consequence |
|---|---|---|
| Canonical Dynamic Runtime | PASS | Add generic streamed APIs and unified `serviceName`; no generated stubs or LLM-specific Core service type. |
| Security Is Part Of The Data Path | PASS | Stream request, event, End, internal feedback, and final Response preserve permission, ABE, token, signature, and replay gates. |
| CodeGraph First, Source Verified | PASS | Current `ServiceUser`, `NDNSFMessages`, `ProviderRuntimeContext`, Qwen session, and tests were inspected before this plan. |
| Spec-Driven Durable Work | PASS | Spec, research, data model, contracts, quickstart, experiment plan, tasks, analyze, and audit are required before implementation. |
| Verify With The Right Scope | PASS | Unit -> CPU integration -> host/CPU MiniNDN -> one final SIF plus host-orchestrated SIF-application replay -> Tiger CUDA promotion order is mandatory. |
| Cohesive Outcome-Based Tasks | PASS | Each task includes its test-first change, implementation, focused gate, and evidence; no one-file bookkeeping tasks. |

No constitution exception is required.

### Post-design gate

PASS. The contracts below preserve one Core lifecycle and one attempt/plan
authority, separate correctness gates from performance claims, and define
rollback as disabling the new streamed request option while retaining unary
behavior. No second placement protocol, final-response protocol, or media stream
mapping protocol is introduced.

## Architecture Decisions

### 0. Frozen host/SIF/Tiger execution boundary

Spec 175 uses two cooperating runtime layers during G4; it does not run one
containerized MiniNDN installation:

| Layer | Owner and contents | Explicit exclusions |
|---|---|---|
| G3/G4 host substrate | MiniNDN, Mininet, Open vSwitch, NLSR, `mnexec`, `ip`, topology creation, route orchestration, replay driver, and process supervision | No host-built `_ndnsf.so` or host NDNSF application process may qualify G4 |
| Exact SIF runtime | NFD and required NDN management/security tools, NDNSF libraries and Controller/repository/Provider/User entry points, CPython 3.10 bindings, ONNX Runtime, `tokenizers`, and sealed workload code | No MiniNDN, Mininet, Open vSwitch, NLSR, PyTorch, Transformers, host venv, or replacement source overlay |
| Tiger execution | Slurm allocation, the same semantic Apptainer 1.5.3 release, verified node-local SIF staging, external content-addressed models, GPU devices, scratch, and evidence paths | No MiniNDN emulation, SIF rebuild, Docker/OCI materialization, package overlay, or changed workload |

For G4, the host creates every namespace/link and then starts the exact SIF's
NFD and NDNSF application commands inside those namespaces with
`apptainer exec --cleanenv`. Host NLSR and routing helpers may configure the
SIF-contained NFD through the namespace-specific management socket. The host
harness, topology, and tiny ONNX fixture are immutable, hash-bound inputs; they
are not copied into the SIF merely to make the replay "self-contained". The SIF
must contain the production runtime and workload entry points, while model
artifacts remain external and read-only. G5--G7 run the SIF directly under
Slurm on Tiger and do not run MiniNDN.

Preflight is therefore split and fail-closed. A host-substrate preflight checks
MiniNDN/Mininet/OVS/NLSR/network-namespace commands and versions. A separate
in-SIF preflight checks NFD/NDNSF/Python/ORT binaries, ABI, libraries, workload,
and forbidden packages. Passing either side never implies the other passed.

### 0a. Reuse the proven Tiger deployment shape before adding model complexity

Tiger is not a new substrate for NDNSF-DI. The operator baseline is the
previously executed Spec170 path:

| Control | Proven scope | Reused operational contract |
|---|---|---|
| D0 Job 189483 | one Controller, one User, four Providers, real NFD, four ACKs, Selection, CPU ONNX, final Response | one-node CPU control; isolated PIB/TPM/HOME; explicit bundle `cwd`; all children checked |
| r23 D2b Job 201039 | two Providers on two Tiger nodes, one GPU each, positive cross-Provider DATA_V1 and final Response | exact SIF staging/hash, per-node ownership, CUDA ORT with no CPU model fallback |
| r23 D2h Jobs 201045/201046 | both declared heterogeneous two-Provider mappings and missing-data negative | signed mapping/binding, local versus peer dependency evidence, numeric oracle |

These jobs are historical controls, not Spec175 acceptance evidence. The new
candidate changes streamed invocation, source bytes, SIF hash, workload, model,
and the three-role 27B plan. T024 therefore emits a machine-readable
baseline-delta manifest and runs one bounded current-SIF D0-shaped CPU control
after G4. That control preserves Apptainer 1.5.3, `cd "$BUNDLE"`, isolated
HOME/PIB, exact hash staging, real Request/ACK/Selection/Response, and child-exit
checks. Only after it passes may G5 introduce the stateful 27B artifacts; only
after G5 may G6 introduce three-Provider CUDA collaboration. Tiger never runs
MiniNDN, NLSR, a SIF builder, or a source overlay.

An unclassified signal exit in a lower gate is not a retryable setup failure.
It remains part of the registered negative regression inventory even if a
later identical process passes. During active implementation, however, the
historical M09 exit does not take priority over unfinished source behavior. It
is re-evaluated only after the implementation subject is frozen; if it
reproduces, the owner is reduced and covered by a focused regression before
the affected case triplet and source-bound manifest are regenerated.

### 1. One invocation core, two public lifecycles

The existing unary API remains unchanged. The new API is named exactly:

```cpp
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
```

This is the primary Normal overload and performs service-scoped dynamic
discovery. A second overload with the same function name accepts exactly one
`provider` and requires `options.mode == Targeted`; there is no Provider-vector
requirement and no `RequestServiceStreamingTargeted` API. Any existing explicit-
provider-vector compatibility path is not the NDNSF-DI default or evidence path.

The typed wrapper serializes application types but delegates to one non-template
Core request. `StreamedInvocationHandle` is shared-state and exposes only:

```cpp
const ndn::Name& requestId() const;
StreamedInvocationStatus status() const;
StreamedInvocationMetrics metrics() const;
void cancel();
```

Provider-facing generic services use:

```cpp
provider.addStreamingHandler<RequestT, EventT, ResponseT>(
  serviceName,
  [](const RequestT& request,
     StreamedResponseWriter<EventT, ResponseT>& writer) {
    writer.publish(event);
    writer.finish(response, StreamFinishReason::ApplicationComplete);
  });
```

`StreamedResponseWriter` owns `publish`, `finish`, `fail`, `isCancelled`, and
`remainingDeadline`. It is move-disabled and valid only during its invocation.
The NDNSF-DI Python bridge exposes the same Core publisher through
`ProviderRuntimeContext.publish_event(...)` and
`ProviderRuntimeContext.finish_stream(...)`; `finish_stream` and the existing
`publish_final_response(...)` compete for the same terminal guard, so they can
never both succeed for one invocation. The bridge does not implement another
stream state machine.

The exact Python user surface is:

```python
invocation = user.request_service_streaming(
    service_name, request,
    options=StreamedInvocationOptions(),
)
async for event in invocation:
    ...
result = await invocation.result()
await invocation.cancel()  # idempotent; no-op after terminal completion
```

Python Targeted use supplies `target_provider=` as a keyword-only single name.
Normal mode rejects that keyword; Targeted mode requires it.

For multi-role NDNSF-DI, the model/task-first public surface is
`AutomaticPlanningCoordinator.request_streaming(...)`, with no Provider list.
It passes stream intent into the same deferred `begin_collaboration` Request,
closes one ACK snapshot, commits one plan through the existing
`commit_collaboration_plan` owner, and attaches event delivery to that request's
shared stream state. It MUST NOT invoke `request_service_streaming` after plan
commit. The returned handle retains the collaboration request ID and the same
terminal-response guard from discovery through completion.

For a streamed collaboration, each `CollaborationRoleSpec` also carries the
planner-owned `terminalResponseOwner` bit. Exactly one selected role MUST set
this bit. That Provider owns the user-facing Event/End/Response closure; every
other selected role may publish internal collaboration data but completes its
local work without claiming the shared terminal. The bit is included in the
plan commit digest, so changing terminal ownership is a new plan rather than a
silent retry variation.

Synchronous callback access remains available through optional `on_event`,
`on_complete`, and `on_error` parameters. Async iteration and callbacks share
one delivery cursor and cannot both consume the same invocation; the second
consumer registration fails immediately.

Python Providers register `add_streaming_handler(service, handler)` or the
authenticated-context variant. Each handler receives the native Core-owned
`StreamedResponseWriter`; it runs on the existing Provider worker pool and must
finish or fail before returning. The Python wrapper owns no second cursor,
publisher queue, or stream state machine.

### 2. Versioned request, event, and completion wire objects

Add explicit bounded wire objects rather than security-critical string tokens:

- `StreamRequestOptions` as an optional block in `RequestMessage`;
- `InvocationEventMessage` as the payload of exact signed event Data;
- `StreamCompletion` as an optional block in the final `ResponseMessage`.

All are wire version 1, reject duplicate singleton fields, unknown critical
fields, noncanonical order, overflow, and trailing elements, and round-trip to
byte-identical canonical wire. TLV numbers are allocated from the repository's
private application range after checking for collisions during T001; the exact
numbers are then recorded in [wire-protocol-v1.md](contracts/wire-protocol-v1.md)
and must not change inside this Spec.

### 3. Exact event names, no predictive mapping

The final-role Provider publishes event Data under:

```text
/<producer>/NDNSF/EVENT/<requester-uri-component>/<service-name...>/<requestId>
  /<attemptEpoch>/<planDigest>/<generationId>/<streamEpoch>/<cursor>
```

Numeric fields use canonical nonnegative-number components. Digests and opaque
identifiers use one canonical generic component with lowercase hexadecimal text.
The user knows all components after the accepted plan/Targeted binding and
expresses exact Interests. No mapping Data, discovery Interest, or FEC group is
used. The unchanged final Response name remains the current V2 RESPONSE name.

### 4. Fixed defaults and validation bounds

`StreamedInvocationOptions` version-1 defaults are frozen:

| Option | Default | Valid range |
|---|---:|---:|
| `mode` | `Normal` | `Normal` or `Targeted`; Targeted requires one Provider and reuses cached fast path, same-call bootstrap, or bounded in-flight-refill fallback |
| `maxEvents` | 512 | 1..4096 total cursors, including the authenticated End event; at most `maxEvents-1` application events |
| `interestWindow` | 16 | 1..64 |
| `interestLifetimeMs` | 500 | 100..5000 |
| `maxEventRetries` | 3 | 0..8 |
| `publisherQueueCapacity` | 64 | 1..1024 |
| `callbackQueueCapacity` | 64 | 1..1024 |
| `reorderCapacity` | 64 | `interestWindow`..1024 |
| `retentionMs` | 30000 | 1000..300000 |
| `completionGraceMs` | 5000 | 0..30000 |
| `maxEventWireBytes` | 16384 | 512..65536 |
| `allowReplacement` | false | boolean |
| `maxReplacements` | 0 | 0 when disabled; exactly 1 when enabled |

No adaptive defaults exist in version 1. Every nondefault value is serialized
into the request and evidence manifest. Environment variables may not silently
override these signed options.

### 5. One session key, selected-Provider grant, per-event integrity

The user creates a random 256-bit event key and nonzero 64-bit `streamEpoch`
after allocating the request ID. A Normal Request carries only the key
commitment. After ACK closure and plan commit, the final-role Provider's
Provider-specific Selection projection carries a certificate-wrapped key
envelope bound to request, attempt, plan, producer, policy epoch, stream epoch,
and commitment; unselected ACK Providers receive no decryptable grant. A
selection-free Targeted request may carry an envelope wrapped only to its one
explicit target certificate. Every event uses AES-256-GCM with the full Data
name and canonical event header as associated data, a unique nonce derived from
stream epoch and cursor, and the final Provider's signature. The key is never
logged. A replacement receives a new attempt, stream epoch, key, commitment, and
Provider-specific grant. Internal activations/token feedback continue to use
their distinct request-scoped collaboration protection.

### 6. Ordered delivery and backpressure

The user keeps one exact-Interest sliding window starting at the next undelivered
cursor. Valid future events enter a bounded reorder buffer. Duplicate Data may
satisfy transport but causes no duplicate callback. A missing cursor is retried
up to `maxEventRetries` and the invocation deadline. The End event fixes the
highest cursor; the terminal Response waits behind any gap.

The provider inserts each accepted event into a bounded publisher queue before
advancing the generation cursor. Queue saturation pauses the decode loop through
the writer's condition variable until space, cancellation, or deadline. There is
no drop-oldest/drop-newest mode. Application callbacks run outside the Face
thread through the existing bounded worker infrastructure; queue saturation
propagates backpressure rather than allocating unbounded work.

### 7. LLM generation ownership

`QwenGenerationSessionStateMachine` remains the generation authority and is
extended rather than replaced. It gains explicit `Prefilling`, `Decoding`,
`Draining`, and successful finish reasons (`Eos`, `StopSequence`, `MaxTokens`,
`ApplicationComplete`). `complete(reason)` accepts EOS/stop before `maxTokens`
but accepts `MaxTokens` only at the bound. It records accepted token prefix and
one terminal claim.

The final role owns sampling and incremental detokenization. For every token it:

1. samples exactly once from the final logits under the sealed parameters;
2. computes the provisional tokenizer delta and terminal condition;
3. serializes, signs, and admits the exact external event into the bounded
   publisher queue without yet starting another decode epoch;
4. only after admission, commits the token epoch, accepted prefix, tokenizer
   state, and candidate KV, then publishes the internal feedback object to the
   first role;
5. starts the next decode epoch only after required feedback/activation state is
   accepted; a failure after the externally visible commit terminates or enters
   a new permitted replacement attempt and never rewinds the same stream epoch.

The qualification adapter is explicitly text-only and single-token
autoregressive. The source model's vision encoder/projector and optional MTP
heads are not placement roles and are not included in the ONNX stage closure.
One final-role sample therefore produces one internal feedback object and one
external event before the next epoch; speculative/MTP branches are outside V1.

For the native path, this contract is carried by a terminal-role-only
`RoleExecutionContext::StreamEventSink`.  The bounded Provider worker passes the
sink to the adapter while preserving the existing role queue and dependency
transport; the adapter submits already serialized token events through the
sink, while Core remains responsible for admission, cursor assignment, signed
Data retention, End, and the terminal Response.  Events are never placed in a
`TensorBundle` or dependency edge.  Until a native adapter drives this sink
from a real incremental ONNX loop, the existing one-event native smoke is
compatibility evidence only and cannot close I01-I03 or G6.

### 7a. Native multi-Provider epoch coordinator

The native multi-role path uses the request-scoped epoch coordinator specified
in [native-epoch-coordinator-v1.md](contracts/native-epoch-coordinator-v1.md).
This is protocol state attached to the existing deferred collaboration, not a
second service or placement plane. Each selected Provider continues to own one
complete role. Its handler runs that role once for prefill and once per decode
epoch through the existing `NativeProviderRuntime` and `ProviderRoleWorker`.

The coordinator calls the runner's unary `run()` for one ONNX state transition.
It does not use `NativeModelRunner::runStreamed()` for a multi-Provider plan,
because that hook owns a complete single-role token loop and cannot receive an
activation from another Provider at every epoch. The hook remains valid for a
one-Provider compatibility path. Nonfinal outputs are published as signed
epoch-specific DATA_V1 activation objects; the final role samples once, admits
one external event, then publishes the epoch-specific token-feedback object.
Only the final role admits End and the terminal Response.

For a one-role plan, the terminal-to-first TOKEN_FEEDBACK edge is a self-edge
inside the same selected Provider process. It retains the same request, plan,
generation, attempt, and epoch validation, but uses the trusted process-local
`DependencyIo` implementation instead of publishing and refetching the object
through SVS. This is not a network fast path and grants no new authority. Any
activation or feedback whose producer and consumer are different selected
Providers continues to use authenticated, exact-name DATA_V1 over NDN.

The accepted capability pre-authorizes all activation and feedback operation
indexes through the signed maximum token bound. The operation index, exact
Data name, request/attempt/plan/generation/stream binding, producer/consumer
roles, and tensor digest are checked for every epoch. Missing or reused
operations fail closed. I02/I03/I15 cannot be registered from the Python
oracle, direct adapter test, deterministic native prerequisite, or generic
one-Provider stream tests; they must exercise this coordinator through the
production Request/ACK/Selection/`NativeProviderHandler` path.

The external `GenerationTokenEvent` application payload contains token ID,
token epoch, UTF-8-safe `textDelta`, cumulative token count, and finish hint. The
Core treats it as opaque `EventT`. The End event is Core metadata and the final
Response contains the complete token list/text and result contract.

### 8. Exact decode-state identity and replacement

The exact `DecodeStateIdentityV1` fields and acceptance predicate are frozen in
[generation-state-machine-v1.md](contracts/generation-state-machine-v1.md).
Provider-local state is never addressed merely by session or prefix length. The
adapter supplies one complete `DecodeStateBundleV1`; Qwen3.6 binds both
full-attention KV and linear-attention recurrent/convolution state. Each decode
verifies the complete identity.

Identity construction has two explicit authorities. Immutable fields are
materialized from the validated content-addressed artifact/adapter manifest,
the accepted Provider projection, the selected Provider boot identity, and the
loaded runtime ABI. The Provider handler rejects an incomplete template before
prefill; it does not synthesize fallback digests from names or free-form runner
metadata. Request fields come from the authenticated collaboration and
generation contract. The request-scoped epoch coordinator alone derives the
dynamic logical prefix, position, state inference epoch, and predecessor
inference epoch. The Provider runtime supplies the cache-incarnation epoch; it
is constant for one lineage and changes only on cache reset/invalidation.

The logical prefix is the canonical model token sequence already consumed by
the model: prompt tokens after prefill, then prompt plus one additional admitted
feedback token after each decode. It is not the serialized activation or tensor
bundle received by a later role. Stage 0 derives the next prefix lineage from
authenticated token feedback; every downstream activation carries only the
authenticated prefix digest/count and position commitment needed to verify the
same lineage. Thus all roles in one pipeline epoch share one logical prefix
digest/count while keeping distinct role/split/component identities. The
adapter derives `positionDigest` from its canonical position/cache-position
inputs for that exact prefix; a static placeholder is invalid.

Replacement is opt-in, Normal-only, and at most once. Because ProviderTokens
are one-time and the old ACK/Selection authority
must not be replayed, replacement is a new internal Request (attempt 2) with a
fresh ACK closure, plan, Selection, event key, and stream epoch, while the public
handle and generation ID remain stable. Without an exact protected checkpoint,
the recovery Request carries the encrypted original prompt plus committed token
IDs/count/digest, and every role recomputes prefill once over that prefix. The
new stream emits only continuation tokens. It never copies live decode state
between Providers or reuses old one-time tokens in this Spec.

### 8a. Request-local Provider-owned decode-state store and CUDA residency

This section specifies only the **request-local KV-cache**. It covers the
prefill-to-decode loop of one Request, including its attempts, generation, and
role epochs. It ends at terminal cleanup unless the explicit promotion
transaction in Section 8b transfers the finalized state. It MUST NOT be used
as a cache for a later Request, even when the prompt, `conversationId`, or
generation text is identical. The separate **conversation-scoped KV-cache**
for cross-request continuation is defined only in Section 8b and is indexed
by an authenticated conversation checkpoint and context epoch.

The live decode state is owned by the selected Provider's request-local role
session, not by the caller, the integration harness, or an NDN dependency edge.
`NativeProviderSession` (or one lower reusable runtime owner reached by every
`executeRoleAsync` path) must own a bounded `KvStateStore`-equivalent service and
must place it in the production epoch path. Merely defining `KvStateStore`,
testing its methods directly, or manually assigning `state_out` to `state_in`
inside a test does not satisfy the design.

For each role and accepted attempt the store manages one committed state and at
most one in-flight candidate. Prefill produces `stateInferenceEpoch=0` with no
predecessor. Decode epoch `e>0` names
`stateInferenceEpoch=e` and `predecessorInferenceEpoch=e-1`, must look up that
exact committed predecessor, verify the complete `DecodeStateIdentityV1`,
execute one transition, validate every state component, and atomically replace
the committed entry only after the role output has been accepted for downstream
publication. The compact store key is only an index; successful reuse always
compares the complete identity. Failure discards the candidate and leaves the
predecessor committed state intact. A miss, eviction, or identity mismatch is
observable and chooses only the registered clean-recompute/new-plan path;
decode must never silently inject zero state or call the runner with a partial
bundle.

Active state is pinned while a role transition or its publication is in flight.
The bounded store may evict only inactive entries using deterministic LRU order.
Cancellation, deadline, terminal completion, attempt fencing, Provider boot-ID
change, and explicit failure release or invalidate the matching entry. Different
requests, attempts, plans, generations, roles, Providers, and boot/cache epochs
cannot share an entry in V1.

The adapter owns the storage representation. CPU qualification may materialize
the complete bundle as host `TensorBundle` bytes. CUDA qualification must retain
state tensors in Provider-local device buffers and reuse them through persistent
ONNX Runtime I/O binding; it must not decode every state output to host bytes and
recreate every state input from host memory on each token. State tensors never
cross NDN. Only planned activations, authenticated token feedback, and bounded
metadata cross Provider boundaries. Evidence exposes identities, sizes,
hit/miss/commit/eviction counters, residence and transfer-byte/timing summaries,
but never tensor contents.

Cache correctness and cache effectiveness are separate acceptance properties
under FR-032a.
The first proves complete identity matching, atomic predecessor/successor
transactions, isolation, and cleanup. The second proves that a production runner
actually consumes the committed predecessor state instead of executing the full
prefix behind a successful lookup. Before CUDA work, the tiny CPU subject runs a
matched cached-versus-full-prefix semantic control: identical prompt/artifact/
split/seed and output, one prefill on the cached branch, one new token/current
activation per later transition, recorded actual input extents, and a positive
machine-derived prefix-work-avoided total. This CPU control makes no performance
claim. G5 repeats the matched control on the real 27B CUDA stages and additionally
requires persistent device state, zero complete-state host round trips, and lower
median cached model-compute time under alternating execution order. A store hit,
counter increment, or correct transcript alone closes neither property.

The current CPU checkpoint closes the semantic half of this boundary on the
production coordinator/runner path. For the same tiny ONNX artifact and prompt,
cached and no-predecessor full-prefix branches both emit
`4,5,6,7,8,9,10,2`. The cached branch records one new input token per epoch,
represented prefix lengths 1--8, and avoided prefix work 0--7 (28 tokens total),
while the control records full-prefix input lengths 1--8. The control initially
failed because its test oracle retained a reference into a temporary decoded
tensor vector; the fixed oracle owns that vector through logits consumption.
This correction changes no production cache path and creates no CPU timing or
CUDA claim. T025 owns persistent CUDA state and the registered real Qwen3.6
stage timing/transfer proof.
For an `APPEND_DELTA` continuation, epoch-zero observation now reports the
promoted parent's exact prefix length as `prefixWorkAvoided`; it does not set
the request-local `decodeStateHit` flag, so conversation and request-local
cache evidence remain separate.

### 8b. Cross-request conversation checkpoints and tiered Provider state

The request-scoped `DecodeStateIdentityV1` remains unchanged for one turn.
Cross-turn reuse is a separate, versioned conversation contract; it must not be
implemented by weakening `requestId` equality in V1 or by treating a public
`generationId` as a cache key.

The two caches have deliberately different lifetimes:

| State | Purpose | Identity/lifetime | Terminal rule |
|---|---|---|---|
| Request-local decode state | Reuse prefill/decode state between token epochs of one invocation | exact request, attempt, plan, generation, role, Provider, model, prefix, and state epoch | release on terminal completion unless a conversation promotion transaction owns it |
| Conversation-scoped state | Reuse the completed conversation prefix in a later turn | conversation/context epoch plus exact requester/security, placement, role, Provider boot/cache, model/layout, and prefix identity | retain under bounded policy after aggregate successor commit; never reuse old Request authority |

Conversation support therefore adds a lifecycle handoff, not a relaxed cache
lookup. A later turn never searches the request-local store using a new request
ID and never removes `requestId` from `DecodeStateIdentityV1`.
After ACK-driven placement is finalized, the planner seals one
`ConversationTurnBindingV1` into every Provider-specific Selection projection.
That commitment fixes the conversation parent/successor epochs, service,
complete one-to-one role-map digest, current request-contract digest, retention
upper bound, and (for a later turn) parent-checkpoint digest. The native
Provider validates it against the actual assignment and, for `APPEND_DELTA`,
against its exact role-local `ConversationStateReferenceV1` before any cache
lookup or runner call. This binding gives the first turn an authenticated
promotion target without pretending that a parent checkpoint already exists.
The cross-request cache is the promoted Provider-local model-state bundle
(`ConversationStateEntryV1`), including every adapter-required KV and
recurrent/convolution component. `ConversationCheckpointV1`, receipts, and
the User transcript authorize and describe that bundle; they do not contain
or replace it. A second request that only sends the checkpoint and recomputes
the full prefix is therefore the explicit full-context fallback, not a
conversation-cache hit.

Ownership is explicit:

| Owner | Responsibility |
|---|---|
| Application/`APPClient` | Chooses `FULL_CONTEXT` or explicit `APPEND_DELTA`, stores one opaque checkpoint, and optionally supplies authenticated full context for fallback. It does not choose Providers or move state. |
| Automatic planning coordinator and protected RuntimeJournal | Persist the committed message/token transcript under the requester's envelope key, create a fresh Request/generation for every turn, prove exact token-prefix extension, verify the parent checkpoint, require the exact prior Provider-role map for reuse, and commit a successor only after all role receipts agree. |
| Provider runtime/state manager | Owns role-local state tensors, exact compatibility validation, GPU/host residency, pinning, asynchronous prefetch, LRU/quota enforcement, and signed role receipts. |
| Core NDNSF | Carries opaque versioned continuation metadata and signed receipts. It does not interpret KV tensors, model layout, or conversation scheduling policy. |

The first turn uses `FULL_CONTEXT`: normal request-scoped placement selects one
Provider per complete role, each role performs prefill, and every Provider
commits role-local state. Before promotion, the adapter freezes the canonical
completed-turn transcript and checks whether the committed request-local state
already represents every accepted non-EOS assistant token and required
turn-finalizing chat-template token. Any bounded missing suffix is consumed by
`CHECKPOINT_FINALIZE` state-only transitions: they advance Provider-local state
but perform no sampling and publish no application event. The coordinator issues
`ConversationCheckpointV1(contextEpoch=1)` only after all selected roles return
compatible receipts for the same logical prefix. The application result and
checkpoint resolve together only after this aggregate commit, even if Core has
already validated the terminal End/Response internally. A later `APPEND_DELTA` turn is
a new Request and generation. It verifies the parent checkpoint, selects the
same exact Provider-role map, restores each Provider's state, and asks the
adapter to prove that canonical tokenization of the new complete transcript is
the committed parent token prefix plus one exact appended suffix. It performs
delta prefill over only that suffix and then enters the existing automatic
decode loop. The parent remains committed until the successor checkpoint is
atomically accepted, so failure cannot corrupt the last usable turn.
Evidence must show a distinct cross-request hit: a fresh request identity, a
conversation-state lookup/readiness result, restored state on every role, and
measured prefix work avoided. A request-local hit from the first turn, a
transcript-only lookup, or full-prefix execution hidden behind a hit counter
does not satisfy this step.

The promotion transaction is fixed:

```text
request-local terminal candidate
  -> freeze completed transcript and canonical token prefix
  -> CHECKPOINT_FINALIZE any bounded unrepresented suffix
  -> bind the exact finalized role returned by NativeEpochCoordinator
  -> stage one ConversationStateCandidate per selected role
  -> collect and validate every Provider receipt
  -> compare-and-swap parent context epoch
  -> publish aggregate successor checkpoint
  -> transfer/retain state ownership in ConversationStateEntryV1
  -> release request-local session ownership
```

A move/strong-reference handoff of adapter-owned device buffers is allowed; an
extra GPU copy is not required. Until aggregate commit, both the previous
conversation parent and the new request candidate remain protected. Failure
releases only the new candidate and preserves the parent.

Each role receipt binds conversation, parent/successor context epochs, service,
requester/security domain, model/graph/adapter/split/layout, plan/role map,
Provider identity and boot/cache epochs, logical prefix digest/count, position
identity, expiry, and the request contract. Receipts and the aggregate
checkpoint are authenticated capabilities; `conversationId` alone is only an
index. No tensor, device pointer, file path, or raw provider-local state name is
placed on the wire.

The Provider state manager keeps storage tier and lifecycle orthogonal:

```text
residency: GPU_RESIDENT <-> HOST_RESIDENT -> EVICTED
lifecycle: IDLE -> PREFETCHING -> PINNED/COMMITTING -> IDLE
```

Active or dispatch-ready state is pinned on GPU. Inactive state may move to a
bounded pinned-host representation while model weights remain GPU resident.
Resume starts a single-flight asynchronous prefetch; execution is admitted only
after every selected role reports the exact state ready. GPU and host stores
have separate byte/entry quotas and deterministic inactive-LRU eviction. The
first version has a configurable, capped 300-second default retention and no
disk tier. Cancellation releases pending prefetch safely and never exposes a
partly ready role set.

A conversation is linear and single-writer. The successor commit compares the
expected parent context epoch; only one concurrent child can win. If any role
state is missing, stale, expired, evicted, or incompatible, the coordinator
either performs one recorded full-context prefill using caller-supplied
authenticated fallback input or returns a specific continuation failure. A
full fallback may choose a new ACK-driven role map, but it is recorded as full
prefill rather than reuse and its successor checkpoint binds that new map. It
never mixes roles, fills missing state, changes placement while claiming reuse,
or migrates state between Providers.

The new contract is additive. A caller that supplies no conversation options
continues to use the existing request-scoped full-context path. A CPU state-tier
emulator closes identity, transaction, scheduling, and failure semantics before
packaging. Only the final CUDA gate may claim actual GPU-to-host-to-GPU state
movement or its measured cost.

The protected User-side transcript is logical conversation state, not model
state. `ConversationTranscriptRecordV1` stores the committed structured
messages, exact tokenizer/chat-template identities, canonical token IDs, prefix
digest/count, checkpoint digest, and context epoch in the existing encrypted
`RuntimeJournal`. A healthy delta turn sends only appended input and checkpoint;
the old transcript/token IDs remain local. If the journal record is missing,
undecryptable, or does not match the checkpoint, the coordinator cannot prove
prefix extension and takes only the explicit full-context fallback/failure path.

### 9. Test ladder is a promotion state machine

The gate order is immutable and cost-ordered. While source, contracts,
workload, or runtime wiring are changing, only focused development checks run;
the formal G0--G3 sequence starts once after the implementation subject is
complete and frozen:

```text
G0 static/contract -> G1 unit -> G2 CPU integration ->
G3 host/CPU MiniNDN -> G4 final local SIF build/native preflight plus
    host-MiniNDN replay of SIF-contained NFD/NDNSF applications ->
G4T current-SIF Tiger D0-shaped deployment control ->
G5 Tiger 27B per-stage CUDA/cache readiness ->
G6 Tiger multi-Provider streamed invocation ->
G6C Tiger conversation-residency control -> G7 performance qualification
```

Failure at any gate blocks later promotion. Tiger does not rebuild the SIF. It
verifies the locally generated hash and runs the same content-addressed workload
bundle. The G4 SIF embeds the exact framework/application source; qualification
does not bind-mount a replacement checkout or Python module. Canonical model
stages remain external and are staged once into a hash-verified node-local
content-addressed cache before measured execution. The exact cases, repetitions,
metrics, and verdicts are frozen in
[validation-contract.md](contracts/validation-contract.md) and
[experiment-plan.md](experiment-plan.md).

The host/CPU MiniNDN gate is the inexpensive debugging boundary. It uses the
current native/Python build, the checked-in tiny ONNX fixture, real NFD/SVS/ABE,
and the four-Provider topology, but no SIF. After G0-G3 pass, the local SIF is
built once and preflighted. The same host MiniNDN harness then creates the
namespaces and topology, but launches NFD and every NDNSF/ORT application from
that exact SIF. This is a final packaging/runtime-boundary check, not MiniNDN
inside the SIF. A failed replay returns to the owning host-substrate or SIF-
runtime gate and never starts Tiger submission.

This ordering is also a cost boundary, not merely documentation. During active
implementation, only behavior-owned compile/unit/integration checks run; the
formal G0--G3 promotion sequence is deferred rather than repeated after every
source edit. SIF creation, SIF preflight/replay, remote upload, Slurm
allocation, and Tiger jobs are blocked until every implementation task closes
and the resulting frozen subject passes G0--G3 once. A SIF produced before
that point is diagnostic-only and must not be reused as a promotion candidate.
Once G3 passes, the source/workload identity is frozen and exactly one final
SIF is built; any later source, contract, or workload edit invalidates it and
returns the process to implementation plus G0 rather than permitting repeated
expensive rebuilds.

The final SIF/Tiger sequence is therefore a one-way release-candidate path:
`G3 PASS -> freeze manifest -> build/preflight one SIF -> host-orchestrated
exact-SIF application replay -> G4T Tiger deployment control -> Tiger G5 ->
G6 -> G6C -> G7`. A failure in the local
replay or any Tiger gate is
diagnosed against the smallest applicable host/CPU gate first; it does not
authorize an ad hoc rebuild or a changed-parameter rerun.

Task status follows acceptance, not code volume. A focused test, a wired entry
point, or a diagnostic SIF may be recorded as an implementation checkpoint, but
it does not close the task unless the task's current-source repetition,
manifest, and cleanup requirements pass. This is intentional: the previous
20/28 display overstated progress while T005--T019 still had partial or stale
evidence. The automatic Provider-state and repository-readiness corrections
reopened T020 and invalidated their predecessor seals. The corrected committed
subject passed one new source-bound G0--G3 sequence, and its exact local SIF
passed both layer-specific preflights plus an auditable 42-entry G4 aggregate
with one explicitly recorded replacement. The current baseline is therefore
29/34 accepted tasks: the implementation queue, T020, T022, and T023 are
closed, while the post-G4 release gates remain open.

After T005--T019, T024, and T029--T033 completed the Core/runtime/API,
conversation, and pre-frozen harness/launcher implementation, T020 resealed the
cheap G0/G1/G2 gates once. T022 executed the host gate as strict packets: (A)
localize any post-Selection exact-event fetch race without changing retry
parameters, (B) obtain three clean M01 processes, (C) run M02--M14 three times
each with one registered fault/continuation dimension, (D) seal the 42/42 manifest, and (E) classify
every same-subject native exit. The resulting clean D/E closure passed G3.
T021 owns the already implemented builder/preflight machinery; T023 used it to
build one final SIF and qualify the host-orchestrated exact-SIF replay. T025 is
now the next gate; this keeps the cheap debugging loop separate from the
expensive Tiger release-candidate path.

The promotion gates are preceded by a development-toolchain closure check. C++
work must compile and link against one explicit NDN-SVS `Experimental`
source/build pair and Boost 1.71; header-only overrides that still link an older
installed `libndn-svs` are invalid. Python-native work must rebuild `_ndnsf.so`
from the same current framework/toolchain before its tests. These checks prevent
development ABI drift but do not themselves advance G0-G7. Container-runtime
extensions are still built only inside the candidate SIF or sealed ABI-identical
builder.

## Source Changes

```text
ndn-service-framework/
├── InvocationStream.hpp                  # options, handle, writer, status/metrics
├── InvocationStream.cpp                  # bounded lifecycle, names, delivery
├── NDNSFMessages.hpp/.cpp                # wire types plus collision-checked TLVs
├── ServiceUser.hpp/.cpp                  # service-only stream plus shared collaboration state
└── ServiceProvider.hpp/.cpp              # streaming handler/writer publication

pythonWrapper/
├── src/ndnsf/_ndnsf.cpp                  # native streamed types and methods
└── ndnsf/service.py                       # async iterator and Python options/events

NDNSF-DistributedInference/
├── cpp/adapters/qwen/QwenGenerationSession.hpp/.cpp
├── cpp/ndnsf-di/AsyncDataflowRuntime.hpp
├── cpp/ndnsf-di/ProviderRoleWorker.hpp/.cpp
├── cpp/ndnsf-di/NativeProviderRuntime.hpp/.cpp
├── cpp/ndnsf-di/NativeProviderSession.hpp/.cpp # production decode-state owner/store
├── cpp/ndnsf-di/NativeProviderHandler.cpp # epoch coordinator + terminal sink
├── cpp/ndnsf-di/NativeEpochCoordinator.hpp/.cpp # request-scoped role epochs
├── cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp/.cpp # persistent state I/O binding
├── ndnsf_distributed_inference/adapters/qwen/
│   ├── generation.py                     # sealed sampling/finish/event payloads
│   ├── tokenizer.py                      # incremental decoder state
│   ├── stateful_onnx.py                   # complete hybrid decode-state schema/conformance
│   └── placement.py                      # ACK-driven pipeline placement and generation binding
├── ndnsf_distributed_inference/provider.py
├── ndnsf_distributed_inference/conversation.py       # checkpoint/receipt validation and turn transaction
└── ndnsf_distributed_inference/app_sdk/
    ├── client.py                         # optional conversation continuation API
    └── placement.py                      # model/task-first request_streaming bridge

examples/python/NDNSF-DistributedInference/llm_pipeline/
├── user.py
├── provider.py
└── workload.py

tests/
├── unit-tests/
│   ├── invocation-stream-message.t.cpp
│   ├── invocation-stream-lifecycle.t.cpp
│   ├── di-qwen-generation-session.t.cpp
│   └── distributed-inference-async-runtime.t.cpp # Provider-session store wiring/lifecycle
├── integration-tests/
│   ├── invocation-stream-flow.t.cpp
│   └── ndnsf-di-streamed-generation.t.cpp
└── python/
    ├── test_streamed_invocation_api.py
    ├── test_spec175_cpu_fixture.py
    ├── test_spec175_qwen_stateful_onnx.py
    ├── test_spec175_conversation_continuation.py
    ├── test_spec175_conversation_state_tiering.py
    ├── test_spec175_real_minindn_gate.py
    ├── test_spec175_sif_preflight.py
    └── test_spec175_evidence.py

Experiments/
└── NDNSF_DI_StreamedGeneration_Minindn.py

tools/ndnsf-di/
└── export_spec175_qwen36_stateful_onnx.py # offline-only exporter; not deployed

packaging/ndnsf-di-container/
├── bin/ndnsf-di-spec175-preflight
└── jobs/spec175/
    ├── qualify-stage-readiness.sbatch
    ├── qualify-multiprovider.sbatch
    └── run-streamed-generation.sh
```

**Structure Decision**: Put application-neutral lifecycle, wire, names,
security binding, and queues in Core; put serialization/async ergonomics in the
Python wrapper; put model/decode-state/sampling semantics in the Qwen adapter; keep
topology, workload, manifests, and Slurm entry points in experiment/packaging
layers. The media `StreamFacade` remains separate because it owns predictive
mapping, FEC, and long-lived stream-session semantics that this request-scoped
event path intentionally does not use.

## Delivery Phases

### Phase A - Core contract and unary compatibility

Implement wire objects, deterministic names, shared lifecycle, provider writer,
user handle, and C++/Python bindings. Start with canonical wire/name tests and
state-machine tests. Close with full existing unit tests and explicit unary/
Targeted regressions.

### Phase B - Incremental Qwen generation

Extend the existing Qwen session state machine, incremental tokenizer, sealed
sampling, complete decode-state identity/bundle, Provider-owned atomic state
store, final-role event publication, feedback loop, and final-result consistency.
Close with a deterministic CPU ONNX oracle that reaches the production
Provider-session path without harness-managed state feedback, plus no deployed
Transformers import.

### Phase C - Fault and security closure

Close ordering, retry, retention, backpressure, cancellation, stale attempt,
tamper/replay, deadline, and optional one-replacement behavior in CPU integration
tests. No network campaign begins until these pass.

### Phase D - Multi-turn continuation and host-tier state

Add the optional conversation API, authenticated aggregate checkpoint and
per-role receipts, exact same-placement resume, delta prefill, transactional
multi-role commit, single-writer context epochs, explicit full-prefill fallback,
and Provider-local GPU/host residency manager. First close unit and process
integration tests. Then extend the same host/CPU MiniNDN subject with M11-M14 for
two-turn exactness, multiple-conversation isolation, host-tier restore,
restart/mismatch/fallback, concurrency, cancellation, and zero state tensors on
NDN edges. No SIF may be built until this expanded G0-G3 subject passes.

### Phase E - Frozen local qualification

After every Core/runtime/API/conversation and qualification-tooling
implementation task closes, freeze one source/workload subject. Run G0--G2
once, then run unchanged unary plus streamed single-, two-, and four-Provider
M01--M14 cases with the current native/Python build and deterministic CPU model.
Inject only one fault dimension per registered case and produce packet lineage
and machine-readable manifests. Do not continue editing the subject between
passing G0--G3 and the final SIF build; after G0--G3 pass, replay the same
manifest against the final SIF as a packaging check.

### Phase F - Final SIF and TigerCluster qualification

Build one locally built SIF only after G0-G3 pass, require its exact hash to pass
the final SIF replay (G4), and promote that hash only after G4. First run one
bounded current-SIF Tiger D0-shaped control using the registered historical
launcher/configuration contract and a machine-readable delta manifest. Then verify every
pinned Qwen3.6-27B stage with CUDA ORT and the node-local content-addressed cache;
this includes persistent device-state bindings, a bounded cached-versus-full-
prefix stage control, and host/device transfer accounting, but is not yet a
streamed-generation result. Then run the
three-Provider one-request streamed functional gate. Only after correctness
passes, run the fixed performance workload and assign the contract verdict
without tuning unregistered parameters inside the campaign. A Qwen3-0.6B
CPU/CUDA run may remain an optional diagnostic but never closes G5, G6, or G7.

## Migration and Rollback

- Wire version 1 is opt-in through `StreamRequestOptions`; peers without support
  reject the request as `STREAM_UNSUPPORTED` instead of treating it as unary.
- Unary requests do not allocate stream state and remain the rollback path.
- The new TLVs and API are additive; no persisted unary artifact is migrated.
- Conversation continuation is additive and disabled unless the caller supplies
  explicit conversation options. Existing request-scoped cache entries are not
  silently promoted into conversation checkpoints.
- Rolling back conversation continuation disables `APPEND_DELTA` and host-tier
  state movement; callers may continue with `FULL_CONTEXT` turns. A failed
  continuation may use that path only when the authenticated full context and
  fallback permission were supplied with the turn.
- A feature flag may disable accepting streamed requests, but it may not bypass
  security or silently downgrade a streamed request to unary.
- If implementation must be reverted, remove streamed API registration and
  keep decode functionality behind unary complete responses; existing response
  names and message semantics remain intact.
- Version-1 compatibility code has no legacy alias names. Any future wire
  version requires a separate migration Spec.

## Complexity Tracking

| Added mechanism | Why needed | Simpler alternative rejected because |
|---|---|---|
| Generic request-scoped event lifecycle | Delivers intermediate results while preserving one terminal Response | Repeating `RequestService` per token repeats placement/prefill and breaks one-generation semantics. |
| Explicit event wire and completion binding | Prevents ambiguous tokens and verifies transcript completeness | Critical lineage in string token maps is not canonical or strongly validated. |
| Exact event Interest window | Provides NDN-native bounded ordering/recovery | Reusing predictive media mapping/FEC adds unrelated long-lived stream semantics and separate mapping Data. |
| Exact KV identity | Makes incremental decode safe across cache/retry boundaries | Session ID or prefix length alone cannot prove graph, role, runtime, authority, or position compatibility. |
| Provider-owned transactional decode-state store | Makes cache reuse automatic and atomic in the production role path | Caller/test-managed state feedback can pass an adapter oracle while the deployed Provider has no cache. |
| CUDA-resident state I/O binding | Prevents the complete cache from crossing host memory every token | Host `TensorBundle` round trips are functionally correct but can erase the intended decode-speed benefit. |
| Authenticated multi-role conversation checkpoint | Makes a later Request prove that every role owns the same committed prefix without exposing Provider-local state | Reusing `conversationId`, prompt equality, or one role's cache hit cannot prove complete compatible state. |
| GPU/host conversation-state tier | Lets multiple paused conversations retain reusable prefixes while active state occupies GPU memory | Keeping every conversation on GPU is unbounded; dropping every inactive state forces full re-prefill. |
| Linear compare-and-swap context epochs | Prevents concurrent turns from silently forking or overwriting one conversation | Last-writer-wins can combine incompatible role states and make checkpoint lineage ambiguous. |

The provider-local exact-forward cache is limited to pure dependency-producing
role executions. A terminal streamed execution carries an external event sink;
because replaying cached TensorBundle outputs would not replay that side effect,
such executions bypass both cache lookup and cache insertion. This prevents a
retry from silently collapsing a multi-event transcript into the legacy
single-final-event compatibility fallback.

That exact-forward result cache is distinct from the decode-state store above.
Streamed roles must bypass replay of externally visible results while still
retaining their private incremental model state across authenticated epochs.
