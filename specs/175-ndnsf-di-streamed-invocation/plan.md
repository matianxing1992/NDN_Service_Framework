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
by incremental decode epochs with exact provider-local KV compatibility, final-
role sampling, internal token feedback, and external token events. The work is
accepted through unit, CPU integration, exact-SIF CPU MiniNDN, and finally
TigerCluster CUDA gates.

## Technical Context

**Language/Version**: C++17; Python 3.10 inside the qualified SIF/runtime
boundary; Bash and Slurm for deployment. Host Python is not SIF ABI authority.

**Primary Dependencies**: ndn-cxx; NDN-SVS `Experimental` built with Boost
1.71; NAC-ABE; NFD/MiniNDN; pybind11; ONNX Runtime CPU/CUDA; standalone
`tokenizers`; Apptainer and Slurm.

**Storage**: Content-addressed canonical ONNX graph/initializer and adapter
artifacts; bounded in-memory invocation/event/KV state; provider Content Store
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
inter-token interval <=75 ms with exact correctness.

**Constraints**: One Provider owns one complete role; `PreSplitFirstStrategy` is
default; no deployed PyTorch/Transformers; no per-token service request; one
terminal Response; no dropped accepted events; replacement disabled by default;
one bounded replacement only after explicit opt-in; no cross-Provider live KV
migration; no cross-Provider TensorGroup in this Spec.

**Scale/Scope**: One active generation in the qualification workload; 1, 2, and
4 Provider CPU test plans plus the existing three-role Qwen3.6-27B Tiger plan;
default maximum 512 events; bounded queues/windows; small deterministic CPU model
locally and Qwen canonical ONNX artifacts on Tiger.

## Constitution Check

### Pre-design gate

| Principle | Result | Design consequence |
|---|---|---|
| Canonical Dynamic Runtime | PASS | Add generic streamed APIs and unified `serviceName`; no generated stubs or LLM-specific Core service type. |
| Security Is Part Of The Data Path | PASS | Stream request, event, End, internal feedback, and final Response preserve permission, ABE, token, signature, and replay gates. |
| CodeGraph First, Source Verified | PASS | Current `ServiceUser`, `NDNSFMessages`, `ProviderRuntimeContext`, Qwen session, and tests were inspected before this plan. |
| Spec-Driven Durable Work | PASS | Spec, research, data model, contracts, quickstart, experiment plan, tasks, analyze, and audit are required before implementation. |
| Verify With The Right Scope | PASS | Unit -> CPU integration -> exact-SIF CPU MiniNDN -> Tiger CUDA promotion order is mandatory. |
| Cohesive Outcome-Based Tasks | PASS | Each task includes its test-first change, implementation, focused gate, and evidence; no one-file bookkeeping tasks. |

No constitution exception is required.

### Post-design gate

PASS. The contracts below preserve one Core lifecycle and one attempt/plan
authority, separate correctness gates from performance claims, and define
rollback as disabling the new streamed request option while retaining unary
behavior. No second placement protocol, final-response protocol, or media stream
mapping protocol is introduced.

## Architecture Decisions

### 1. One invocation core, two public lifecycles

The existing unary API remains unchanged. The new API is named exactly:

```cpp
template<typename RequestT, typename EventT, typename ResponseT>
std::shared_ptr<StreamedInvocationHandle<EventT, ResponseT>>
ServiceUser::RequestServiceStreaming(
  const std::vector<ndn::Name>& providers,
  const ndn::Name& serviceName,
  const RequestT& request,
  const StreamedInvocationOptions& options,
  std::function<void(const EventT&)> onEvent,
  std::function<void(const ResponseT&)> onComplete,
  std::function<void(const StreamedInvocationError&)> onError,
  size_t strategy = ndn_service_framework::tlv::FirstResponding);
```

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
    providers, service_name, request,
    options=StreamedInvocationOptions(),
)
async for event in invocation:
    ...
result = await invocation.result()
await invocation.cancel()  # idempotent; no-op after terminal completion
```

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
| `maxEvents` | 512 | 1..4096 |
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

### 5. One session key, per-event integrity

The user creates a random 256-bit event key and nonzero 64-bit `streamEpoch`
after allocating the request ID. The key grant is included in the protected
stream request and is accessible only to Providers authorized to decrypt that
service request; version 1 does not claim selected-Provider-only key secrecy.
Only the bound final Provider's signature is accepted for event publication.
Every event uses AES-256-GCM with the full Data name and canonical event header
as associated data, a unique nonce derived from stream epoch and cursor, and a
provider signature. The key is never logged. A replacement receives a new
attempt, stream epoch, and key. Internal activations/token feedback continue to
use their distinct request-scoped collaboration protection.

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

The external `GenerationTokenEvent` application payload contains token ID,
token epoch, UTF-8-safe `textDelta`, cumulative token count, and finish hint. The
Core treats it as opaque `EventT`. The End event is Core metadata and the final
Response contains the complete token list/text and result contract.

### 8. Exact KV identity and replacement

The exact `KvStateIdentityV1` fields and acceptance predicate are frozen in
[generation-state-machine-v1.md](contracts/generation-state-machine-v1.md).
Provider-local KV state is never addressed merely by session or prefix length.
Each decode verifies the complete identity. Replacement is opt-in, at most once,
and always starts a new attempt/stream epoch. Without an exact protected
checkpoint, it recomputes from prompt plus accepted token prefix. It never
copies live KV between Providers in this Spec.

### 9. Test ladder is a promotion state machine

The gate order is immutable:

```text
G0 static/contract -> G1 unit -> G2 CPU integration ->
G3 local SIF build/native preflight -> G4 exact-SIF CPU MiniNDN ->
G5 Tiger single-GPU -> G6 Tiger multi-Provider -> G7 performance qualification
```

Failure at any gate blocks later promotion. Tiger does not rebuild the SIF. It
verifies the locally generated hash and runs the same content-addressed workload
bundle. The exact cases, repetitions, metrics, and verdicts are frozen in
[validation-contract.md](contracts/validation-contract.md) and
[experiment-plan.md](experiment-plan.md).

## Source Changes

```text
ndn-service-framework/
├── InvocationStream.hpp                  # options, handle, writer, status/metrics
├── InvocationStream.cpp                  # bounded lifecycle, names, delivery
├── NDNSFMessages.hpp/.cpp                # wire types plus collision-checked TLVs
├── ServiceUser.hpp/.cpp                  # streamed request and event consumer
└── ServiceProvider.hpp/.cpp              # streaming handler/writer publication

pythonWrapper/
├── src/ndnsf/_ndnsf.cpp                  # native streamed types and methods
└── ndnsf/service.py                       # async iterator and Python options/events

NDNSF-DistributedInference/
├── cpp/adapters/qwen/QwenGenerationSession.hpp/.cpp
├── ndnsf_distributed_inference/adapters/qwen/
│   ├── generation.py                     # sealed sampling/finish/event payloads
│   ├── tokenizer.py                      # incremental decoder state
│   └── placement.py                      # unchanged one-role/one-Provider plan binding
├── ndnsf_distributed_inference/provider.py
└── ndnsf_distributed_inference/app_sdk/placement.py

examples/python/NDNSF-DistributedInference/llm_pipeline/
├── user.py
├── provider.py
└── workload.py

tests/
├── unit-tests/
│   ├── invocation-stream-message.t.cpp
│   ├── invocation-stream-lifecycle.t.cpp
│   └── di-qwen-generation-session.t.cpp
├── integration-tests/
│   ├── invocation-stream-flow.t.cpp
│   └── ndnsf-di-streamed-generation.t.cpp
└── python/
    ├── test_streamed_invocation_api.py
    ├── test_spec175_cpu_fixture.py
    ├── test_spec175_real_minindn_gate.py
    ├── test_spec175_sif_preflight.py
    └── test_spec175_evidence.py

Experiments/
└── NDNSF_DI_StreamedGeneration_Minindn.py

packaging/ndnsf-di-container/
├── bin/ndnsf-di-spec175-preflight
└── jobs/spec175/
    ├── qualify-single-gpu.sbatch
    ├── qualify-multiprovider.sbatch
    └── run-streamed-generation.sh
```

**Structure Decision**: Put application-neutral lifecycle, wire, names,
security binding, and queues in Core; put serialization/async ergonomics in the
Python wrapper; put model/KV/sampling semantics in the Qwen adapter; keep
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
sampling, KV identity, final-role event publication, feedback loop, and final
result consistency. Close with a deterministic CPU ONNX oracle and no deployed
Transformers import.

### Phase C - Fault and security closure

Close ordering, retry, retention, backpressure, cancellation, stale attempt,
tamper/replay, deadline, and optional one-replacement behavior in CPU integration
tests. No network campaign begins until these pass.

### Phase D - CPU MiniNDN

Run unchanged unary plus streamed single-, two-, and four-Provider cases with
the exact SIF and deterministic CPU model. Inject only one fault dimension per
registered case. Produce packet lineage and machine-readable manifests.

### Phase E - TigerCluster qualification

Promote one locally built SIF hash. Run one excluded cold single-GPU generation,
three independent warm single-GPU processes, then the two-/four-Provider plan.
Only after correctness passes, run the fixed performance workload and assign the
contract verdict without tuning unregistered parameters inside the campaign.

## Migration and Rollback

- Wire version 1 is opt-in through `StreamRequestOptions`; peers without support
  reject the request as `STREAM_UNSUPPORTED` instead of treating it as unary.
- Unary requests do not allocate stream state and remain the rollback path.
- The new TLVs and API are additive; no persisted unary artifact is migrated.
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
