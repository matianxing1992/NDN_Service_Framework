# Tasks: NDNSF-DI Streamed Invocation

**Input**: Design documents from
`specs/175-ndnsf-di-streamed-invocation/`

**Prerequisites**: [spec.md](spec.md), [plan.md](plan.md),
[research.md](research.md), [data-model.md](data-model.md),
[contracts/](contracts/), [experiment-plan.md](experiment-plan.md), and
[quickstart.md](quickstart.md)

**Development tests**: Each behavioral implementation task includes the
smallest failing/positive test needed to develop and review that behavior.
Formal G0--G3 manifests, repeated MiniNDN campaigns, SIF work, and Tiger work
are not development tests and MUST wait until the complete implementation
queue below is closed.

## Implementation guidance and bounded judgment for Luna

1. Work on `Experimental`; commit each completed task or inseparable task group
   atomically. Do not create a persistent Spec branch, modify `main`, or push.
2. Treat the named API/type names, TLVs, defaults, queue policy, retry/replacement
   semantics, topology, model revision, workload, repetitions, metrics, and
   verdict thresholds as the implementation baseline. Do not change them merely
   for convenience.
3. If a lower-level implementation detail is genuinely omitted, choose the
   smallest solution that matches the current code and all stated invariants,
   then record the choice, rationale, and closing test in the same task. If the
   code conflicts with a written detail, make the smallest coherent contract and
   implementation correction together. Pause for user direction only when the
   correction would change a core decision: unary compatibility, security,
   service-only Normal discovery, one shared deferred-collaboration control
   plane, selected-Provider-only event grants, one-Provider/one-role ownership,
   one placement/prefill per generation, ONNX-only deployed runtime, default-
   disabled replacement, or registered evaluation meaning.
4. Before editing an indexed source, use CodeGraph for the symbol and callers;
   after editing, use source and focused tests to verify behavior.
5. Before C++ work, configure one explicit NDN-SVS `Experimental` source/build
   pair with `--toolchain-root=/usr/bin`, verify the configure-time catch-up API
   closure probe, and require `ldd` to resolve that exact `libndn-svs` plus only
   Boost 1.71. Before T009 Python-native tests, rebuild the host diagnostic
   `_ndnsf.so` from the same current framework/toolchain. A stale host extension
   is not repaired by copying it into SIF; container-native outputs remain owned
   by T021.
6. Preserve unary/Targeted/security/StreamFacade/Spec174 compatibility. Never
   add an authorization bypass, plaintext fallback, silent stream-to-unary
   downgrade, adaptive default, dropped-event mode, or automatic replacement.
7. Never build container-runtime Python/native outputs on the host and copy them
   into SIF. Build inside the candidate SIF stage or ABI-identical sealed builder
   and run the full native preflight before promotion.
8. Use the implementation-first boundary as a hard rule: while implementation
   is changing, run only the focused compile/unit/integration checks owned by
   the task being implemented. Do not repeatedly reseal G0/G1/G2, run the full
   MiniNDN matrix, invoke the SIF builder/preflight, create or upload a candidate
   SIF, allocate Slurm resources, or start Tiger. A SIF made earlier (for
   example r30) is diagnostic-only. After the implementation queue closes, run
   one G0--G3 qualification sequence, then freeze the source/workload seal
   and build exactly one final SIF; any later source/contract/workload change
   returns to G0 instead of starting a rebuild loop.
   Treat `G3 PASS -> one final SIF -> host-orchestrated replay of SIF-contained
   NFD/NDNSF processes -> Tiger G5/G6/G6C/G7` as a serial release-candidate path.
   MiniNDN, Mininet, Open vSwitch, NLSR, and network-namespace tools remain on
   the host; they are never added to the SIF to close G4. A failure in that path
   is assigned to either the host substrate or SIF runtime and reduced to the
   cheapest reproducing G0-G3 case; it is never fixed by changing parameters
   inside the candidate or by repeatedly rebuilding SIFs.
9. Do not submit Tiger work until G0-G4 pass for the exact candidate. A Tiger
   failure is first reduced to the lowest gate that can reproduce it. Jobs from
   Spec162 or another historical harness are diagnostic-only and never close a
   Spec175 task or gate.
10. Do not use `git add -A`, reset, clean, implicit stash, or stage unrelated
   documentation, local assistant/tooling files, historical evidence, secrets,
   or raw debug output.
11. Mark a task complete only after its named acceptance gate passes. `compiled`,
   `wired`, or `one smoke ran` is not completion.

**Current progress correction (2026-08-27 audit)**: exactly 28 of 34 tasks
satisfy their named implementation/focused acceptance gates: T001--T019,
T020--T022, T024, and T029--T033. T023, T025--T028, and T034 remain open.
The previous G0/G1/G2 manifests, the selected 30/30
M01--M10 matrix, its additional M09 `returncode=-11`, and every existing SIF
candidate are historical subjects. They remain preserved as regression inputs,
but they do not qualify the corrected source. The implementation queue is now
closed, and T020's current-source G0/G1/G2 qualification plus T022's complete
42-process G3 matrix are closed. The next valid step is T023's exact-SIF/G4
candidate. No focused test, stale manifest, or
diagnostic SIF waives an open qualification or promotion gate.

**Local regression correction (2026-08-27)**: the asynchronous four-Provider
production-ingress mapping test, candidate-only V3 role-spec compatibility, and
CPU topology identity regressions were fixed and revalidated. T033's M11--M14
harness implementation is closed; its clean-source repeated qualification is
now recorded by T020/T022. Current-source G0--G7, SIF, CUDA, and Tiger gates
remain open as listed below. See
`evidence/current-local-regression-20260827.md`.

**Python contract correction (2026-08-27)**: the validated
`TOKEN_STREAMING` generation specification in
`llm_pipeline/provider.py` now returns for ordinary non-conversation requests;
the return path was previously nested under the conversation branch. The
current focused Spec175/Provider-generation Python regression is 204 passed;
the broader Spec175/168/170 regression is 349 passed with 10 expected skips,
and the dedicated conversation regression is 36 passed. A real four-Provider
M01 process also confirmed that both the request envelope and terminal
response carry `TOKEN_STREAMING` rather than the unary `FULL` label. This is a
source/regression correction only; the implementation closures below additionally
require the request-ID and real-Provider evidence recorded in
`evidence/implementation-closure-request-id-20260827.md`. It does not close any
formal G0--G7, SIF, CUDA, or Tiger acceptance gate.

**Status rule**: a checked box means the task's acceptance gate is closed for
the current source/workload identity. “Implemented checkpoint”, “focused
tests pass”, “wired”, and “diagnostic candidate” are evidence levels, not task
completion. A task is unchecked until its required repetition count, manifest,
and current-source hash are present.

### Mandatory current execution queue: qualification after implementation

The source implementation queue is closed for the host/CPU contract:

```text
Stateful generation:               T014 -> T015 [closed]
Fault/recovery implementation:     T016 -> T017 -> T018 -> T019 [closed]
Conversation/state implementation: T029 -> T030 -> T031 -> T032 [closed]
Frozen harness/launcher source:    T033 -> T024 [closed]
```

The formal promotion sequence is now:

```text
T020 (G0--G2) [closed] -> T022 (G3 M01--M14) [closed] -> T023 (one final SIF + G4)
-> T025 (current-SIF control + G5) -> T026 (G6) -> T034 (G6C)
-> T027 (G7) -> T028 (closure)
```

Historical G0--G4 results, the retained M09 signal exit, and old SIF failures
remain regression inputs. They do not satisfy the current-source sequence.

### Formal native I-case boundary

The formal I01-I03/I15 subject is one reusable C++ production harness, not a
collection of stronger-looking unit tests. The harness MUST enter through the
real `BeginCollaboration` Request/ACK path, close an ACK-driven
`CollaborationPlan`, publish encrypted Selection projections, invoke the
checked-in tiny ONNX graph through `NativeProviderHandler` and
`OnnxRuntimeModelRunner`, and observe the real event/End/Response consumer.
It MUST record the same request ID across ACK closure, plan, Selection,
prefill, every activation and token-feedback edge, external events, End, and
the terminal Response. Provider/role maps are inputs produced from signed ACK
capability/residency data; they MUST NOT be hard-coded in the caller payload.
The direct adapter test, Python oracle, deterministic native runner, and
generic one-Provider stream tests remain prerequisites only. They MUST NOT be
added to `REGISTERED` in `scripts/run_spec175_integration_gate.py`.

## Format: `[ID] [P?] [Story] Description`

- **[P]** means files and acceptance evidence do not depend on another unfinished
  task in the same phase.
- **[US1..US5]** maps to the five user stories in `spec.md`.
- All commands below run from repository root unless explicitly stated.

## Phase 1: Setup and frozen baseline

**Purpose**: Turn the written contracts and current code reality into mechanical
guards before behavior changes.

- [X] T001 Implement the G0 contract/traceability gate for FR-001..FR-057 including FR-030a and SC-001..SC-012 by adding TLV-collision, placeholder, cross-document default/name, service-only-Normal API, same-collaboration owner, selected-Provider-grant, source-owner, runtime-import, task/test/evidence, and dirty-input checks in `scripts/spec175_contract_gate.py`, `tests/python/test_spec175_contract_gate.py`, and `specs/175-ndnsf-di-streamed-invocation/traceability.md`; first make mutation fixtures fail, then retain an honest `BLOCKED_EXPECTED` manifest at `results/spec175/g0/qualification-manifest-v1.json` without changing any production behavior. The initial source census MUST fail clearly on the then-missing streamed symbols, the legacy Qwen `maxGeneratedTokens<=32` bound versus the registered 64-token workload, legacy `/LLM/Stage/*` versus canonical `/LLM/Pipeline/Stage/*`, and the per-token `distributed_inference(...)` loop.
  - **Historical content-bound seal checkpoint (2026-08-24, pre-Face-fix)**: `scripts/spec175_source_seal.py` and `--source-seal` bind the then-current HEAD, exact source-input dirty-path set, and every changed-file SHA-256. That sealed subject passed G0 with no blockers; feature evidence remains separately bound by G0 document digests. The later `publishStreamEventOnFaceEventLoop(...)` source change invalidated that subject for promotion. The current replacement seal is recorded by T020; see `evidence/t020-full-python-suite-20260824.md`. This does not waive G1 or candidate-SIF identity checks.
  - **Fixed owner map**: current `ServiceUser`, `ServiceProvider`, `NDNSFMessages`, `ProviderRuntimeContext`, `QwenGenerationSessionStateMachine`, exact collaboration Data, integration fixture, MiniNDN gate, and local-SIF tooling.
  - **Pass**: the gate's own positive/mutation tests pass, the recorded TLV range is proven collision-free, every requirement has a planned task/test/evidence link, and the first repository run emits an honest `BLOCKED_EXPECTED` manifest naming the missing/drifted implementation. Formal G0 becomes `PASS` only after T020 reruns this same gate against the implemented source; development may proceed from the retained expected-negative baseline, but promotion may not.

- [X] T002 [P] Build the byte-reproducible CPU ONNX oracle required by FR-050..FR-052 and SC-003 by adding the frozen vocabulary-32/hidden-8/four-block fixture generator, combined graph, graph-valid two-/four-role partitions, tokenizer, prompts, expected token sequences, and content manifest in `tests/fixtures/spec175/build_tiny_causal_onnx.py`, `tests/fixtures/spec175/tiny-causal-lm-v1/`, and `tests/python/test_spec175_cpu_fixture.py`; include both attention-KV and recurrent/convolution state components so the fixture exercises the hybrid `DecodeStateBundleV1`, regenerate twice under the lock, and require byte-identical manifests plus real ORT CPU execution.
  - **Fixed output**: normal `[4,5,6,7,8,9,10,2]`, early EOS `[14,15,2]`, and a no-EOS eight-token max case.
  - **Evidence boundary**: this tiny fixture is the deterministic CPU protocol/state oracle; it is not the Qwen3-0.6B or Qwen3.6-27B qualification subject and cannot close CUDA, SIF, Tiger, or performance gates.
  - **Pass**: one-, two-, and four-role local ORT prefill plus incremental decode matches the committed token/tensor/state oracle; independently omitting either state family changes/fails the next-token result; no fake runner or full-context-only graph can satisfy the test.

---

## Phase 2: Foundational Core stream contract

**Purpose**: Implement the reusable wire, names, lifecycle, security binding,
queues, and exact Data transport that block all user stories.

**CRITICAL**: No application/DI integration starts until T003-T007 pass.

- [X] T003 Encode the version-1 request/event/End/completion and deterministic name contract for FR-009..FR-018 and FR-039..FR-041 by writing failing canonical/mutation vectors, allocating exactly the 36 collision-cleared TLVs `0xF661..0xF684`, and implementing `StreamRequestOptions` including attempt epoch 1/2, key commitment/provider-specific grant envelopes, `InvocationEventMessage`, `StreamCompletion`, binding/transcript digests, nonce/AAD derivation, and exact name build/parse in `ndn-service-framework/NDNSFMessages.hpp`, `ndn-service-framework/NDNSFMessages.cpp`, `ndn-service-framework/InvocationStream.hpp`, `ndn-service-framework/InvocationStream.cpp`, and `tests/unit-tests/invocation-stream-message.t.cpp`.
  - **Completed checkpoint (2026-08-22)**: standalone types are embedded in `RequestMessage`/`ResponseMessage` copy/clear/encode/decode paths. Eleven focused native cases pass, including explicit wrapped-key `STREAM-GRANT` envelope semantics, Targeted grant nesting, outer duplicate/final-position rejection, missing/duplicate/unknown/reordered/noncanonical/overflow mutations, fixed canonical vectors, oversize and wrong-End rejection, and End/Response completion matching. Existing DeploymentControl, Targeted, token/replay, crypto/authorization, and encrypted-permission suites also pass; see `evidence/t003-wire-20260822.md`. The grant is still only a wire container until T007 authenticates its Provider-specific binding.
  - **Reject**: missing/duplicate/out-of-order/unknown field, noncanonical integer, bad range/digest/name, oversize, wrong End shape, or completion mismatch.
  - **Pass**: canonical wire re-encodes byte-identically; every single-component mutation fails before payload delivery; existing message suites still pass.

- [X] T004 Implement the single shared user/provider lifecycle and terminal authority for FR-001..FR-008, FR-014, FR-018, FR-020, and FR-042..FR-046 in `ndn-service-framework/InvocationStream.hpp`, `ndn-service-framework/InvocationStream.cpp`, `ndn-service-framework/ServiceUser.hpp`, `ndn-service-framework/ServiceProvider.hpp`, and `tests/unit-tests/invocation-stream-lifecycle.t.cpp`, starting from exhaustive transition, duplicate-terminal, cancel-race, deadline, callback-exception, and capacity tests.
  - **Fixed states**: the exact user/provider state sets in `data-model.md`; terminals absorb; `cancel()` is idempotent; destruction does not silently cancel.
  - **Fixed ownership**: ordinary streamed service and deferred collaboration attach to this same state/terminal owner; attachment never allocates a second request ID or publishes another Request.
  - **Fixed queues**: admission precedes cursor commit; capacity waits propagate bounded backpressure; there is no drop mode or unbounded allocation.
  - **Pass**: every valid transition succeeds once, every invalid transition has zero side effects, and high-water marks never exceed signed options.
  - **Completed checkpoint (2026-08-22)**: `StreamInvocationLifecycle` is attached to existing user/provider request contexts without allocating a second request or publishing another Request; provider pending cleanup fences an active owner before erasure. The lifecycle/queue suite (9 cases), message regression (10 cases), focused attachment/cleanup case (1 case), and full 99/99 unit-test build pass; see `evidence/t004-lifecycle-20260822.md`. Event transport, exact Interest delivery, public APIs, and DI generation remain later tasks.

- [x] T005 Implement provider event admission, AES-256-GCM encryption, ECDSA signing, immutable signed-wire retention, exact Interest satisfaction, retry-safe republication, End admission, and completion binding for FR-009..FR-020 and FR-039..FR-044 in `ndn-service-framework/InvocationStream.cpp`, `ndn-service-framework/ServiceProvider.cpp`, `ndn-service-framework/ServiceProvider.hpp`, `ndn-service-framework/HybridMessageCrypto.cpp`, and `tests/unit-tests/invocation-stream-lifecycle.t.cpp`.
  - **Historical partial checkpoint (2026-08-23, refreshed)**: the standalone `StreamEventPublisher` and real Normal integration path pass admission/cursor, AEAD, signed Data, exact IMS retention/expiry, byte-identical retransmission, End, terminal, Provider-failure, and SVS-bound recovery tests. Queue admission now holds its slot until publication completes, and the capacity-1 regression proves the in-flight high-water mark is exactly one. The current-source focused closure in `evidence/t005-t008-t016-t019-current-20260826.md` supersedes the earlier partial boundary; formal G0--G2 still remains a separate qualification gate.
  - **Fixed atomic order**: serialize -> bound -> transcript candidate -> queue admit -> cursor commit -> encrypt/sign -> retain/publish; failure after commit terminates and never renumbers.
  - **Pass**: retries reuse identical Data wire/name, retention expires only by contract, queue deadline fails explicitly, and no key/plaintext enters logs.

- [x] T006 Implement the bounded user exact-Interest window, signature/decryption/lineage validation order, reorder buffer, duplicate suppression, gap retry, Draining state, End/Response closure, and callback dispatch for FR-010..FR-020, FR-038, and FR-042..FR-048 in `ndn-service-framework/InvocationStream.cpp`, `ndn-service-framework/ServiceUser.cpp`, `ndn-service-framework/ServiceUser.hpp`, and `tests/unit-tests/invocation-stream-lifecycle.t.cpp`.
  - **Historical partial checkpoint (2026-08-23, refreshed)**: the consumer now passes exact Interest/retry, reorder, duplicate, retention-expiry/permanent-gap, signature/decryption/binding, Response-before-End, terminal mismatch, cancellation, and callback-exception paths. Application callbacks execute on a bounded worker queue rather than the Face thread; the capacity-1 test directly proves callback queue high-water 1 and callback concurrency 1. The current-source stream suite and Python contract suite close the focused implementation boundary; formal G1 is tracked separately by T020.
  - **Fixed defaults**: window 16, lifetime 500 ms, retries 3, reorder 64, completion grace 5 s; no environment override.
  - **Pass**: ordered at-most-once callbacks under reorder/duplicate/one-loss, exact timeout outside retention, Response held behind gaps, and stale/tampered Data never enters the buffer.

- [x] T007 Bind Normal and Targeted streamed requests to the existing authorization/token paths for FR-006, FR-031..FR-041, and FR-045 by generating the key/commitment after request-ID allocation, placing only the commitment in a Normal Request, projecting a certificate-wrapped grant only to the final-role Provider through the accepted plan's Selection, allowing a request-carried target-wrapped grant only on the cached selection-free Targeted path, deriving the Normal accepted plan or canonical Targeted binding digest, creating a new key/commitment/grant/epoch on replacement, and rejecting unsupported/downgraded peers in `ndn-service-framework/ServiceUser.cpp`, `ndn-service-framework/ServiceProvider.cpp`, `ndn-service-framework/NDNSFMessages.cpp`, and the existing security/Targeted unit suites plus `tests/unit-tests/invocation-stream-lifecycle.t.cpp`.
  - **Historical partial checkpoint (2026-08-23, refreshed)**: Normal Selection carries one RSA-wrapped event key only for the selected final role; selected nonfinal and unselected ACK Providers receive no stream grant. Provider binding and commitment checks, Normal/Targeted paths, and I09 tamper/unselected execution isolation pass. Signed attempt epoch 1/2 is now part of `StreamRequestOptions` and every grant/event binding; Targeted replacement rejects. The current full unit/integration and three-repeat I04--I13 evidence closes the focused authorization/recovery boundary; formal source-sealed qualification remains T020.
  - **Fixed mode**: Normal default; Targeted requires one Provider and preserves current behavior: cached token -> selection-free fast path; cache miss -> the same streamed invocation is a `TargetedBootstrapRequest`; refill already in flight -> bounded one-Provider normal path. No `RequestServiceStreamingTargeted` alias and no unary downgrade.
  - **Pass**: permission, ABE service attributes, UserToken, ProviderToken, provider permission, signer, replay, policy epoch, commitment/grant mismatch, unselected/nonfinal unwrap, and outer/plaintext-grant negatives fail closed; evidence/logs contain only key digests, and unary requests allocate no stream key/state.

**Checkpoint**: G1 Core subsets pass; the generic non-DI lifecycle is ready.

---

## Phase 3: User Story 1 - Consume One Ordered Streamed Invocation (Priority: P1) MVP

**Goal**: Applications use the frozen C++ or Python lifecycle and receive ordered
events plus one complete result without changing unary behavior.

**Independent Test**: A one-Provider deterministic service publishes five typed
events and one result through real Core codecs; C++ callbacks and Python async
iteration each observe the exact lifecycle, while the existing unary suite is
unchanged.

- [x] T008 [US1] Expose exactly the service-only Normal and same-name single-target Targeted C++ `RequestServiceStreaming<RequestT,EventT,ResponseT>` overloads, `StreamedInvocationHandle`, `addStreamingHandler`, and `StreamedResponseWriter` contract for FR-001..FR-008 and FR-045..FR-046 in `ndn-service-framework/ServiceUser.hpp`, `ndn-service-framework/ServiceUser.cpp`, `ndn-service-framework/ServiceProvider.hpp`, `ndn-service-framework/ServiceProvider.cpp`, and `tests/integration-tests/invocation-stream-flow.t.cpp`, with no-Provider-list discovery, Normal/Targeted argument rejection, serialization failure, handler lifetime, five-event success, zero-event success, failure, cancellation, and full unary/Targeted regression cases.
  - **Historical partial checkpoint (2026-08-23)**: C++ public overloads, writer/handle types, provider dispatch, exact Interest consumer, callback lifetime fencing, and a reusable real one-Provider integration fixture are present. The current 70-case integration suite and 69-case Python contract suite now include capacity, retry, unary/Targeted, and lifecycle regressions; see `evidence/t005-t008-t016-t019-current-20260826.md`.
  - **Pass**: suite `Spec175InvocationStream` proves service-only Normal discovery, exactly one Targeted provider, callbacks 1..5 then one complete result, zero-event completion, no post-cancel callbacks, and no changed existing API signature/behavior.

- [X] T009 [US1] Rebuild and prove the host diagnostic Python extension against the same current framework/NDN-SVS/Boost closure, then expose exact native bindings, the single-consumer Python user async/callback API, and the Python Provider Core-writer API for FR-003..FR-005 and FR-049 in `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/service.py`, `pythonWrapper/ndnsf/__init__.py`, and `tests/python/test_streamed_invocation_api.py`, using real native messages and event delivery rather than Python-only queue/cursor simulation.
  - **Closed checkpoint (2026-08-23)**: `examples/run_python_streamed_invocation_regression.sh` passed with a real NFD, ServiceController, Python Provider, and Python User. It delivered five ordered events plus one final result through both async-iterator and callback consumers, contained a handler exception, and fenced late Core-writer calls. The runner now validates the NFD socket and `nfdc status` instead of trusting a stale process name. See `evidence/t009-python-native-stream-20260823.md`. This closes only the generic Python/native boundary; T015 remains the separate automatic DI process proof.
  - **Historical checkpoint (2026-08-22)**: Python 3.8 host diagnostic extension imports against the current framework shared library; native `StreamWriter`, Provider registration, User callback request, ONNX boundary, and surface tests pass. This was superseded by the real-process evidence below; the exact Python-3.10 SIF proof remains a T020/T021 promotion requirement.
  - **Current-source refresh (2026-08-23)**: the CPython-3.8 host extension was rebuilt after the attempt/error-binding changes, imported from its exact SOABI path, resolved the current framework/Boost-1.71/NDN-SVS Experimental closure with no missing library, and passed focused Python cases. A stale differently named CPython-3.10 file in the package directory was not loaded and must never be selected by a wildcard during SIF sealing. The generic host boundary is now closed; SIF sealing remains separate.
  - **Superseded audit note (2026-08-23)**: an earlier audit incorrectly treated the high-level API as absent and relied mainly on fake handles. The real `request_service_streaming(...) -> StreamedInvocation` surface and native process proof are now recorded in `evidence/t009-python-native-stream-20260823.md`.
  - **Host implementation checkpoint (2026-08-23)**: the contract-level handle/API, full Python Provider writer/context registrations, complete error-code range, and explicit `max_events`/End semantics are implemented. The host extension links the rebuilt current framework, focused facade cases pass, and a real NFD/Controller/Python-Provider/Python-User regression passes iterator delivery, callback delivery, contained Provider failure, and post-handler writer fencing. T009 is closed for the generic host boundary; exact CPython-3.10 candidate-SIF proof remains T020/T021 work; see `evidence/t009-python-native-stream-20260823.md`.
  - **Fixed user behavior**: positional `service_name, request` with no Provider list for Normal; keyword-only `target_provider` required only for Targeted; bytes default, optional event/response decoders, `async for`, awaitable `result()`/`cancel()`, immutable request ID/status/metrics, double-consumer `RuntimeError`.
  - **Fixed Provider behavior**: `add_streaming_handler`, `streaming_handler`, and `add_streaming_context_handler` pass the native writer to a synchronous existing-worker handler; exception and return-without-terminal fail the invocation; writer use after return fails closed.
  - **Pass (host boundary)**: host import and `ldd` contain no unresolved/stale ABI symbol, real `_ndnsf` user async tests, Provider writer/context tests, exception containment, and lifetime negatives pass; no event loop callback or Provider handler is invoked from the Face thread. The exact-SIF rerun is required by T020/T021 and is not implied here.

- [X] T010 [US1] Reuse the same Core writer and terminal guard at the DI application boundary for FR-007, FR-017, FR-023..FR-024, and FR-046 by adding only `ProviderRuntimeContext.publish_event`, `finish_stream`, and `stream_cancelled` in `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`, binding them in `pythonWrapper/ndnsf/service.py` and `pythonWrapper/src/ndnsf/_ndnsf.cpp`, and extending `tests/python/test_spec168_provider_generation.py` and `tests/integration-tests/ndnsf-di-core-flow.t.cpp`.
  - **Completed checkpoint (2026-08-22; audit correction 2026-08-23)**: the DI bridge returns Core cursors, accepts named finish reasons, exposes cancellation/fencing, preserves unary fallback, and shares one terminal guard with `publish_final_response`. Selection reservation release now follows accepted terminal publication; native streamed legacy publication closes through the Core stream terminal instead of bypassing End. Native Python import/ldd, 18 Python bridge/provider tests, and the DI/Core cursor/terminal integration case pass; see `evidence/t010-di-writer-20260822.md` and `evidence/audit-state-commit-20260823.md`.
  - **Pass**: streamed finish and existing `publish_final_response` compete for one terminal claim; exactly one wins; nonstreamed DI providers remain unchanged.

**Checkpoint**: User Story 1 independently passes with a generic one-Provider
service. This is the MVP; do not claim LLM streaming yet.

---

## Phase 4: User Story 2 - Generate Tokens Through One Selected ONNX Plan (Priority: P1)

**Goal**: One selected ONNX plan runs prefill once, incremental exact decode-state reuse,
final-role sampling/feedback/events, and one complete result.

**Independent Test**: The tiny CPU ONNX fixture runs one-, two-, and four-role
plans and matches exact token/final-result oracles with one Request/plan/prefill.

- [X] T011 [US2] Correct and extend `QwenGenerationSessionStateMachine` for FR-021..FR-022, FR-027..FR-028, FR-033..FR-038 by writing failing transition tests for Prefilling/Decoding/Draining, EOS/stop early success, exact max success, invalid early max, cancellation, deadline, one replacement, stale epoch, and one terminal claim, then updating `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.hpp`, `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp`, and `tests/unit-tests/di-qwen-generation-session.t.cpp`. Migrate the Spec175 role identity to `/LLM/Pipeline/Stage/{index}` and make the validated maximum cover the registered 64-token workload without weakening the signed `maxEvents-1` bound.
  - **Completed checkpoint (2026-08-22)**: canonical roles, 64-token bound, early EOS/stop completion, exact max completion, reason-matched terminal completion, cancellation, deadline, one replacement, stale epochs, and one terminal claim are covered by 14 passing native cases; see `evidence/t011-qwen-state-machine-20260822.md`.

- [X] T012 [US2] Implement sealed `GenerationTokenEventV1`, Greedy and fixed-seed sampler contracts, and stateful Unicode-safe incremental detokenization for FR-027..FR-030 in `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/generation.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/tokenizer.py`, `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.hpp`, and `tests/python/test_spec175_cpu_fixture.py`.
  - **Completed checkpoint (2026-08-22)**: sampler, token-event digest, Unicode/empty-delta/stop handling, standalone digest-bound tokenizers, and deployment import boundaries pass six Python cases; see `evidence/t012-generation-tokenizer-20260822.md`.
  - **Fixed tests**: exact Greedy logits; seeded fixed-logit TopK/TopP repeatability; leading-space, empty-delta, multibyte Unicode, EOS, and stop strings across two/three tokens.
  - **Pass**: deployed imports include `tokenizers` but not Transformers/PyTorch; accepted text deltas concatenate to the oracle final text.

- [X] T013 [US2] Implement exact `DecodeStateIdentityV1` and adapter-owned `DecodeStateBundleV1`, adapter-certified stateful prefill/decode ONNX I/O schema, persistent ORT session/I/O binding, and the automatic Provider-owned candidate/committed state transaction required by FR-022 and FR-030..FR-032a in `NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp`, `NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.cpp`, `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.hpp`, `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/stateful_onnx.py`, `tools/ndnsf-di/export_spec175_qwen36_stateful_onnx.py`, `tests/python/test_spec175_qwen_stateful_onnx.py`, `tests/unit-tests/di-qwen-generation-session.t.cpp`, and the existing `tests/unit-tests/distributed-inference-async-runtime.t.cpp`. Start with failing production-call-path tests, then wire store lookup/commit/rollback, predecessor-epoch matching, pin/evict/release, clean recompute, CPU materialization, and CUDA device-resident I/O binding. The Qwen3.6 bundle MUST bind both full-attention KV and linear-attention recurrent/convolution state. Its canonical artifact inventory MUST be language-model-only text, seal `decodeMode=single-token-autoregressive` and `mtpEnabled=false`, and reject any loaded vision encoder/projector or MTP/speculative head. PyTorch/Transformers are permitted only inside the offline exporter and are excluded from the deployment SIF.
  - **Lifecycle boundary**: this task owns only the request-local store and its terminal handoff seam. A non-conversation terminal releases the entry. A conversation terminal may transfer a finalized adapter-owned state handle to T030/T031's promotion transaction, but T013 must not weaken `requestId` matching or itself claim cross-request reuse. The later conversation-scoped cache is a distinct store and acceptance surface owned by T030/T031.
  - **Complete-identity implementation order (fixed 2026-08-25)**:
    1. Define one C++ `DecodeStateIdentityV1` type shared by the Qwen adapter and Provider runtime; do not maintain a second reduced cache identity or compare only a precomputed opaque key.
    2. Populate immutable/runtime-authority fields (`modelDigest`, `graphSemanticDigest`, `artifactDigest`, `adapterDigest`, `tokenizerDigest`, `runnerDigest`, `roleName`, `roleSplitDigest`, `layerRange`, `precision`, `layoutDigest`, `stateSchemaDigest`, ordered `stateComponentDigests`, `runtimeAbiDigest`, `securityDomainDigest`, `providerIdentity`, `providerBootId`, and the cache-incarnation `cacheEpoch`) only from the validated content-addressed artifact/adapter manifest, accepted Provider projection, selected Provider runtime, and loaded runtime ABI. A free-form runner metadata value is usable only after it is bound by those authorities. Missing fields fail before prefill; tests may use explicit fixture digests but production code may not synthesize fallback digests.
    3. Populate request fields (`requestId`, `attemptEpoch`, and `generationId`) only from the authenticated collaboration/generation contract. They are not runner-global metadata.
    4. Add explicit `stateInferenceEpoch` and optional `predecessorInferenceEpoch` to both C++ and Python identity types and every equality/validation/serialization path. Prefill produces state epoch 0 with no predecessor; decode state epoch `e` requires predecessor `e-1`. Keep this lineage distinct from the Provider-local `cacheEpoch`.
    5. Let `NativeEpochCoordinator` own all dynamic fields. Epoch 0 derives `prefixDigest`, `prefixTokenCount`, and `positionDigest` from the exact admitted canonical prompt-token IDs and adapter position inputs. Every later candidate extends the logical prefix by exactly the authenticated token admitted through TOKEN_FEEDBACK and derives the new position commitment. All roles for one inference epoch receive and verify the same authenticated logical prefix digest/count; never hash a role-local activation/tensor bundle to manufacture this identity. A Provider role carries the exact predecessor identity and exact candidate identity into `NativeProviderRuntime`; the runtime must not reconstruct them from session ID or epoch number alone.
    6. Make `KvStateStore` use a compact lookup key only as an index. `beginTransition` must compare the complete expected predecessor identity; `stageCandidate` must enforce immutable-field equality, contiguous logical prefix, exact state/predecessor epochs, unchanged cache-incarnation epoch, and exactly one candidate; commit/rollback retain the complete identity.
    7. Add production-path mutation tests for every field, state/predecessor epoch, reordered component digests, role-local activation substitution, common-prefix lineage mismatch, concurrent generations, concurrent attempts, Provider reboot, cancellation, deadline, event/activation/feedback publication failure, and capacity pressure. The runner call count must stay zero on a missing or mismatched predecessor. Multi-role tests must prove equal logical prefix digest/count across roles while role identities and activation bytes differ.
    8. Add a CPU production-runner cached-versus-full-prefix semantic control with identical prompt/artifact/split/seed and exact output parity. Record actual input token/activation extent, prefix extent represented by state, and machine-derived prefix work avoided for every epoch. The cached branch must prefill once and must not call a full-prefix runner on a reported hit. This gate proves effective reuse semantics but makes no CPU timing claim.
    9. Only after the CPU identity/lifecycle/effectiveness matrix passes, implement persistent CUDA ORT I/O binding. Map every declared `*_out` state allocation to its exact successor `*_in` name, require every graph-declared non-state input on prefill and decode, and materialize adapter-certified `attention_mask`, `position_ids`, or `cache_position` inputs from the same sealed position policy used by `positionDigest`. Local tests must reject a missing declared position input and a mismatched state-output/input pair. CPU `TensorBundle` materialization is not CUDA-cache evidence.
    10. Keep implementation and qualification separate. T013 closes only after the production source path and local/static/mutation tests above are complete. T025, not T013, executes the real Qwen3.6-27B CUDA subject and proves device residency, zero complete-state host round trips after prefill, and cache effectiveness. A GPU allocation is therefore not required before T020, but unimplemented CUDA/state/position wiring cannot be deferred to the post-SIF campaign.
  - **Artifact progression**: the tiny CPU fixture closes the generic state contract; the offline exporter may then produce a pinned 27B candidate manifest, but that manifest remains `PROPOSED_UNQUALIFIED` until T025 executes every stateful stage with CUDA ORT. Existing `useCache=false` graphs cannot be upgraded by metadata alone.
  - **Fixed match**: every identity field in `data-model.md` exact; no partial or cross-request prefix reuse in V1.
  - **Partial checkpoint (2026-08-23)**: Python and C++ identity/bundle transactions now reject field, component-layout, prefix/position, cache-epoch, and admission mutations while preserving the committed candidate. The persistent Python ORT wrapper and C++ runner retain one session; a declared stateful C++ manifest now requires exact graph I/O names plus all three state families (`attention_kv`, `recurrent_state`, `convolution_state`). The offline sealing tool and 7 Python/native-focused state cases pass. The native adapter's incremental path now executes the checked-in tiny causal model for eight persistent-state epochs and returns the exact EOS sequence; this is adapter-level evidence, not the Qwen qualification subject. Real Qwen3.6 export, provider-local tensor-state commit/recompute, and CUDA stage continuity remain open; see `evidence/t013-stateful-onnx-partial-20260823.md` and `evidence/t014-native-onnx-stream-adapter-20260823.md`.
  - **Subject-identity correction (2026-08-23; historical qualification note)**: the official pinned source is multimodal and MTP-capable, so the qualification profile now seals FP16, `decodeMode=single-token-autoregressive`, `modality=text-only`, and `mtpEnabled=false`. The offline sealer rejects declared vision/MTP components and the fixed adapter no longer advertises BF16. These contract/unit checks pass; graph-derived component inventory from the real ONNX artifact remains a T025 qualification requirement. See `evidence/audit-qwen36-subject-identity-20260823.md`.
  - **False-admission guard (2026-08-24)**: inspection of the remote Qwen3.6 candidate found empty `cacheInputs/cacheOutputs` and legacy `past_key/present_key` tensors, with no recurrent/convolution state. `build-qwen-onnx-stage-manifest.py` now rejects that shape and requires all three state families plus the sealed text-only single-token subject fields. The regression passes; this prevents metadata-only promotion but does not close real Qwen3.6/CUDA state continuity. See `evidence/t013-stateful-manifest-guard-20260824.md`.
  - **Production CPU transaction checkpoint (2026-08-25)**: the real `NativeEpochCoordinator -> NativeProviderRuntime -> ProviderRoleWorker` path now assigns an inference epoch, automatically pins and loads the exact predecessor, stages one successor candidate, commits only after dependency/token-feedback publication succeeds, rolls back a failed transition without replacing the committed predecessor, releases terminal state, and reports hit/miss/commit/rollback/recompute/eviction/cleanup plus pinned/candidate counts. `ProviderRoleWorker` removes declared private state tensors at the NDN publication boundary while retaining the complete local runner result for the state transaction. Focused C++ cases pass for automatic reuse, production coordinator reuse, state-free dependency publication, pinned deterministic LRU, failed-decode rollback, boot invalidation, terminal cleanup, and explicit failure when decode state is absent after prefill; the production ONNX path no longer silently zero-fills a missing decode state.
  - **Complete-identity and V3-lineage checkpoint (2026-08-25; historical status)**: one shared C++ `DecodeStateIdentityV1` reaches `RoleSpec`, `KvStateBinding`, and exact Store comparisons. `NativeProviderHandler` now constructs its trusted static template from the authenticated V3 assembly/projection, content-addressed artifact and runner identities, selected Provider/boot identity, and runtime ABI; incomplete or inconsistent authority fails before execution. `GenerationEpochLineageV1` serializes the canonical logical token prefix/count plus position lineage on activation/TOKEN_FEEDBACK plaintext, and `NativeEpochCoordinator` advances explicit `stateInferenceEpoch`/`predecessorInferenceEpoch` while keeping `cacheEpoch` constant for the lineage. Nine focused state/lineage unit cases and the 14-case native I01-I13/I15 matrix passed at this checkpoint. The later CPU identity/lifecycle and cache-effectiveness checkpoints close steps 7--8; the remaining real Qwen3.6-27B CUDA execution evidence belongs to T025.
  - **Device-binding audit checkpoint (2026-08-26; fixed in current source)**: the audit found that CUDA state outputs were retained under `*_out` while the next epoch looked up `*_in`, so the advertised device-state lookup always missed. The current runtime validates a one-to-one state input/output contract and stores every device allocation under its exact successor input name; the native mapping regression passes. Python decode validation rejects an omitted graph-declared `attention_mask` or `position_ids`, and the current source now includes the adapter-certified causal-position materializer. The remaining device-residency and host-round-trip measurements are T025 qualification work.
  - **Streaming CUDA state-retention correction (2026-08-26; fixed in current source)**: the stateful streaming adapter marks CUDA streamed epochs as device-resident execution, suppresses complete state materialization on every token, and exports the terminal state bundle once after the final event. Intermediate state allocations remain indexed by their exact successor `*_in` names and are rebound through ORT I/O binding. The native coordinator's unary per-role transaction still uses its host `ProviderRoleResult` representation until the real CUDA multi-role subject is qualified; that qualification evidence belongs to T025 and does not reopen T013's implementation gate. See `evidence/t013-cuda-streaming-state-retention-20260826.md`.
  - **Device-state cleanup correction (2026-08-27)**: the CUDA streamed runner now uses an RAII cleanup guard so normal completion, cancellation, and runner exceptions erase the session-keyed device-state map after terminal state export. This prevents stale device allocations from being reused by a later request or counted as a cache hit. The exact Qwen3.6 CUDA residency/zero-round-trip qualification remains T025.
  - **Implementation closure (2026-08-27)**: the production state path now has one shared C++ identity, exact predecessor/candidate validation, CPU request-local transaction and cache-effectiveness control, graph-signature validation, adapter-certified causal-position materialization, persistent ORT I/O binding, exact `*_out -> *_in` device-state mapping, streamed terminal export, and RAII cleanup. The current focused Python state/mutation suite, native state/runner suites, and full 600-case unit suite pass. Historical source-sealed local gates remain preserved but are not current qualification evidence. This closes T013's implementation acceptance. Exact graph-derived Qwen3.6 subject identity, CUDA residency, and measured zero host round trips are qualification evidence owned by T025 and are not claimed here.
  - **Conversation observation correction (2026-08-27)**: an `APPEND_DELTA` epoch-zero observation now records the exact promoted parent prefix as `prefixWorkAvoided`; it keeps `conversationStateHit` distinct from the request-local `decodeStateHit` metric. The native three-token-parent regression reports three avoided tokens and the full 600-case unit suite passes.
  - **Python I/O-contract correction (2026-08-27)**: the adapter-owned Python `StatefulOnnxIOContractV1` now validates the same one-to-one `*_out` to successor `*_in` mapping as the native runner, rejects orphaned state tensors, and exposes `materialize_causal_position_inputs(...)` for the certified attention-mask/position-IDs/cache-position policy. Two focused regressions cover orphan rejection and epoch-accurate position tensors; the full Spec175 Python/API suite passes 174 cases. This closes the Python contract seam; real Qwen3.6 graph-derived position inputs and CUDA zero-host-round-trip evidence remain T025 qualification work.
  - **Cancellation/deadline checkpoint (2026-08-25)**: `NativeProviderHandler` now derives a coordinator stop check from the authenticated streamed request's absolute deadline and shared cancellation lifecycle. `NativeEpochCoordinator` checks it before new work and at every post-run publication/commit boundary; a stop after runner return rolls back the candidate and publishes neither an event nor feedback. Focused unit tests prove zero runner calls for pre-run cancellation and rollback/no residue for a post-run deadline. The real I08 Request/ACK/plan/Selection/native path additionally proves both Providers finish cancellation with zero committed entries, zero pins, zero candidates, and one terminal cleanup. An end-to-end expired-deadline regression exposed and fixed a multi-role bug: stop checks had incorrectly been gated by external stream-publisher ownership, so an upstream role could execute once. Every role carrying authenticated stream options now checks the absolute deadline; cancellation observation remains with the role that owns the shared stream lifecycle. The corrected two-Provider path completes ACK/plan, reports `REQUEST_DEADLINE` from both Providers, executes zero state commits, emits no events, and leaves no state residue. See `evidence/t013-cancel-deadline-20260825.md`.
  - **CPU identity/lifecycle matrix checkpoint (2026-08-25)**: the production runtime now accepts explicit decode-state byte/entry limits while preserving the 512 MiB/128-entry defaults. A production-path matrix rejects 27 independently mutated predecessor fields/component-order variants before the runner while retaining the committed predecessor. Three simultaneous lineages prove different generations and two attempts of the same request cannot cross-load state. Re-registering the production runner with a new Provider boot ID clears all three old lineages. A one-entry production runtime deterministically evicts the inactive LRU entry and rejects its later decode before runner execution. Event admission rejection, TOKEN_FEEDBACK publication failure, and activation publication failure all leave zero entries, pins, and candidates; failures after candidate staging roll back exactly once. Together with the direct Store byte/pin-pressure tests and the corrected end-to-end deadline case, this closes the registered CPU identity/lifecycle matrix. The next checkpoint closes step 8's CPU cache-effectiveness semantics; CUDA residency remains open. See `evidence/t013-cpu-lifecycle-20260825.md`.
  - **CPU cache-effectiveness checkpoint (2026-08-25)**: the production coordinator records one machine-readable observation per epoch: actual new input extent, logical prefix represented by committed state, avoided prefix work, and whether an exact predecessor was loaded. I01 now includes a same-artifact, same-prompt, deterministic no-predecessor full-prefix reference. Both branches emit `4,5,6,7,8,9,10,2`; cached input extents remain `[1,1,1,1,1,1,1,1]`, represented prefix extents are `[1,2,3,4,5,6,7,8]`, full-prefix control extents are `[1,2,3,4,5,6,7,8]`, and avoided prefix work totals 28 tokens. The first RED control exposed a test-only dangling reference to a decoded temporary tensor vector; holding the decoded bundle for the lifetime of the logits reference corrected the oracle without changing production cache behavior. This closes step 8's CPU semantic gate only. It does not prove lower CPU time, device residency, transfer avoidance, or real Qwen3.6-27B behavior. See `evidence/t013-cpu-cache-effectiveness-20260825.md`.
  - **Pass**: `NativeProviderSession::executeRoleAsync` automatically commits prefill state and supplies it to later role epochs without caller/harness `*_out -> *_in` assignment; each independently mutated field/component/predecessor epoch forces recompute/reject; pinned state cannot be evicted; cancel/deadline/terminal/attempt/boot changes clean up exactly; pre-failure state stays committed; failed candidate never replaces it; concurrent generations and attempts remain isolated; evidence distinguishes hit/miss/recompute/commit/eviction/cleanup; and a healthy decode consumes only the new token/current activation plus the exact complete local bundle. Every graph-declared non-state decode input is present and derived under the sealed adapter position policy, and CUDA state output/input mapping is one-to-one. The matched CPU control must report exact output parity, one prefill, incremental cached inputs, and positive prefix work avoided. A `useCache=false` full-context loop, a fake hit around full-prefix execution, silently zero-filled decode state, omitted position input, direct-store-only test, or unwired store fails T013. Real CUDA execution and zero-host-round-trip evidence remain T025 acceptance.

- [x] T014 [US2] Implement the one-plan multi-role generation loop, attach its stream state to the original deferred collaboration, and encode exact internal activation/token-feedback names for FR-021..FR-028, FR-031..FR-036, and FR-047 by extending `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/generation.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/placement.py`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp`, `examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py`, and `tests/integration-tests/ndnsf-di-core-flow.t.cpp`.
  - **Path correction (2026-08-27)**: the previously listed `tests/integration-tests/ndnsf-di-streamed-generation.t.cpp` does not exist in the repository. The Qwen one-plan loop is owned by `adapters/qwen/generation.py`; its native integration coverage is registered in `ndnsf-di-core-flow.t.cpp`.
  - **Fixed epoch order**: prefill epoch 0 -> token/cursor 1; decode epoch k consumes token k -> token/cursor k+1; End cursor token count+1.
  - **Logical-prefix transport rule (fixed 2026-08-25)**: implement and serialize `GenerationEpochLineageV1` inside the encrypted/authenticated DATA_V1 plaintext (or the equivalent process-local self-edge object). TOKEN_FEEDBACK advances one canonical model-token prefix. Every activation edge carries authenticated bounded prefix digest/count and position lineage for that epoch, not KV tensors and not an activation-derived substitute. Each selected role verifies the common lineage before its exact predecessor lookup; I02/I03/I15 assert equal prefix digest/count across roles despite distinct activations.
  - **Fixed ownership**: final role samples, publishes internal feedback and external event separately, and claims End/Response; no extra sampling role, per-token Request, or second plan.
  - **Native-harness checkpoint (2026-08-23)**: `OnePlanGenerationLoop` binds activation producers to the selected role-to-Provider map, emits separate exact activation and token-feedback names, and is attached to the deferred stream bridge without allocating a second Request. The production C++ request-scoped epoch coordinator drives the checked-in tiny stateful ONNX graphs through one-, two-, and four-Provider Request/ACK/plan/Selection paths when the harness explicitly supplies TOKEN_FEEDBACK, operation stride/capabilities, and generation config. I01-I03 and the I15 role-map permutation each pass three fresh processes with fixed seed `1750001`, the exact eight-token/EOS oracle, one Request ID, one terminal owner, and zero Provider failures. Terminal feedback is propagated as authenticated in-band activation control so every upstream coordinator exits without a ninth ORT step. Evidence is `evidence/t014-native-i01-20260823.md`, `evidence/t014-native-i02-20260823.md`, and `evidence/t014-native-i03-i15-20260823.md`.
  - **Production-wiring checkpoint (2026-08-24, corrected)**: the shared V3 automatic projection seals the exact terminal-to-first-role TOKEN_FEEDBACK dependency, nonzero streaming operation stride, bounded per-epoch GroupCapability operations, `GenerationExecutionContractV1`, and adapter-owned attention-KV/recurrent/convolution state I/O. The native plan parser validates that contract; `NativeProviderHandler` derives generation configuration from the authenticated Request/projection and invokes the request-scoped epoch coordinator; `DI_NativeProviderExecutable` binds capabilities to the authenticated execution-plan digest. These are source and focused-regression results; formal current-source qualification remains T020/T022.
  - **Native adapter boundary (2026-08-23)**: `RoleExecutionContext::StreamEventSink` is carried through the bounded Provider worker and exposed only to the terminal role. It lets a synchronous incremental ONNX adapter submit serialized token events without placing application events in dependency `TensorBundle`s; the sink does not grant cursor or terminal authority. The worker regression and direct native `OnnxRuntimeModelRunner::runStreamed(...)` tiny-ONNX test pass, including eight token events, EOS, state feedback, and final payload. This remains an adapter seam, not a formal I-case; the native tiny-ONNX multi-Provider loop must drive it before G2 registration.
  - **Cache side-effect correction (2026-08-23)**: provider-local exact-forward caching is bypassed for executions carrying the sink, because cached dependency outputs cannot replay externally visible token events. A focused regression proves two streamed executions invoke the runner twice and emit both events; this does not close the formal retry/replacement cases.
  - **Architecture closure (2026-08-23)**: the request-scoped epoch coordinator re-runs each selected local role with unary ONNX transitions, carries epoch-specific activation and token-feedback over authenticated DATA_V1, and leaves Event/End/Response authority with the terminal role. The shared harness records clean completion/failure for every Provider and rejects the former false-green condition where the final Response succeeded while an upstream drain failed.
  - **Terminal-owner correction (2026-08-23)**: the production Python Qwen pipeline now matches the same contract. Stage0 coordinates feedback and sends the bounded STOP record, but does not publish application token events or finish the stream. The selected terminal role verifies the recovery prefix, emits continuation events, validates STOP, and alone publishes End/Response. The provider-generation regression exercises attempt 2 and proves that a committed prefix is recomputed but not delivered twice.
  - **Required state-wiring correction (2026-08-25)**: formal I01-I03/I15 must reach the T013 Provider-owned store through the production epoch coordinator, record one prefill commit and the expected per-role decode hits/commits/cleanup, and prove zero decode-state bundle objects or bytes on activation/feedback edges. Existing case-body state feedback and direct `runStreamed()` loops are prerequisite adapter tests only.
  - **Automatic-state integration checkpoint (2026-08-25)**: current-source I01, I02, I03, and I15 enter through the production Request/ACK/plan/Selection/`NativeProviderHandler` path and use `NativeEpochCoordinator` plus the Provider-owned store. I01 produces the exact eight-token oracle with eight commits, seven predecessor hits, zero misses, and one terminal cleanup. I02 and I03 likewise report the expected per-role commit/hit/cleanup counts, and the multi-role regression rejects declared decode-state tensors on NDN dependency edges. I15 passes with four ACKs, four Provider completions, a genuinely permuted role-to-Provider map, eight events, and zero Provider failures. The I01 terminal-to-first TOKEN_FEEDBACK self-edge uses bounded process-local `LocalDependencyIo`; cross-Provider edges carry authenticated DATA_V1 plus bounded `GenerationEpochLineageV1`, never KV/recurrent/convolution tensors. The complete focused I01-I13/I15 matrix now passes in one process. I09 injects one content mutation on the real Provider-to-User bridge while suppressing the untampered SVS duplicate; the User rejects cursor 1 before any callback, both selected Provider coordinators terminate, and the unselected Provider executes zero times. The repeated multi-process qualification is recorded by T020/T022.
  - **Required coordinator contract (2026-08-23)**: implement the bounded request-scoped object specified in `contracts/native-epoch-coordinator-v1.md`. It must use one unary `NativeModelRunner::run()` transition per role and epoch, pre-authorize distinct DATA_V1 operation indexes through the signed token bound, include epoch in exact names/manifests, and fail closed on missing/reused operations or wrong role/provider lineage. `runStreamed()` remains a one-Provider compatibility seam and cannot be used to register I02/I03/I15.
  - **Coordinator runner-boundary correction (2026-08-27)**: a lineage-bearing coordinator epoch now always invokes one unary `run()` transition. The Provider worker no longer starts a complete `runStreamed()` loop for each epoch, and the ONNX adapter rejects direct `runStreamed()` calls carrying authenticated coordinator lineage. This prevents duplicate token events and stale logical-prefix/position reuse; standalone streamed calls without coordinator lineage remain compatible.
  - **Fail-closed stream boundary correction (2026-08-27)**: `AutomaticStreamingHandle` now checks optional event/terminal `requestId` and `generationId` bindings, turns malformed lineage and application callback exceptions into terminal errors, and `OnePlanGenerationLoop` rejects incomplete activation edges or role-to-Provider maps instead of silently skipping them. This was a focused implementation correction; the current Python-user-to-native-Provider workload and source-sealed G2 matrix are now covered by the T020 closure. See `evidence/implementation-corrections-20260827.md`.
  - **Implementation closure (2026-08-27)**: after fixing the tiny-ONNX workload to pass and verify the campaign Request ID, a real four-Provider M01 run used `/spec175-M01-1750001` unchanged across automatic Request publication, the streamed handle, all eight token events, and the terminal response. Four Provider spans closed with no open span. Together with the production coordinator/store regressions, this closes T014's implementation boundary without claiming the repeated G2/G3, Qwen3.6, CUDA, SIF, or Tiger gates. See `evidence/implementation-closure-request-id-20260827.md`.
  - **Registration checkpoint (2026-08-24, corrected)**: I01-I15 are registered on the shared native harness. I12 exercises an opt-in two-request replacement with a spare terminal Provider, attemptEpoch=2, committed-prefix replay, and End/Response completion. I13 detaches the live terminal Provider transport at the publication boundary and proves default-disabled failure with one Request and no replacement. Their corrected focused repetitions passed for the pre-cache-correction source; the complete matrix remains historical until T014 removes harness-managed state feedback and the reopened T020 reseals G2.
  - **Pass**: I01-I03 exact oracles and I15 capability permutation pass with one unchanged request ID across Request, ACK closure, plan, Selection, events, prefill, and terminal Response; I15 changes the role mapping instead of using Provider identities as a fixed map. The one-role feedback self-edge is process-local and single-consumption; every cross-Provider edge is authenticated DATA_V1; neither path transports decode-state tensors.

- [x] T015 [US2] Add model/task-first `AutomaticPlanningCoordinator.request_streaming(...)` with no Provider list and convert the LLM user/workload path from per-token/final-only evidence to its real streamed handle for FR-002..FR-006, FR-017, FR-021, FR-047..FR-048, and FR-055..FR-056 in `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`, `pythonWrapper/ndnsf/service.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1_evidence.py`, `scripts/build_spec175_workload.py`, `packaging/ndnsf-di-container/jobs/spec175/workload.json`, `tests/python/test_spec175_workload.py`, and `tests/python/test_spec175_evidence.py`. There is no separate `llm_pipeline/workload.py`; the content-addressed builder plus frozen JSON are the single workload authority and the application user consumes that artifact rather than maintaining a second copy.
  - **Frozen workload artifact**: create and validate `packaging/ndnsf-di-container/jobs/spec175/workload.json` before G3 with the exact P1/P2 messages in `contracts/validation-contract.md`, their prompt and derived token IDs/counts, tokenizer/chat-template digests, `thinkingMode=disabled`, Greedy, `maxGeneratedTokens=64`, `maxEvents=65`, and `requestDeadlineMs=120000`. The file is embedded unchanged in the candidate SIF; G5 adds an external oracle and may not rewrite the inputs.
  - **Fixed evidence**: TTFT, every inter-token interval, per-component spans, exact tokens/text/transcript, queues, retries, decode-state hit/miss/recompute, resource/fallback, End, and final Response; no plaintext prompt/answer/logits/decode-state tensors in operational logs.
  - **Partial checkpoint (2026-08-24, corrected; timing gap reduced 2026-08-27)**: model/task-first Coordinator and `APPClient.request_streaming(...)` forward one `TOKEN_STREAMING` request with the existing collaboration ID and require all three callbacks; the example user consumes the streamed handle instead of expanding per-token Requests. The production user requests `useCache=true`/`outputMode=TOKEN_STREAMING`, automatic V3 seals the stateful generation contract, and the legacy Python Provider rejects an incomplete stateful streaming path. `AutomaticStreamingHandle.timing_summary` now records real monotonic callback timestamps and derives metadata-only TTFT/ITL/terminal timing without exposing token or prompt contents. The immutable P1/P2 workload and bounded `ndnsf-di-spec175-stream-evidence-v1` validator exist. Per-role lifecycle spans and a real Python-user-to-native-Provider workload process proof remain open.
  - **Per-role span checkpoint (2026-08-27)**: the Spec175 MiniNDN launcher now enables runtime timing for its registered cases and emits a strict `ndnsf-di-spec175-provider-timing-v1` report. It pairs repeated Provider handler epochs by `(session, role)`, requires all configured roles, rejects unmatched/negative spans, and binds the report into `spec175-case-result.json`. One current M11 run produced eight matched spans across four roles; this closes the timing-report wiring gap but remains one execution checkpoint, not repeated T015/T020 qualification or a Qwen3.6 performance result. See `evidence/t015-provider-timing-20260827.md`.
  - **Real automatic-stream checkpoint (2026-08-27)**: current-source M01
    completed through the real four-Provider MiniNDN Request/ACK/plan/Selection
    path using the Python model/task-first `request_streaming` handle.  It
    delivered eight ordered token events, one terminal response, four matched
    Provider spans, and zero user/Provider failures.  The run also caught and
    fixed a contract-label defect: the tiny streaming context and final
    response now report `TOKEN_STREAMING` instead of `FULL`.  This is current
    host/tiny-ONNX evidence only; the frozen Qwen workload, repeated G2 matrix,
    CUDA, SIF, and Tiger qualification remain open.  See
    `evidence/m01-real-stream-mode-20260827.md`.
  - **Implementation closure (2026-08-27)**: the immutable P1/P2 workload, no-Provider-list API, callback timing, strict per-role spans, and actual automatic native-Provider path now have focused regressions. The post-fix M01 run additionally verifies that the public/campaign Request ID is the Request ID used by the stream, rather than merely a launcher label. This closes the workload/API implementation task; repeated qualification and performance measurement remain T020/T022/T027. See `evidence/implementation-closure-request-id-20260827.md`.
  - **ONNX/native selection correction (2026-08-24)**: `Experiments/NDNSF_DI_LlmPipeline_Minindn.py` now admits `qwen-onnx` and `qwen-onnx-cpu-native` for Selection Dataflow V2/V3 and publishes `onnxruntime-cuda` or `onnxruntime-cpu` residency metadata. The prior path silently advertised a Transformers backend for an ONNX/native campaign; the source regression now rejects that drift. This closes a capability-description bug only; it does not replace the required fresh Python-user-to-native-Provider process proof.
  - **ONNX admission correction (2026-08-24)**: `validate_real_model_binding(...)` now maps both ONNX launcher runtimes to the canonical stage-manifest value `onnxruntime`; the previous direct comparison to `qwen-onnx` would reject a valid exported manifest before MiniNDN startup. The mapping is covered for both `qwen-onnx` and `qwen-onnx-cpu-native` in `test_spec168_tiny_qwen_fixture.py`; see `evidence/t015-onnx-selection-backend-20260824.md`.
  - **Pass**: the user observes token deltas before final completion, `result()` returns the complete oracle payload, the stream handle request ID equals the existing `begin_collaboration`/`commit_plan` request ID, traces contain no second Request or terminal owner, and evidence rejects missing/extra/reordered/misattributed spans.

**Checkpoint**: The formal native NDNSF-DI I01-I03/I15 healthy harness paths are
registered and process-qualified through the same Request/ACK/plan/Selection
path with one request, one prefill, exact activation/feedback lineage, clean
termination of every selected Provider, and one End/Response owner. User Story
2 remains open because the real Python model/task-first workload and its
workload-process proof are still required for T014/T015, T013's exact Qwen3.6
stateful/CUDA qualification is still open, and T020 still needs a fresh
source-bound repeated matrix. The current one-repeat G2 is a diagnostic
checkpoint, not formal promotion evidence; the tiny CPU cases do not qualify
Tiger performance.

---

## Phase 5: User Story 3 - Recover Without Mixing Attempts or Duplicating Output (Priority: P2)

**Goal**: Close deterministic fault, cancellation, security, capacity, and
explicit replacement semantics at integration fidelity.

**Independent Test**: I04-I13 inject one registered fault each and produce the
exact success/failure/logical-transcript verdict in fresh processes.

- [x] T016 [US3] Close event reorder, duplicate, one-loss retry, permanent-gap, retention expiry, and End-before-gap cases for FR-013..FR-019, FR-038, FR-042, and SC-001..SC-005 by adding deterministic transport interception to the existing integration fixture and cases I04-I07 in `tests/integration-tests/ndnsf-integration-fixture.hpp`, `tests/integration-tests/ndnsf-integration-fixture.cpp`, `tests/integration-tests/invocation-stream-flow.t.cpp`, and `scripts/run_spec175_integration_gate.py`.
  - **Historical partial checkpoint (2026-08-24, corrected)**: the production two-role tiny-ONNX harness registers I04 reorder, I05 duplicate, I06 exact one-loss IMS retry, and all three I07 permanent-gap subscenarios. The earlier I05 race was not reproduced by the current three-repeat I04--I13 run; all current runs passed. The full source-sealed matrix remains a T020/T022 gate.
  - **Pass**: recoverable cases deliver exact 1..N once; permanent gap fails explicitly; no case skips a cursor or returns partial success.

- [x] T017 [US3] Close signature, AEAD, name, binding, token, policy, replay, stale-attempt/generation, event-key commitment/grant, unselected/nonfinal Provider unwrap, and End/Response tamper cases for FR-034, FR-038..FR-041, and SC-005 by implementing I09 plus exhaustive mutation cases in `tests/integration-tests/invocation-stream-flow.t.cpp`, `tests/unit-tests/invocation-stream-message.t.cpp`, and the existing token/ABE/security regressions.
  - **Historical partial checkpoint (2026-08-23, refreshed)**: formal two-role I09 now adds an unselected ACK Provider, proves only the selected final role receives stream context/key authority, proves the unselected Provider executes zero times, and rejects tampered ciphertext before callback. Canonical wire/name/binding unit negatives pass; the current 590-case native unit gate and three-repeat I09 run provide the focused mutation closure. Source-sealed G0--G2 remains separate.
  - **Pass**: every invalid event fails before reorder insertion/callback; a valid event under the wrong name/key/attempt cannot be adopted; only the final-role Selection projection can unwrap the Normal event key; no debug bypass appears.

- [x] T018 [US3] Close callback exception, slow consumer, capacity-1 backpressure, cancellation-after-event-3, deadline, and concurrent cancel/publication races for FR-008, FR-020, FR-036, FR-042..FR-044, and SC-005 by implementing I08/I10/I11 in `ndn-service-framework/InvocationStream.cpp`, `tests/integration-tests/invocation-stream-flow.t.cpp`, and `tests/python/test_streamed_invocation_api.py`.
  - **Historical partial checkpoint (2026-08-23, refreshed)**: formal two-role I08 cancels after exactly three callbacks, I10 contains a third-callback exception as `ApplicationCallbackFailed`, and I11 completes all eight events with Provider queue high-water 1, callback queue high-water 1, and callback concurrency 1. The current stream integration and Python API suites cover the deadline/publication-race boundary; source-sealed manifest generation remains T020.
  - **Pass**: no accepted event drops, capacities remain bounded, generation pauses, cancel fences all later application delivery, and terminal/callback occurs at most once.

- [x] T019 [US3] Implement the explicit one-replacement generation contract for FR-031..FR-038 and SC-005. First extend `StreamRequestOptions` with signed attempt epoch 1/2 and propagate it through grant/event bindings in `ndn-service-framework/NDNSFMessages.hpp`, `ndn-service-framework/NDNSFMessages.cpp`, `ndn-service-framework/InvocationStream.cpp`, `ndn-service-framework/ServiceUser.cpp`, and `ndn-service-framework/ServiceProvider.cpp`; Targeted replacement rejects. Then make `AutomaticPlanningCoordinator` own one public logical handle but create at most one internal Normal recovery Request with a fresh request ID, one-time tokens, ACK closure, plan, Selection, event key, and stream epoch in `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`. Seal the original prompt plus committed token IDs/count/digest into the encrypted recovery Request/plan, exclude the failed Provider, recompute prefill once, emit continuation only, fence the old attempt, and implement default-disabled I13 plus opt-in I12 across `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp`, `tests/unit-tests/invocation-stream-message.t.cpp`, `tests/unit-tests/di-qwen-generation-session.t.cpp`, `tests/python/test_spec175_evidence.py`, and `tests/integration-tests/ndnsf-di-core-flow.t.cpp`.
  - **Implementation checkpoint (2026-08-24, corrected)**: signed attempt 1/2 reaches the event/grant/name/native coordinator paths; `AutomaticStreamingHandle` preserves one public generation while `request_streaming(...)` creates at most one new Normal Request with a fresh ID and deadline-bounded ACK/plan/Selection path. Replacement requires a recoverable error carrying an absolute authenticated Provider identity, excludes that Provider, embeds `GenerationRecoveryV1`, and delivers only events whose token epoch extends the committed transcript. `request_contract_digest` seals the exact request/recovery bytes into `PlacementPlanCoreV3` and every Provider projection, and Python/native Providers reject a mismatch. Focused Python orchestration and the corrected I12/I13 runs passed; the previously observed I05 transport failure and G2 matrix are historical diagnostic evidence until the corrected same-source T020 rerun.
  - **I12/I13 evidence (2026-08-23)**: the rebuilt native Core process registers `Spec175NativeTinyOnnxI12ProviderUnavailableAfterEvent3WithReplacement` and passes in all three fresh-process healthy repetitions with two Request publications, four Provider epoch-coordinator completions across the two attempts, one spare-provider execution, post-reselection delivery, and a complete End/Response carrying the four-token tiny oracle. I13 passes with exactly one Request publication, one live transport detachment at cursor 4, no completion, an explicit fetch/timeout failure, and no second attempt. See `evidence/t019-i12-replacement-20260823.md`, `evidence/g2-live-i13-20260823.md`, and `evidence/t019-i13-default-disabled-20260823.md`.
  - **Pass**: disabled mode makes exactly one Request and never re-executes; opt-in mode makes exactly two Requests/ACK closures/plans/Selection sets, uses attempt 1 then 2 under one stable generation ID, never reuses a ProviderToken or failed Provider, publishes only the continuation after the committed prefix, and yields the exact combined transcript. Second replacement, mismatched prefix, stale output, Targeted replacement, or token/plan reuse fails; no live decode-state migration is added.

**Historical checkpoint (pre-Face-fix)**: G2 was closed for the then-current
source-bound host subject. The complete integration binary passed all 68 cases,
the registered I01-I15 matrix passed all 27 required processes, and that
historical manifest recorded binary SHA-256
`184d0676d441a8eab654ee45a057cd28d82e9e3816fe069c54c37f9124136989`. The later
Provider Face-event-loop publication change invalidated those hashes for the
current subject; the current replacement manifests are recorded under T020.

---

## Current Qualification Task Definitions (Priority: P3)

T020--T028 preserve their published task/evidence IDs. The implementation
queue is closed, so T020 is now the current execution phase. T021 is already
closed as builder/preflight source implementation, and T024's source/interface
and mutation boundary is also closed. T022 and every later gate remain ordered
behind their explicit predecessor.

**Goal**: Make local gates exercise the same signed model/runtime/network path,
then promote one immutable SIF to bounded Tiger functional and performance runs.

**Independent Test**: A failed lower gate prevents submission; a passing
candidate retains one hash through G3-G7 and emits the registered verdict.

- [x] T020 [US4] After T005--T019, T024, and T029--T033 close their implementation and focused-test acceptance, freeze one current source/workload identity and make formal G0 plus G1/G2 manifest closure executable and non-skippable for FR-049..FR-050, FR-055, FR-057, FR-070..FR-071, SC-004, SC-006, and SC-013..SC-015 by rebuilding the host diagnostic framework and Python extension from that same source/toolchain identity, recording their hashes and `ldd`, rerunning `scripts/spec175_contract_gate.py` against the implemented source, running `scripts/run_spec175_python_gate.py` for the bounded Spec175/shared-contract Python subject, completing `scripts/run_spec175_integration_gate.py` for I01--I20, and recording suite/case/process/skip/failure inventories under `results/spec175/g0/`, `results/spec175/g1/`, and `results/spec175/g2/`. Extend G0/G1/G2 to reject an unwired Provider state store, formal cases with harness-managed state feedback, missing/untrusted static identity authority, activation-derived prefix identity, inconsistent multi-role logical-prefix/count/position lineage, missing state/predecessor-epoch or lifecycle counters, a runner call after rejected predecessor/checkpoint/readiness lookup, direct new-request reuse of request-local state, missing terminal-prefix finalization/promotion evidence, full-prefix execution disguised as delta prefill, partial role checkpoint commit, public Provider/state-location fields, or decode-state bytes on NDN dependency edges. Both G1 and G2 require and record the same G0 source-seal digest; G2 also records the exact integration-binary SHA-256. The full historical `tests/python` tree remains diagnostic-only and does not trigger a SIF rebuild unless a failure touches a Spec175/shared-contract owner. T020 MUST NOT run as a repeated debugging loop while any implementation task is still open.
  - **Audit reopening (2026-08-27)**: the post-fix G1/G2 manifests pass, but the 42/42 G3 manifest still binds the earlier pre-fix source seal. The repository-owned gate and preflight tests were also absent from the pushed checkpoint. T020 is therefore open until those files are versioned and G0/G1/G2 are regenerated from one new immutable source seal.
  - **Post-audit closure (2026-08-27)**: revision `fe0b09fcc5230dd7feb3d35b1d140b40f434da14` was rebuilt with the system toolchain and explicit NDN-SVS tree. G0 passes with zero blockers and zero dirty source inputs; G1 passes native unit 600/600 plus 219/219 Python checks; G2 passes 38/38 registered I01--I20 results. G1 and G2 bind source seal `sha256:eda21b2b8b05e2961503c8bd07fecafa1d9af0cdd1e72d6d92778c905535a771`. See `evidence/t020-post-audit-gates-20260827.md`. T022 remains open because its older 42/42 matrix does not bind this seal.
  - **Reopened by contract correction (2026-08-25, extended 2026-08-26)**: the earlier 131-test/27-process manifests predate the automatic Provider-cache, formal no-manual-feedback, and I16--I20 conversation checks. They remain historical positive evidence but cannot close the updated T020 until T014 and T029--T032 are implemented and the same-source G0-G2 gates pass again.
  - **Historical native checkpoint (2026-08-24)**: the pre-fix candidate host build passed the recorded native unit/integration inventories, and the D2b/D2h sequence plus DATA_V1 cases passed their stated development repetitions. It is retained for diagnosis only; the current-source acceptance is recorded by the 2026-08-25 manifests below.
  - **Historical formal closure checkpoint (2026-08-24)**: G0 reported `PASS` with zero blockers; bounded G1 reported 121 passed, zero failed, and zero skipped; the complete integration binary reported 68/68; and G2 reported 27/27 registered processes with no missing case. Those results are retained as development evidence only because their source seal predates the current fix. Historical full-project Python drift remains diagnostic and must not trigger a SIF rebuild unless it touches a Spec175/shared-contract owner.
  - **Historical source-seal checkpoint (2026-08-24)**: `results/spec175/g0/source-seal-current.json` binds the pre-event-loop-fix subject and is retained as historical evidence. The latest pre-cache-correction seal is `results/spec175/g0/source-seal-final-20260825.json`; it is not the next promotion seal.
  - **Latest pre-correction closure checkpoint (2026-08-25)**: G0 reported `PASS` with zero blockers; G1 reported 131 passed, zero failed, and zero skipped with native/Python import and loader checks; G2 reported 27/27 registered processes with no missing case and the exact integration-binary SHA-256. These manifests share `results/spec175/g0/source-seal-final-20260825.json` and remain historical. The current closure below supersedes them.
  - **Current closure (2026-08-27)**: after the implementation and documentation corrections, one content-bound current-source candidate passed G0 with zero blockers, G1 with 219 passed and zero failed/skipped, and G2 with 38/38 registered results across I01--I20 and no missing case. Feature progress/evidence documents are bound by G0 document digests but excluded from the compiled-source seal, so recording this status does not invalidate the tested binaries. These machine manifests are the T022 input. No G3, SIF, CUDA, or Tiger claim follows from T020 alone.
  - **Pass**: G0, the bounded G1 manifest, the complete native unit gate, and G2 manifest status `PASS`, host extension/framework hashes and loader closure agree, zero missing symbol/contract drift or mandatory skip, three fresh healthy repetitions, the named unary/Targeted/security/StreamFacade/Spec174 regressions, and no leaked process/resource.

- [x] T021 [US4] Extend the current local SIF builder and native preflight for FR-030, FR-053, FR-055, FR-057, and SC-007..SC-008 in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh`, `packaging/ndnsf-di-container/bin/ndnsf-di-spec175-preflight`, `packaging/ndnsf-di-container/bin/spec175-host-substrate-preflight`, `packaging/ndnsf-di-container/lib/spec170_sif_build_boundary.py`, and `tests/python/test_spec175_sif_preflight.py` without adding a Docker/remote-materialization alternative. This task is intentionally deferred until the host/CPU MiniNDN gate passes: before G3, only static/unit/mutation checks of this builder are allowed, and no SIF may be created or preflighted. The builder MUST require the host-gate manifest and refuse to build unless its status is `PASS`, its source/workload digests match the requested subject, and its M01-M14 inventory is complete. The final SIF MUST embed the exact sealed framework/Python/adapter/workload runtime source; G4-G7 MUST reject runtime source or replacement-module overlays. The final definition MUST explicitly remove `torch*`, `transformers*`, and `functorch*`; the in-SIF runtime probe remains the authority and rejects any residual deployment module. The SIF MUST contain NFD, NDNSF, the production Controller/repository/Provider/User entry points, CPython bindings, NDNSF-DI conversation-state runtime, ONNX Runtime, and required NDN management/security commands. It MUST NOT be required to contain MiniNDN, Mininet, Open vSwitch, NLSR, `mnexec`, or host network tools. A separate host-substrate preflight owns those dependencies.
  - **Superseded candidate checkpoint (2026-08-25)**: `spec175-final-candidate-20260825b` and its SIF (`sha256:932672c8aef10800a854c859b4f3b05c10184f5381323956829570d99d560c17`) were built before the current two-layer/runner changes and are retained only as historical diagnostics. The replacement build was intentionally stopped before compilation completed: strict source-identity checking found that the recorded 30/30 G3 manifest contains an older `Experiments/NDNSF_DI_StreamedGeneration_Minindn.py` hash. No current SIF is `READY_FOR_G4_REPLAY` or promotable. The replacement sequence is: rerun G3 with the current source, freeze its seal, build once, run both preflights, then run the host-orchestrated G4 replay.
  - **Partial checkpoint (2026-08-23, refreshed)**: the preflight now runs after the candidate SIF is built, imports the embedded CPython-3.10 `_ndnsf` extension, checks the in-SIF `ldd` closure for both the extension and native Provider, requires CUDAExecutionProvider, and rejects `torch`, `transformers`, or `functorch` residue. The existing local candidate passed ABI/ldd/CUDA checks but was correctly rejected for embedded `functorch`; see `evidence/t021-runtime-probe-20260823.md`. A replacement SIF and G4-G7 evidence remain open.
  - **Deferred-candidate checkpoint (2026-08-24)**: r25/r26 were rejected at the container-native link boundary because direct Provider source groups omitted `InvocationStream.cpp` and then `NativeEpochCoordinator.cpp`; those source omissions are fixed. r27 passed its older preflight but is superseded. r28 was rebuilt from the current sealed source archive inside Apptainer and passed the immutable SIF preflight, but is superseded by r30. r29 was rejected because its SIF label retained an old source-seal digest. r30 rebuilt from the sealed source archive, passed the immutable SIF preflight, and independently verified the source-seal label, CPython-3.10 import, ldd, CUDA-EP, and forbidden-runtime checks (`sha256:9b5c08368ff56a65caf10358b6abc5b5df6d26e8fe7b8c942243c322c98b6baf`). It is diagnostic only until G3 host/CPU MiniNDN passes; no SIF is promoted from this checkpoint. See `evidence/t021-runtime-r30-20260824.md`.
  - **Fixed SIF checks**: source/SIF/native hashes; Python 3.10/SOABI/import; RPATH/`ldd`; Boost 1.71; ndn-cxx/SVS Experimental/NFD/NDNSF; CPU/CUDA ORT; real workload `--help`; bundle cwd/artifacts; writable paths; secret scan; `tokenizers` present; deployed PyTorch/Transformers absent. **Fixed host checks**: MiniNDN, Mininet, Open vSwitch, NLSR, `mnexec`, `ip`, topology input, namespace privileges, matching Apptainer 1.5.3, and the replay driver. The two lists MUST remain separate.
  - **Updated two-layer preflight implementation (2026-08-25)**: `ndnsf-di-spec175-preflight` now checks only SIF-owned runtime commands and ABI/ORT state; `spec175-host-substrate-preflight` checks the host-only MiniNDN/Mininet/OVS/NLSR namespace layer. If either gate requests the other layer's binaries, classify it as `WRONG_LAYER_PREFLIGHT` and stop.
  - **Verification checkpoint (2026-08-25)**: focused source/preflight/host-gate tests report 34 passed; the existing historical candidate passes the corrected exact-SIF runtime probe. The host-substrate probe correctly fails closed for an unprivileged invocation with `HOST_NAMESPACE_PRIVILEGE_MISSING`. This closes the T021 builder/preflight implementation boundary but does not close G4 because the future post-G3 candidate build record and expanded exact 42-case replay remain pending; see `evidence/t021-two-layer-preflight-20260825.md`.
  - **Current identity checkpoint (2026-08-25)**: the current host runner passes one fresh M01 with the frozen 4-Provider/tiny-ONNX/disabled-admission configuration (`results/spec175/g3/current-M01-r1-20260825`, user return code 0). The focused source/preflight/build-boundary suite now reports 44 passed. This is a smoke confirmation only; the old 30/30 manifest remains invalid for the changed source and must be regenerated before any SIF build.
  - **Superseded local candidate checkpoint (2026-08-25)**: candidate `spec175-final-candidate-20260825l` was built once with the sealed source and `host-minindn-manifest-current-20260825l.json`. Its build record and layer-specific preflights are `PASS`, but the exact SIF replay is 30/30 `FAIL` at NFD readiness; a follow-up launcher smoke reached certificate bootstrap and failed decrypt. The host runner then changed to use Apptainer `--home`, so candidate l is invalidated. T023 must regenerate G0-G3, build a new SIF once, and prove the bounded launcher smoke before replaying G4.
  - **Partial checkpoint (2026-08-23)**: `ndnsf-di-spec175-preflight` is executable, validates the frozen workload/model/tokenizer identity, requires the workload in the source seal, rejects compiled/runtime overlay names, and is invoked by `build-local-sif.sh`; its PASS record is embedded in the build record. Unit and local-builder regressions pass. Candidate-SIF ABI/loader/toolchain/runtime checks and one promotable SIF remain open.
  - **Pass**: mutation tests reject stale host `_ndnsf.so`, wrong cwd, missing asset/library/provider, ABI/version/hash drift, fallback, secret, and runtime Transformers; the in-SIF manifest does not require MiniNDN/Mininet/OVS/NLSR; and the separate host-substrate preflight owns those checks. The implemented builder and both preflights are ready to consume a future post-G3 seal in T023; T021 neither builds nor marks a current candidate `PROMOTABLE`.

- [ ] T022 [US4] Execute and close the host/CPU real MiniNDN star topology and M01-M14 cases for FR-051..FR-052, FR-070..FR-071, SC-003..SC-006, and SC-013..SC-015 in `Experiments/NDNSF_DI_StreamedGeneration_Minindn.py`, `tests/python/test_spec175_real_minindn_gate.py`, and the existing MiniNDN process/security helpers, using one controller, repository, user, four separate Providers, router, 100 Mbit/s/10 ms links, real NFD/SVS/ABE, disabled admission, and the frozen tiny ONNX bundle. T033 owns the M11--M14 harness implementation and focused contract tests; this gate runs the complete matrix against the corrected same-source native/Python build only after T020 passes and before any SIF build. Its sealed manifest is the input to the later exact-SIF replay rather than a substitute for it.
  - **Reopened evidence boundary (2026-08-25, extended 2026-08-26; superseded by the current closure below)**: all existing M01--M10 manifests predate the production Provider-cache wiring and the M11--M14 conversation contract. They remain historical transport/protocol evidence. The required fresh 42/42 matrix and Provider lifecycle counters are now recorded by the current T020/T022 subject; only the exact-SIF replay remains before T023 can close.
  - **Historical G3 closure invalidated by later source fixes (2026-08-27)**: a fresh output root ran every M01--M14 case three times with the then-current T020 source seal, fixed workload seed `1750001`, four Providers, and admission control disabled. All 42 processes returned `0` with case-result status `PASS`; the strict validator accepted 42/42 entries, including the M11--M14 conversation evidence and complete Provider timing records. The durable manifest is `results/spec175/g3/spec175-g3-current-20260827.json`; the source seal is `results/spec175/g0/source-seal-current-20260827.json`. Subsequent ONNX-copy/cancellation and qualification-source corrections changed the subject, so this matrix remains valid historical regression evidence but no longer closes T022 or authorizes a SIF build. After T020 reseals, rerun the unchanged 42-case matrix once from a fresh output root.
  - **Layout checkpoint (2026-08-24)**: the production MiniNDN runner accepts a controlled 2--4 stage layout; four-stage mode assigns `ucla`, `arizona`, `wustl`, and `neu` to distinct Provider and repository identities and rejects unsupported sizes before MiniNDN startup. The frozen Spec175 path now drives the checked-in tiny-ONNX fixture through the real repository-backed four-Provider topology. The layout and preparation contracts are covered by `tests/python/test_spec175_minindn_layout.py` and `tests/python/test_spec175_repo_bootstrap.py`; final execution acceptance now requires the expanded 42/42 matrix below.
  - **Implemented host-gate boundary (2026-08-24)**: G3 is a separate, cheap qualification subject that consumes `tests/fixtures/spec175/tiny-causal-lm-v1` and the current host build. It does not require a Qwen3.6 stage manifest, a 27B artifact, a SIF, or a Tiger allocation. Dry-run and topology assertions remain preparation evidence only and cannot substitute for the real M01-M14 executions.
  - **Historical matrix checkpoint (2026-08-25)**: the prior source passed all 30 required fresh host/CPU processes: M01--M06 in `results/spec175/g3/replay1-M01..M06-*-20260825` and M07--M10 in `results/spec175/g3/replay2-M07..M10-*-20260825`, three repetitions per case. Every result has four Providers, real NFD/SVS/ABE, tiny ONNX runtime, disabled admission, user return code 0, and clean MiniNDN teardown. M10 records the required rotated Provider-role map `[1,2,3,0]`; M07 records the deferred-cancel terminal path; M08/M09 record the deadline/provider-failure terminal paths. Because the runner and preflight source changed afterward, this manifest is historical evidence and cannot unlock a new SIF; the earlier cursor-1 and setup-only runs remain retained as excluded diagnostics and are not pooled.
  - **Preparation correction**: repository publication now receives an explicit 5000 ms ACK-collection timeout through `spec175_repo_bootstrap.py` and the MiniNDN runner. This is a bounded control-plane preparation timeout only; the registered streamed request/ACK/event deadlines and retry budgets are unchanged. A zero-candidate repository closure remains a setup failure, never an M01 pass or fail.
  - **Preparation rule retained**: repository publication uses an explicit 5000 ms ACK-collection timeout for preparation only; registered streamed request/ACK/event deadlines and retry budgets remain unchanged. The one failed M07 preparation attempt is recorded as setup-only and excluded from the matrix.
  - **Execution packets (strict order; each packet must leave an evidence record)**:
    0. **T022-P — qualify preparation**: verify the explicit 5000 ms repository ACK deadline, permission/handler readiness, committed artifact registration, and all four Provider artifact fetches. A zero-candidate or pre-Selection setup failure is excluded from M01 and must be corrected or rerun before pooling results.
    1. **T022-A — localize the fetch race**: run one fresh healthy M01 with exact Interest-arrival, IMS hit/miss, pending-Interest, and `face.put` tracing only if a clean post-diagnostic M01 reproduction still fails. Do not change retry/lifetime/deadline values. If setup fails before Selection, record it separately as a preparation failure and rerun; it is not an M01 result.
    2. **T022-B — close healthy M01**: after the owning fix, obtain three independent clean M01 processes with exact cursors, one End/Response, packet lineage, and leak-free teardown. A 2/3 set is diagnostic only. **Completed 2026-08-24** by `t022b-clean-20260824j/k/l`.
    3. **T022-C — run fault matrix**: completed M02--M10 with three clean processes each, one registered fault dimension per case, with no parameter changes or pooled configurations.
    4. **T022-D — seal G3**: the historical seal completed with `results/spec175/g3/host-minindn-manifest-final-20260825.json`. The current selected manifest is `results/spec175/g3/host-minindn-manifest-current-20260825n.json`, but it cannot close promotion while an additional same-subject signal exit remains unclassified.
    5. **T022-E — classify every current-subject native exit**: retain the historical failed `replay4-M09-r3-20260825n` process and passing isolated rerun as regression inputs. After the implementation queue closes, run the fresh M09 triplet from the frozen T020 subject. If the signal reproduces, reduce its owner, add a focused lifetime/teardown regression, and restart from T020; if it does not reproduce, preserve both the historical failure and the complete fresh triplet in the manifest. Retrying until three selected PASS rows exist is not acceptance.
  - **Visible T022 sub-progress (not independent acceptance gates)**:
    - [x] Freeze and validate the eight-node star topology, four distinct Provider identities, tiny-ONNX four-stage assignment, disabled admission, and `NDNSF_SELECTION_TARGETED_PREFETCH=0` baseline.
    - [x] Publish all four stage artifacts through the real DistributedRepo path and make each Provider fetch its exact assigned artifact; repair manifest indexing, receipt scoping, and permission-readiness regressions with focused tests.
    - [x] Reach the real four-ACK, ACK-driven placement, and four-Selection boundary in one frozen-topology process.
    - [x] Repair and regress the post-Selection public event fetch race: publication now commits on the Face loop; the current 30/30 matrix has no cursor-1 loss. Historical c/e processes remain retained negative evidence.
    - [x] Pass M01 in three fresh post-diagnostic processes with exact tokens, final Response, and clean teardown (`t022b-clean-20260824j/k/l`; historical c/e failures retained separately).
    - [x] Pass M02-M10 in 27 fresh processes with one registered fault dimension per case.
    - [x] After the implementation freeze, run and classify the complete fresh M01--M14 subject, including M09, and seal a promotion-eligible 42/42 G3 manifest with no hidden same-subject crash.
  - **Fixed matrix**: three clean processes for each M01-M14 (42/42); seed 1750001, fault seed 1750002; one fault dimension per fault case; M10 permutes signed Provider capacity/cache residency and requires a different ACK-driven role map than M04; M11--M14 are frozen by T033.
  - **Pass**: exact tokens/final result, Request-to-Response packet lineage, bounded queues/retries, no host NFD/fake transport, and clean process-tree teardown under the host-build manifest digest.

- [ ] T023 [US4] After T022 passes, execute and close G0-G4 for one exact candidate: freeze the host manifest, build the SIF once, run the separate host-substrate and in-SIF runtime preflights, and replay M01-M14 with the host MiniNDN harness creating namespaces/topology while every NFD, Controller, repository, Provider, User, Python binding, and ONNX Runtime process is launched from the exact SIF through `apptainer exec --cleanenv`. Implement this boundary in the tracked host driver `packaging/ndnsf-di-container/jobs/spec175/replay-exact-sif.py` and focused launcher-contract tests; do not copy MiniNDN/Mininet/OVS/NLSR into the SIF. Bind the host harness/topology/tiny-fixture digests and SIF digest in the G4 manifest, diagnose any failure at its host-substrate or SIF-runtime owner, and write immutable gate/evidence hashes and the promotion decision in `specs/175-ndnsf-di-streamed-invocation/evidence/local-closure.md`, `results/spec175/g0/`, `results/spec175/g1/`, `results/spec175/g2/`, `results/spec175/g3/`, and `results/spec175/g4/`. Do not build, upload, or rebuild a SIF during core or host MiniNDN debugging; a post-G3 source change invalidates the candidate and restarts at G0.
  - **Candidate checkpoint (2026-08-25)**: `results/spec175/candidate-final-20260825b.json` binds G0-G3, the definition, source archives, base SIF, build record, preflight record, host-gate manifest, and one corrected local SIF. G4 remains `PENDING_EXACT_SIF_REPLAY`; no upload or Tiger allocation is authorized.
  - **Superseded candidate checkpoint (2026-08-25)**: `spec175-final-candidate-20260825l` has SIF SHA-256 `sha256:250d510ae13b1d8567b0d83935df70fa14e7bf9dc46fbecbecbee40548262cd8`, complete source seal `sha256:ddb90bb790a81bc5a4d3cca43b7cfc85cdb5fe1180de35ca8ee81147c22c8a9d`, host manifest `results/spec175/g3/host-minindn-manifest-current-20260825l.json`, and passing layer-specific preflights. Its exact replay is retained as 30/30 FAIL at NFD readiness; a one-case smoke after the `--home` launcher fix reached certificate bootstrap but failed decrypt. The host runner is now a new source subject, so candidate l cannot be reused and G0-G3 must be regenerated before the next SIF.
  - **Pass**: `G0 PASS -> G1 PASS -> G2 PASS -> G3 host/CPU MiniNDN PASS -> G4 host-substrate preflight + exact-SIF runtime preflight + host-orchestrated 42/42 replay PASS`; the candidate manifest names one SIF, records `apptainer exec` as the NFD/application launcher for every node, records no host NDNSF/native-extension substitution, and binds all host-harness and runtime hashes. Do not begin T024 otherwise.

- [x] T024 [US4] Before T020 freezes the source, implement and statically/mutationally validate the single frozen Tiger submission interface, historical-success delta checker, and bounded current-SIF deployment-control entrypoint for FR-053..FR-055, FR-057, and SC-008 in `packaging/ndnsf-di-container/jobs/spec175/submit.sh`, `packaging/ndnsf-di-container/jobs/spec175/run-streamed-generation.sh`, a new tracked D0-shaped control entrypoint, `packaging/ndnsf-di-container/jobs/spec175/qualify-stage-readiness.sbatch`, `packaging/ndnsf-di-container/jobs/spec175/qualify-multiprovider.sbatch`, the pre-frozen `conversation-residency` entrypoint required by T032/T034, and `tests/python/test_spec175_sif_preflight.py`, reusing current VPN/SSH/Slurm/Apptainer helpers and the proven Spec170 D0/r23 operational contract. This task closes the source/interface implementation only; T025 executes the current-SIF control after T023 produces the exact G4 candidate.
  - **Completed checkpoint (2026-08-26)**: all five positional gates, the tracked candidate-bound checklist validator, the CPU/no-GPU control entrypoint, prerequisite/hash/cwd/source-overlay guards, and the pre-frozen conversation-residency entrypoint are present. The checklist and SIF/submission mutation suites pass 21/21 and prove invalid input reaches neither SSH/upload nor `sbatch`. This closes the interface only; no Tiger allocation or current-SIF control result is claimed.
  - **Interface boundary**: extend the tracked positional interface to `submit.sh {control|stage-readiness|multi-provider|conversation-residency|performance}` while preserving its environment-bound immutable inputs; `control` does not exist until this task implements and tests it. The `control` gate must not require a model manifest or GPU; the four model gates must. `conversation-residency` maps to the frozen functional checklist gate and is the only G6C entrypoint. Reject flag-style/documented commands that the script cannot parse.
  - **Mandatory no-repeat checklist binding**: implement a repository-owned, version-controlled validator at `packaging/ndnsf-di-container/bin/ndnsf-di-pre-tiger-checklist` for schema `ndnsf-itiger-pre-submit-checklist-v1`, behaviorally matching the current operator-side reference without making the repository runtime depend on any user-local installation. `submit.sh` must require `PRE_TIGER_CHECKLIST` and invoke the tracked validator before any SSH, upload, remote mutation, or `sbatch`; it must write `PRE_TIGER_CHECKLIST_VALIDATION` under the candidate evidence directory and bind that result to the closure-manifest candidate ID, positional gate, exact local SIF path/hash, and every required evidence-file hash. Map `control`, `stage-readiness`, `multi-provider`, and `performance` to checklist gates `control`, `stage-readiness`, `functional`, and `performance`. A missing/unknown row, non-PASS row, empty/missing/stale evidence, gate/candidate/SIF mismatch, unclassified native exit, or failed current-SIF control for a model gate must exit nonzero before allocation, with no override flag. Add mutation tests proving each rejection and a spy assertion that neither SSH/upload nor `sbatch` is reached; the checklist validator proves evidence completeness and binding, while the repository-specific preflights remain the semantic authority.
  - **Gate-specific prerequisite binding**: `control` requires the exact G4 candidate; `stage-readiness` additionally requires the passing current-SIF Tiger control; `multi-provider` requires the same candidate's passing G5 stateful-stage/cache-effectiveness manifest; and `performance` requires that G5 manifest plus the same candidate's passing G6 functional manifest. The validator and submitter must reject missing, stale, differently hashed, or wrong-candidate prerequisite manifests before network access or allocation.
  - **Fixed preflight**: lower-gate proof, no unclassified current-subject native exit, allocated compute-node Apptainer 1.5.3 parity, exact remote SIF/config/model/workload hashes, rendered Slurm lint, explicit bundle cwd, isolated HOME/PIB/TPM, no source overlays, missing-only content-addressed upload, node-local stage-cache capacity/write/fsync/hash readiness, free space, GPU/driver/ORT compatibility, output path, secret exclusion, and a machine-readable comparison with Spec170 D0 Job 189483/r23 before `sbatch`. Every intentional launcher/topology/runtime delta is named and justified. Tiger stages and executes the exact SIF directly; it does not run MiniNDN/NLSR or rebuild/materialize the image.
  - **Current-SIF control contract**: freeze the post-G4 command and evidence schema for one bounded CPU/no-GPU Tiger job with one Controller, one User, four Providers, real NFD, Request, four ACKs, provider-specific Selection, final Response, exact SIF hash staging, explicit bundle `cwd`, isolated identities, and every child exit checked. It is deployment evidence only and cannot satisfy G5--G7. This task does not allocate Tiger resources.
  - **Pass**: every mutated mismatch, including a checklist mutation, refuses before network access or allocation; focused tests prove the tracked validator, positional commands, prerequisite graph, exact-hash/cwd/identity checks, rendered Slurm inputs, and a spy assertion that neither SSH/upload nor `sbatch` is reached by invalid input. The interface, control workload, and evidence schemas are frozen in the T020 source seal. Actual current-SIF control execution is the first T025 step.
  - **Implementation checkpoint (2026-08-24)**: the frozen gate selector, exact-SIF hash guard, bundle-cwd wrapper, and three Slurm entrypoints are implemented and shell-checked. The exact r28 SIF wrapper captures the Provider usage contract from stderr and verifies its expected nonzero CLI status. The pre-submission path is deliberately not marked PASS until G0-G4 and model/stage readiness are closed; see `evidence/t024-submission-interface-20260824.md`.

- [ ] T025 [US4] **Mandatory current-SIF control and G5 readiness:** first execute the pre-frozen T024 CPU/no-GPU current-SIF deployment control through the exact G4 candidate and require exit `0:0` with the full Request/four-ACK/Selection/Response lifecycle. Only then freeze the adapter-certified stateful Qwen3.6-27B canonical ONNX language-model-only manifest with `decodeMode=single-token-autoregressive`, `modality=text-only`, `mtpEnabled=false`, `thinkingMode=disabled`, and the exact chat-template/workload digests; execute each stage (`[0,21)`, `[21,42)`, `[42,64)`) through the same exact candidate SIF and allocated CUDA ORT, stage each artifact once into a content-addressed node-local cache, verify persistent device-resident prefill/decode state, canonical outputs, complete state I/O, absence of vision/MTP runtime components, and all digests, and run the registered eight-token cache-effectiveness control (one excluded warmup plus three alternating-order cached-versus-full-prefix matched pairs per stage) before preserving the combined record in `specs/175-ndnsf-di-streamed-invocation/evidence/tiger-stage-readiness.md`.
  - **Pass**: the same candidate first passes the current-SIF control with exit `0:0` and no runtime substitution; then 3/3 stages load and execute prefill plus incremental decode on CUDA with exact output, complete state-component continuity, one-token cached decode input, monotonically advancing state epoch/length, persistent I/O binding, zero complete-state host round trips after prefill, zero CPU model-compute fallback, bounded memory, verified cache identity, and separate staging/load/execute/transfer timing; every matched pair retains output parity and lower median cached model-compute time; the stateful CUDA ORT oracle uses the exact tokenizer digest, produces in-vocabulary token IDs and a nonempty decoded transcript, and G6 resolves the same paths without another project-filesystem copy. `useCache=false` as the primary subject, empty transcript, host-materialized per-token state, unfavorable omitted pair, or optional 0.6B diagnostic fails T025.

- [ ] T026 [US4] **Mandatory primary result:** execute G6 with pinned `Qwen/Qwen3.6-27B@6a9e13bd6fc8f0983b9b99948120bc37f49c13e9` canonical ONNX language-model-only text subject, single-token autoregressive decode, MTP disabled, the frozen split `Stage0[0,21)`, `Stage1[21,42)`, `Stage2[42,64)`, and the pinned signed ACK capability/residency manifest whose expected placement is `P0/P1/P2`, one GPU per complete role, and the exact candidate through the positional `submit.sh multi-provider` interface, preserving all six measured functional invocations in `specs/175-ndnsf-di-streamed-invocation/evidence/tiger-multiprovider.md`; neither caller input nor offline artifacts may contain the Provider-role map.
  - **Pass**: 6/6 exact streams/final results, one request/plan/prefill, exact activation/feedback/event lineage, one automatic Provider-owned state hit/transition/commit per role and non-prefill epoch, zero unexpected miss/recompute, zero decode-state bytes on NDN edges, zero complete-state host round trips, bounded cleanup, all three GPUs used through ONNX Runtime `CUDAExecutionProvider`, zero CPU model-compute fallback (bounded CPU shape-control is reported separately), no deployed Transformers/PyTorch, and no runtime source overlay. Caller/harness state feedback, full-context recomputation per token, a per-token `distributed_inference(...)` call, or a 0.6B diagnostic fails T026. Do not run G6C or G7 on failure.

- [ ] T027 [US4] After the same candidate passes G6C, execute and analyze G7 without parameter changes by running three fresh processes, each with one recorded/excluded cold plus ten alternating-prompt warm invocations, through the positional `submit.sh performance` interface, computing registered TTFT/inter-token/TPOT/tokens-per-second/component/resource summaries and fixed-seed bootstrap interval in `scripts/analyze_spec175_performance.py`, and recording the unfiltered 30-unit verdict in `specs/175-ndnsf-di-streamed-invocation/evidence/tiger-performance.md`.
  - **Fixed verdict**: `PERFORMANCE_PASS` only with all correctness checks, all G5 cache-effectiveness controls, zero fallback, median warm steady-state >=20.0 token/s, and p95 inter-token <=75 ms; otherwise correct runs are `FUNCTIONAL_PASS_PERFORMANCE_MISS` with the largest measured bottleneck, including state transfer/copy time when applicable.
  - **Pass**: analyzer mutation tests reject omitted failures/runs, changed subject hashes, wrong warmup, pooled configurations, or incorrect threshold arithmetic; all 30 valid units remain visible.

- [ ] T028 [US4] Close Spec 175 by rerunning G0 traceability against actual code/tests/evidence, verifying every FR-001..FR-071 and SC-001..SC-016 as implemented/wired/executed/measured at the correct level, updating only the current feature's `traceability.md` and `specs/175-ndnsf-di-streamed-invocation/evidence/closure.md`, and committing the final coherent state on `Experimental`.
  - **Pass**: no unchecked task, uncovered requirement, unsupported claim, dirty in-scope code, missing evidence hash, or superseded candidate; final report states the G7 verdict honestly and identifies any separately scoped future optimization.

---

## Phase 6: User Story 5 - Multi-Turn Conversation Continuation (Priority: P2)

Task IDs are appended to preserve the already published T001--T028 evidence
references. Their dependency edges below place all source and local-test work,
including T024/T033, before T020 starts formal qualification and before T023
builds the one final SIF.

- [x] T029 [US5] Freeze and implement the additive conversation contract for FR-058..FR-061, FR-068, and FR-071 in `contracts/api-contract.md`, `contracts/wire-protocol-v1.md`, `data-model.md`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/base.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`, and the corresponding native/wire bindings. Add `FULL_CONTEXT` and explicit `APPEND_DELTA`, stable high-entropy `conversationId`, fresh request/generation per turn, expected parent context epoch, opaque authenticated checkpoint, optional authenticated full-input fallback, and awaitable successor-checkpoint result. Define distinct request-local and conversation-scoped state types and metrics; the checkpoint is not a request-cache handle and a request-local hit cannot satisfy cross-request continuation. Keep generic Core streaming and callers without conversation options byte/behavior compatible. The public API MUST expose no Provider list, role map, state tensor, device pointer, cache address, or local path.
  - **Completed checkpoint (2026-08-26)**: the additive Core `ConversationContinuationOptions` wire block and Python `ConversationContinuation` API use fresh request/generation identities, preserve the absent-option full-context path, reject malformed APPEND_DELTA before publication, and expose only an opaque checkpoint. Canonical C++/Python round-trip, mutation, compatibility, and public-surface tests pass. This closes the public contract only; T030/T031 still own Provider receipt verification and production cross-request state reuse.
  - **Pass**: canonical round-trip/default/range/conditional-field and every-field mutation tests pass in C++ and Python; absent options use the existing full-context path; `APPEND_DELTA` without parent checkpoint/epoch is rejected before Request publication; checkpoint capability bytes, prompt text, state, and secrets are absent from logs/manifests.

- [x] T030 [US5] Implement transactional multi-role continuation for FR-059..FR-063, FR-066..FR-068, FR-071, and SC-013/SC-015 in the existing `AutomaticPlanningCoordinator`/streamed collaboration path plus a focused `ndnsf_distributed_inference/conversation.py` owner. Persist `ConversationTranscriptRecordV1` through the existing envelope-key-protected `RuntimeJournal`. At turn completion, freeze the canonical completed transcript; make each role consume any bounded unrepresented final token/template suffix through state-only `CHECKPOINT_FINALIZE` using the next unused authenticated inference epochs and the sealed V1 cap of 32 tokens; stage exact conversation candidates; collect all receipts; compare-and-swap the parent; and only then atomically expose result/checkpoint and release request-local ownership. A new turn MUST create fresh Request/plan/Selection/generation authority, verify the aggregate parent checkpoint, protected transcript, exact tokenizer/chat-template prefix extension, and complete Provider-signed role-receipt set, require the exact prior one-to-one Provider-role map, wait for every role's state-ready record, execute delta prefill over only the appended canonical suffix, retain the parent until every successor receipt is accepted, and compare-and-swap the parent context epoch before returning a successor. Implement the explicit sealed full-context fallback and failure codes; never mix resumed and zero/new role state or migrate state between Providers.
  Local boundary update (2026-08-27): the native lineage wire now authenticates
  `transitionKind` and the Provider/User path emits and validates
  `CHECKPOINT_FINALIZE` for the bounded terminal-prefix state-only pass. Real
  multi-process execution and final evidence remain part of this task.
  - **Authentication boundary**: local aggregate checkpoint authentication MUST use a purpose- and requester-identity-separated key derived from the owner-injected `RuntimeJournal` key ring (active key for new checkpoints, bounded previous keys for verification). Production code MUST reject a missing key source and MUST NOT contain a deterministic shared fallback. This local MAC protects User-owned journal/checkpoint state only; it MUST NOT be counted as, or substituted for, the Provider's real NDN Data signature. Every `ConversationStateReadyV1` and `ProviderConversationStateReceiptV1` must still be published as Provider-signed, requester-encrypted Data and verified through the configured trust/security path before it contributes to the all-role barrier or checkpoint.
  - **Scope correction**: T030 MUST prove a real cross-request conversation-state reuse path, not merely serialize a checkpoint or retain the User transcript. The second turn must have a fresh Request/generation identity, obtain a conversation-scoped hit/readiness result from each selected Provider, consume the promoted KV/recurrent/convolution state for suffix-only prefill, and report that hit separately from first-turn request-local decode hits. Full-prefix recomputation remains a fallback and cannot satisfy the conversation-cache acceptance gate.
  - **Real M11 checkpoint (2026-08-27)**: the current four-Provider MiniNDN
    run completed two fresh requests, accepted four Provider-signed receipt and
    readiness records, committed epochs 1 and 2, and recorded four
    conversation-state hits with zero state bytes on NDN and zero leaked
    request-local entries. The run is preserved in
    `evidence/m11-real-20260827.md`.
  - **Implementation closure (2026-08-27)**: a current-source real M11 run
    executed two fresh Requests through four independent Providers, verified
    all role Ready/receipt/commit records, promoted epochs 1 then 2, reported
    four conversation hits and delta prefill, and exposed no state tensor bytes
    on NDN. Focused mutation tests cover rollback, scope, lineage, and commit
    failure. This closes the production transaction implementation boundary;
    repeated M11--M14 qualification is owned by T022, not by this task.
  - **Current-source implementation checkpoint (2026-08-26)**: the native
    Provider now stages a finalized role state, emits a signed/encrypted
    collaboration receipt, and waits for an exact User COMMIT/ROLLBACK control
    before exposing it to a later Request. It also publishes a compact
    signed/encrypted state-ready record after resolving a role-local parent
    reference; the Python streaming owner validates one ready record and the
    complete receipt set for every selected role, binds the committed entry to
    the aggregate checkpoint digest, and emits one role-bound control per
    selected Provider. The focused receipt/readiness barrier, expiry, and
    completion-race tests pass. The task remains open until the real
    multi-process MiniNDN path proves all-role commit/failure behavior,
    explicit terminal-prefix finalization, and suffix-only reuse rather than a
    local emulation.
  - **Pass**: deterministic 1/2/4-role and client-restart tests prove exact full-transcript oracle parity, protected journal recovery, failure on missing/wrong journal key/record, fresh per-turn authority, one delta prefill with positive prefix work avoided, all-role atomic commit/rollback, one winner for concurrent children, preserved parent on every failure, and zero runner calls after rejected checkpoint/readiness validation.

- [x] T031 [US5] Implement the Provider-owned conversation-state manager for FR-062, FR-064..FR-066, FR-069, FR-071 and SC-014 in `NativeProviderRuntime`, `NativeProviderSession`, the Qwen stateful ONNX adapter, and the Python/native Provider bridge. Accept only atomic ownership promotion of an exact finalized `ProviderDecodeStateEntryV1`; never search the request-local store from a later Request. Add exact `ConversationStateEntryV1` identity, independent bounded GPU/host byte and entry quotas, orthogonal residency (`GPU_RESIDENT/HOST_RESIDENT/EVICTED`) and lifecycle (`IDLE/PREFETCHING/PINNED/COMMITTING`) transitions, deterministic inactive LRU, configurable capped 300-second default retention, active/commit/dispatch pinning, single-flight asynchronous host-to-device prefetch, buffer release/zeroization, deadline/cancel cleanup, and separate request-local/promotion/conversation-hit per-role metrics. Model weights remain GPU resident; no disk tier or state transport over NDN is permitted.
  - **Local boundary closed (2026-08-27)**: ConversationStateStore now wipes retained TensorBundle payload bytes before eviction, expiry cleanup, Provider-boot invalidation, and explicit clear. Cross-process Provider ownership, real CUDA residency, and the M11--M14 execution proof remain open.
  - **Production wiring boundary**: the V3 per-Provider Selection projection MUST carry the aggregate checkpoint digest plus only the selected local role's parent epoch and receipt commitment. `NativeProviderHandler` MUST resolve that compact authenticated reference against the Provider's conversation store, publish the signed/encrypted readiness result, and set `NativeEpochCoordinatorConfig::conversationStateBinding` only after an exact match. On completion it MUST promote the actual finalized role state, publish the signed/encrypted successor receipt, and release request-local ownership only after the promotion transaction commits. Direct unit calls to `lookup`, `promote`, or a Python manager do not satisfy this requirement.
  - **State correction**: `ConversationStateEntryV1` MUST own the actual complete adapter state bundle after promotion. Receipt/checkpoint/transcript bytes are commitments and authorization metadata only; they cannot be counted as a cache hit or used as a substitute for state readiness. A later Request may use only this separate conversation store after exact identity and prefix validation.
  - **Current-source implementation checkpoint (2026-08-26)**: the native
    store now has explicit staged/committed promotion, Provider boot/cache
    invalidation, expiry cleanup, bounded GPU/host residency, deterministic
    LRU, single-flight prefetch, and exact receipt/reference/checkpoint
    matching. The `*_out` to successor `*_in` device-state mapping and
    delayed-prefetch expiry check are covered by focused native regressions.
    CUDA-resident execution, complete state ownership in the Qwen3.6 graph,
    and a real cross-request Provider process remain open acceptance gates.
  - **Local cleanup correction (2026-08-27)**: the Python bridge now restores
    failed host-to-device prefetches to retryable `HOST_RESIDENT`/`IDLE`, fences
    stale cancelled workers with a generation increment, and releases aliased
    request/conversation/staged state at most once during Provider-boot
    invalidation. Native binding changes also clear stale staged-promotion
    side-index records and reject mismatched aggregate checkpoint digests.
    The 29-case conversation cleanup regression passes; this does not close
    the real Provider/CUDA acceptance gates above.
  - **Implementation closure (2026-08-27)**: real M11 confirms Provider-owned
    cross-request promotion and exact later-request lookup across all four
    roles, with zero request-local leaks. M12 supplies the current bounded
    host-tier/isolation checkpoint, while the transition, quota, expiry,
    restart, prefetch, cancellation, and zeroization matrices pass locally.
    This closes the CPU/provider implementation boundary. Exact CUDA
    GPU-host-GPU movement and residency remain exclusively T034/G6C.
  - **Pass**: unit and CPU-tier-emulation tests cover every transition, quota edge, oversize state, expiry, boot/cache reset, concurrent conversation isolation, duplicate prefetch, cancellation, and pin protection; accounting returns to baseline after cleanup; state contents never enter logs, manifests, or dependency packets.

- [x] T032 [US5] Before T020 runs any formal manifest, extend the current-source G0/G1/G2 implementation and gate tooling for FR-069..FR-071 and SC-013..SC-015. Register the native/Python conversation suites and I16--I20 exactly as defined in `contracts/validation-contract.md`; add mutation checks that distinguish actual delta-prefill work from a hit counter around full-prefix execution. Before any SIF exists, also implement and statically/mutationally validate the frozen `submit.sh conversation-residency`/G6C workload and evidence schema so no post-SIF source edit is needed.
  - **Completed checkpoint (2026-08-26)**: I16--I20, conversation metrics/schema rejection, full-prefix-disguised-as-hit negatives, source/binary binding, and the pre-frozen G6C launcher/checklist mutations are registered and pass their focused local suites. This closes gate implementation only. The earlier v5 G0--G2 execution is historical after later source fixes; T020's current closure supplies the replacement manifests.
  - **Pass**: focused positive/mutation tests prove that I01--I20 are registered with the required process/repetition/metric schema and that the runner rejects direct new-request lookup of request-local state, missing or event-producing checkpoint-finalization, incomplete promotion, missing state/predecessor/checkpoint/residency/fallback/conflict evidence, full-prefix work disguised as delta prefill, unexpected skips, state-tensor NDN bytes, and source/binary drift. Tests separately prove non-conversation terminal cleanup, accepted-token/template-suffix finalization, all-role zero-copy-or-copy ownership handoff, raw-message versus canonical-delta token counts, and parent preservation on failed promotion. G6C launcher mutations reject another candidate, missing G6, changed role map/workload, missing tier metrics, or runtime overlay. T020, not T032, executes the final same-source repeated G0--G2 manifests.

- [x] T033 [US5] Before T020 freezes the source, extend the real host/CPU MiniNDN harness and exact-SIF replay inventory with M11--M14. M11 proves two-turn four-role delta-prefill parity; M12 proves three-conversation isolation and bounded CPU host-tier emulation; M13 covers missing role, expiry, Provider restart, and wrong checkpoint/model/plan with explicit fallback/failure; M14 covers concurrent same-parent turns and cancellation during prefetch. Preserve the existing M01--M10 definitions. This task implements and locally validates the harness/case contracts; T022 later executes the complete M01--M14 host matrix, and T023 replays it from the exact final SIF.
  - **Reopened by production-path audit (2026-08-26; superseded for implementation status)**: the tracked M11--M14 entry point initially executed the real tiny-ONNX stream but fabricated all Provider receipts and four `ProviderConversationStateManager` instances inside the User process. That historical run remains useful only as a focused transaction-model test. The helper now requires an explicit test-only emulation flag, while formal MiniNDN/SIF runs fail closed with `SPEC175_REAL_PROVIDER_CONVERSATION_PATH_REQUIRED` until T030--T031 wire the real path. The implementation boundary was subsequently closed by the focused formal-path guards and current M11--M14 real-path checkpoints recorded below; repeated clean-source qualification is recorded by T020/T022.
  - **M11 real-path checkpoint (2026-08-27)**: M11 now reaches the real
    multi-process Provider path and passes the two-turn tiny-ONNX case. The
    local emulation helper remains test-only.
  - **Deadline-bound promotion correction (2026-08-27)**: receipt, state-ready,
    and Provider commit-ack waits now use the authenticated request deadline
    instead of an unconditional 30-second wait; expired promotion fails before
    a successor checkpoint is exposed. This closes a timeout-boundary defect
    in the local owner path but does not close the real multi-process gate.
  - **Receipt-lineage/error-sink correction (2026-08-27)**: direct
    `commit_turn()` now enforces the same origin request/generation/service,
    requester, and security-domain bindings as checkpoint preparation, while
    stream error callbacks are contained so they cannot escape the delivery
    thread or replace the original terminal error. The later real M11
    checkpoint closes the implementation boundary; repeated Provider-signed
    acknowledgement and all-role transaction proof is recorded by the current
    T022 matrix.
  - **M12 real-path checkpoint (2026-08-27)**: M12 now reaches the same real
    four-Provider path for three isolated conversations. Provider logs prove
    24 bounded HOST pause transitions and 12 HOST-to-GPU prefetches; the User
    evidence records six fresh Requests, twelve conversation hits, zero state
    tensor bytes on NDN, and zero request-local leaks.
  - **M13 real-path checkpoint (2026-08-27)**: M13 passes on four independent
    Providers. A malformed checkpoint is rejected before network publication;
    after Provider restart, a valid continuation fails closed and the explicit
    full-prefill fallback succeeds. Evidence records three fresh Requests,
    four restart events, one pre-network negative check, one authorized
    fallback, zero state-tensor bytes on NDN, and zero request-local leaks.
  - **M14 real-path checkpoint (2026-08-27)**: M14 passes on four independent
    Providers with distinct child request identities. The designated child is
    cancelled by an explicit Provider-side test fault during HOST-to-GPU
    prefetch; shared-flight waiter protection lets the sibling complete. The
    run records three fresh Requests, four HOST prefetches, four HOST pauses,
    one cancelled prefetch, four conversation hits, successor epoch 2, zero
    state-tensor bytes on NDN, and zero request-local leaks. User-side
    `cancel()` is local today; no remote cancellation Interest is claimed.
  - **Implementation closure (2026-08-27)**: M11--M14 each pass once through
    the current real four-Provider MiniNDN entry point with
    `seed=1750001` recorded in the case result. The focused parser, dry-run,
    topology, mutation, lineage, cleanup, and 42-case manifest-contract tests
    also pass. This closes the harness/case implementation task. The clean-source
    repeated qualification matrix is recorded by T020/T022; these checkpoints do
    not qualify SIF, CUDA, Tiger, or Qwen3.6-27B.
  - **Pass**: focused dry-run, parser, topology, lineage-schema, oracle/fallback/conflict, cleanup, and mutation tests prove that M11--M14 reach the intended real process entry points and that the 42-case manifest builder rejects missing cases, wrong repetitions, unclassified native exits, multiple fault dimensions, missing state-ready/receipt lineage, or state tensors on NDN edges. No full MiniNDN matrix, SIF, upload, or Tiger resource is used to close T033.

- [ ] T034 [US5] After the same candidate passes G6, execute the pre-frozen G6C `submit.sh conversation-residency` control and record `evidence/tiger-conversation-residency.md`. Use the pinned Qwen3.6-27B three-role placement, one two-turn conversation, one paused pressure conversation, and one unavailable-role negative/fallback pair. Require actual first-turn request-local decode, exact terminal-prefix finalization, atomic all-role ownership promotion, per-role `GPU_RESIDENT -> HOST_RESIDENT -> PREFETCHING -> GPU_RESIDENT` movement while model weights remain resident, exact fresh-request delta-prefill/full-transcript CUDA parity, fresh turn authority, aggregate successor checkpoint, and complete state/transfer metrics.
  - **Pass**: the exact candidate reports positive matching state transfer bytes, bounded prefetch latency, all-role readiness before execution, zero state bytes on NDN, zero CPU model-compute fallback, no incompatible runner call, one explicit failure without fallback, one exact full-prefill result with authorized fallback, and bounded cleanup. G6C makes no performance-benefit claim but MUST pass before T027/G7.

---

## Dependencies and execution order

```text
T001 -> T003 -> T004 -> T005 -> T006 -> T007
T002 -------------------------------> T011
T007 -> T008 -> T009 -> T010
T010 + T002 -> T011 -> T012 -> T013 -> T014 -> T015
T015 -> T016 -> T017 -> T018 -> T019
T019 -> T029 -> T030 -> T031 -> T032 -> T033 -> T024
T024 -> T020 -> T022-A -> T022-B -> T022-C -> T022-D -> T022-E
T021 -----------------------------------------------> T023
T022-E -> T023
T023 -> T025 -> T026 -> T034 -> T027 -> T028
```

- T002 may run in parallel with T001 because it owns only the deterministic
  fixture and its oracle test.
- After T007, T008 and the initial T011 state-machine test preparation touch
  different owners, but the default execution order above is preferred for one
  Luna agent to avoid concurrent shared-worktree edits.
- T032, T033, and T024 implement and mutation-test all formal gate, MiniNDN,
  exact-SIF, and Tiger entry points before T020 freezes the source. T020 then
  executes G0--G2 once; T022 executes G3. The pre-fix manifests are historical
  evidence only and are never prerequisites for implementation work.
- Tiger tasks are strictly serial and stateful. Never overlap G5-G7 candidates
  or reuse evidence across a changed hash.
- T029--T033 and T024 are source/local-test work and therefore precede T020 and
  T023 despite their appended IDs. T032/T024 freeze the G6C and Tiger
  launcher/workload interfaces before the candidate; T025/T034 execute them
  without changing source after G4/G6.

## Requirement coverage summary

| Requirement group | Owning tasks |
|---|---|
| FR-001..FR-008 public lifecycle | T004, T008, T009, T010 |
| FR-009..FR-020 event wire/delivery | T003, T005, T006, T016 |
| FR-021..FR-030 generation/roles | T011, T012, T014, T015 |
| FR-031..FR-038 decode-state/recovery/fencing | T013, T017, T018, T019 |
| FR-039..FR-044 security/resources | T003, T005, T006, T007, T017, T018 |
| FR-045..FR-048 compatibility/evidence | T004, T008, T010, T015 |
| FR-049..FR-050 unit/integration | T001, T002, T020 |
| FR-051..FR-052 MiniNDN | T022, T023 |
| FR-053..FR-057 SIF/Tiger/evidence | T021, T023, T024, T025, T026, T027, T028 |
| FR-058..FR-063 conversation API/checkpoint/roles | T029, T030, T032, T033 |
| FR-064..FR-069 tiering/fallback/security/metrics | T030, T031, T032, T033, T034 |
| FR-070 cost-ordered validation | T020, T022, T023, T032, T033, T034 |
| FR-071 request-local to conversation-state lifecycle handoff | T013, T020, T022, T029, T030, T031, T032, T033 |
| SC-001..SC-006 correctness/compatibility | T008..T020 |
| SC-007..SC-009 local/Tiger functional | T021..T026 |
| SC-010..SC-011 performance/repetition | T027 |
| SC-012 high-impact decisions fixed; bounded implementation judgment documented | T001, all task acceptance text, T028 |
| SC-013..SC-015 conversation exactness/isolation/negative cases | T029..T033 |
| SC-016 real CUDA-host-CUDA state transition | T034 |

## Independent story checkpoints

- **US1**: T008-T010; generic five-event/zero-event/unary cases pass.
- **US2**: T011-T015; tiny ONNX one-/two-/four-role exact generation passes.
- **US3**: T016-T019; I04-I13 recover or fail exactly as registered.
- **US4**: T020-T028; host/CPU gates block SIF/Tiger until the final one-time SIF reaches the registered verdict.
- **US5**: T029-T034; implementation and all fast/local gates run before T023,
  while only the pre-frozen G6C execution occurs on the final Tiger candidate.

## Implementation strategy

### MVP

Complete T001-T010 and stop. This yields an application-neutral streamed
invocation with one Provider and preserves unary behavior. Do not describe it as
incremental LLM generation until T011-T015 pass.

### Incremental delivery

1. Core event contract and security.
2. Public C++/Python API.
3. Incremental ONNX/Qwen pipeline.
4. Fault/recovery closure.
5. Multi-turn checkpoint/delta-prefill and bounded Provider state tiering.
6. Freeze and locally mutation-test all G0--G7 harness/launcher interfaces.
7. Run one G0--G3 local promotion sequence, including M01--M14.
8. Build one final SIF, then run Tiger control, 27B readiness,
   multi-Provider/G6C, and performance.

### Commit rule

Each T### completion gets one focused commit unless two adjacent tasks are
inseparable in the actual dependency graph; any combined commit names both task
IDs and must satisfy both gates. Documentation/evidence updates directly owned by
the behavior remain in that commit. Unrelated dirty documentation and local
assistant/tooling artifacts remain unstaged.
