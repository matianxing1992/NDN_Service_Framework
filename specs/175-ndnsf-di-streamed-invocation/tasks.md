# Tasks: NDNSF-DI Streamed Invocation

**Input**: Design documents from
`specs/175-ndnsf-di-streamed-invocation/`

**Prerequisites**: [spec.md](spec.md), [plan.md](plan.md),
[research.md](research.md), [data-model.md](data-model.md),
[contracts/](contracts/), [experiment-plan.md](experiment-plan.md), and
[quickstart.md](quickstart.md)

**Tests**: Test-first work is mandatory. Each behavioral task includes the
failing test/fixture, implementation, focused passing gate, and bounded evidence
needed to review that behavior.

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
   one-Provider/one-role ownership, one placement/prefill per generation,
   ONNX-only deployed runtime, default-disabled replacement, or registered
   evaluation meaning.
4. Before editing an indexed source, use CodeGraph for the symbol and callers;
   after editing, use source and focused tests to verify behavior.
5. Preserve unary/Targeted/security/StreamFacade/Spec174 compatibility. Never
   add an authorization bypass, plaintext fallback, silent stream-to-unary
   downgrade, adaptive default, dropped-event mode, or automatic replacement.
6. Never build container-runtime Python/native outputs on the host and copy them
   into SIF. Build inside the candidate SIF stage or ABI-identical sealed builder
   and run the full native preflight before promotion.
7. Do not submit Tiger work until G0-G4 pass for the exact candidate. A Tiger
   failure is first reduced to the lowest gate that can reproduce it.
8. Do not use `git add -A`, reset, clean, implicit stash, or stage unrelated
   documentation, local assistant/tooling files, historical evidence, secrets,
   or raw debug output.
9. Mark a task complete only after its named acceptance gate passes. `compiled`,
   `wired`, or `one smoke ran` is not completion.

## Format: `[ID] [P?] [Story] Description`

- **[P]** means files and acceptance evidence do not depend on another unfinished
  task in the same phase.
- **[US1..US4]** maps to the four user stories in `spec.md`.
- All commands below run from repository root unless explicitly stated.

## Phase 1: Setup and frozen baseline

**Purpose**: Turn the written contracts and current code reality into mechanical
guards before behavior changes.

- [ ] T001 Implement the G0 contract/traceability gate for FR-001..FR-057 and SC-001..SC-012 by adding TLV-collision, placeholder, cross-document default/name, source-owner, runtime-import, task/test/evidence, and dirty-input checks in `scripts/spec175_contract_gate.py`, `tests/python/test_spec175_contract_gate.py`, and `specs/175-ndnsf-di-streamed-invocation/traceability.md`; first make mutation fixtures fail, then require a `PASS` manifest at `results/spec175/g0/qualification-manifest-v1.json` without changing any production behavior.
  - **Fixed owner map**: current `ServiceUser`, `ServiceProvider`, `NDNSFMessages`, `ProviderRuntimeContext`, `QwenGenerationSessionStateMachine`, exact collaboration Data, integration fixture, MiniNDN gate, and local-SIF tooling.
  - **Pass**: G0 command in `quickstart.md` passes; the recorded TLV range is proven collision-free; every requirement has a planned task/test/evidence link; no unresolved high-impact decision remains and any added low-level choice is documented with its test.

- [ ] T002 [P] Build the byte-reproducible CPU ONNX oracle required by FR-050..FR-052 and SC-003 by adding the frozen vocabulary-32/hidden-8/four-block fixture generator, combined graph, graph-valid two-/four-role partitions, tokenizer, prompts, expected token sequences, and content manifest in `tests/fixtures/spec175/build_tiny_causal_onnx.py`, `tests/fixtures/spec175/tiny-causal-lm-v1/`, and `tests/python/test_spec175_cpu_fixture.py`; regenerate twice under the lock and require byte-identical manifests plus real ORT CPU execution.
  - **Fixed output**: normal `[4,5,6,7,8,9,10,2]`, early EOS `[14,15,2]`, and a no-EOS eight-token max case.
  - **Pass**: one-, two-, and four-role local ORT execution matches the committed token/tensor oracle; no fake runner can satisfy the test.

---

## Phase 2: Foundational Core stream contract

**Purpose**: Implement the reusable wire, names, lifecycle, security binding,
queues, and exact Data transport that block all user stories.

**CRITICAL**: No application/DI integration starts until T003-T007 pass.

- [ ] T003 Encode the version-1 request/event/End/completion and deterministic name contract for FR-009..FR-018 and FR-039..FR-041 by writing failing canonical/mutation vectors, allocating exactly TLVs `0xF636..0xF656`, and implementing `StreamRequestOptions`, `InvocationEventMessage`, `StreamCompletion`, binding/transcript digests, nonce/AAD derivation, and exact name build/parse in `ndn-service-framework/NDNSFMessages.hpp`, `ndn-service-framework/NDNSFMessages.cpp`, `ndn-service-framework/InvocationStream.hpp`, `ndn-service-framework/InvocationStream.cpp`, and `tests/unit-tests/invocation-stream-message.t.cpp`.
  - **Reject**: missing/duplicate/out-of-order/unknown field, noncanonical integer, bad range/digest/name, oversize, wrong End shape, or completion mismatch.
  - **Pass**: canonical wire re-encodes byte-identically; every single-component mutation fails before payload delivery; existing message suites still pass.

- [ ] T004 Implement the single shared user/provider lifecycle and terminal authority for FR-001..FR-008, FR-014, FR-018, FR-020, and FR-042..FR-046 in `ndn-service-framework/InvocationStream.hpp`, `ndn-service-framework/InvocationStream.cpp`, `ndn-service-framework/ServiceUser.hpp`, `ndn-service-framework/ServiceProvider.hpp`, and `tests/unit-tests/invocation-stream-lifecycle.t.cpp`, starting from exhaustive transition, duplicate-terminal, cancel-race, deadline, callback-exception, and capacity tests.
  - **Fixed states**: the exact user/provider state sets in `data-model.md`; terminals absorb; `cancel()` is idempotent; destruction does not silently cancel.
  - **Fixed queues**: admission precedes cursor commit; capacity waits propagate bounded backpressure; there is no drop mode or unbounded allocation.
  - **Pass**: every valid transition succeeds once, every invalid transition has zero side effects, and high-water marks never exceed signed options.

- [ ] T005 Implement provider event admission, AES-256-GCM encryption, ECDSA signing, immutable signed-wire retention, exact Interest satisfaction, retry-safe republication, End admission, and completion binding for FR-009..FR-020 and FR-039..FR-044 in `ndn-service-framework/InvocationStream.cpp`, `ndn-service-framework/ServiceProvider.cpp`, `ndn-service-framework/ServiceProvider.hpp`, `ndn-service-framework/HybridMessageCrypto.cpp`, and `tests/unit-tests/invocation-stream-lifecycle.t.cpp`.
  - **Fixed atomic order**: serialize -> bound -> transcript candidate -> queue admit -> cursor commit -> encrypt/sign -> retain/publish; failure after commit terminates and never renumbers.
  - **Pass**: retries reuse identical Data wire/name, retention expires only by contract, queue deadline fails explicitly, and no key/plaintext enters logs.

- [ ] T006 Implement the bounded user exact-Interest window, signature/decryption/lineage validation order, reorder buffer, duplicate suppression, gap retry, Draining state, End/Response closure, and callback dispatch for FR-010..FR-020, FR-038, and FR-042..FR-048 in `ndn-service-framework/InvocationStream.cpp`, `ndn-service-framework/ServiceUser.cpp`, `ndn-service-framework/ServiceUser.hpp`, and `tests/unit-tests/invocation-stream-lifecycle.t.cpp`.
  - **Fixed defaults**: window 16, lifetime 500 ms, retries 3, reorder 64, completion grace 5 s; no environment override.
  - **Pass**: ordered at-most-once callbacks under reorder/duplicate/one-loss, exact timeout outside retention, Response held behind gaps, and stale/tampered Data never enters the buffer.

- [ ] T007 Bind Normal and Targeted streamed requests to the existing authorization/token paths for FR-006, FR-031..FR-041, and FR-045 by adding the request option/key-grant block only after request-ID allocation, deriving the Normal accepted plan or canonical Targeted binding digest, creating a new key/epoch on replacement, and rejecting unsupported/downgraded peers in `ndn-service-framework/ServiceUser.cpp`, `ndn-service-framework/ServiceProvider.cpp`, `ndn-service-framework/NDNSFMessages.cpp`, and the existing security/Targeted unit suites plus `tests/unit-tests/invocation-stream-lifecycle.t.cpp`.
  - **Fixed mode**: Normal default; Targeted requires one Provider and preserves current behavior: cached token -> selection-free fast path; cache miss -> the same streamed invocation is a `TargetedBootstrapRequest`; refill already in flight -> bounded one-Provider normal path. No `RequestServiceStreamingTargeted` alias and no unary downgrade.
  - **Pass**: permission, ABE service attributes, UserToken, ProviderToken, provider permission, signer, replay, policy epoch, key grant, and outer/plaintext-grant negatives fail closed; evidence/logs contain only key digests, and unary requests allocate no stream key/state.

**Checkpoint**: G1 Core subsets pass; the generic non-DI lifecycle is ready.

---

## Phase 3: User Story 1 - Consume One Ordered Streamed Invocation (Priority: P1) MVP

**Goal**: Applications use the frozen C++ or Python lifecycle and receive ordered
events plus one complete result without changing unary behavior.

**Independent Test**: A one-Provider deterministic service publishes five typed
events and one result through real Core codecs; C++ callbacks and Python async
iteration each observe the exact lifecycle, while the existing unary suite is
unchanged.

- [ ] T008 [US1] Expose exactly the typed C++ `RequestServiceStreaming<RequestT,EventT,ResponseT>`, `StreamedInvocationHandle`, `addStreamingHandler`, and `StreamedResponseWriter` contract for FR-001..FR-008 and FR-045..FR-046 in `ndn-service-framework/ServiceUser.hpp`, `ndn-service-framework/ServiceUser.cpp`, `ndn-service-framework/ServiceProvider.hpp`, `ndn-service-framework/ServiceProvider.cpp`, and `tests/integration-tests/invocation-stream-flow.t.cpp`, with serialization failure, handler lifetime, five-event success, zero-event success, failure, cancellation, and full unary/Targeted regression cases.
  - **Pass**: suite `Spec175InvocationStream` proves callbacks 1..5 then one complete result, zero-event completion, no post-cancel callbacks, and no changed existing API signature/behavior.

- [ ] T009 [US1] Expose exact native bindings, the single-consumer Python user async/callback API, and the Python Provider Core-writer API for FR-003..FR-005 and FR-049 in `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/service.py`, `pythonWrapper/ndnsf/__init__.py`, and `tests/python/test_streamed_invocation_api.py`, using real native messages and event delivery rather than Python-only queue/cursor simulation.
  - **Fixed user behavior**: bytes default, optional event/response decoders, `async for`, awaitable `result()`/`cancel()`, immutable request ID/status/metrics, double-consumer `RuntimeError`.
  - **Fixed Provider behavior**: `add_streaming_handler`, `streaming_handler`, and `add_streaming_context_handler` pass the native writer to a synchronous existing-worker handler; exception and return-without-terminal fail the invocation; writer use after return fails closed.
  - **Pass**: real `_ndnsf` import, user async tests, Provider writer/context tests, exception containment, and lifetime negatives pass inside active Python and later exact SIF; no event loop callback or Provider handler is invoked from the Face thread.

- [ ] T010 [US1] Reuse the same Core writer and terminal guard at the DI application boundary for FR-007, FR-017, FR-023..FR-024, and FR-046 by adding only `ProviderRuntimeContext.publish_event`, `finish_stream`, and `stream_cancelled` in `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`, binding them in `pythonWrapper/ndnsf/service.py` and `pythonWrapper/src/ndnsf/_ndnsf.cpp`, and extending `tests/python/test_spec168_provider_generation.py` and `tests/integration-tests/ndnsf-di-core-flow.t.cpp`.
  - **Pass**: streamed finish and existing `publish_final_response` compete for one terminal claim; exactly one wins; nonstreamed DI providers remain unchanged.

**Checkpoint**: User Story 1 independently passes with a generic one-Provider
service. This is the MVP; do not claim LLM streaming yet.

---

## Phase 4: User Story 2 - Generate Tokens Through One Selected ONNX Plan (Priority: P1)

**Goal**: One selected ONNX plan runs prefill once, incremental exact-KV decode,
final-role sampling/feedback/events, and one complete result.

**Independent Test**: The tiny CPU ONNX fixture runs one-, two-, and four-role
plans and matches exact token/final-result oracles with one Request/plan/prefill.

- [ ] T011 [US2] Correct and extend `QwenGenerationSessionStateMachine` for FR-021..FR-022, FR-027..FR-028, FR-033..FR-038 by writing failing transition tests for Prefilling/Decoding/Draining, EOS/stop early success, exact max success, invalid early max, cancellation, deadline, one replacement, stale epoch, and one terminal claim, then updating `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.hpp`, `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp`, and `tests/unit-tests/di-qwen-generation-session.t.cpp`.
  - **Pass**: current exact-max-only completion bug is removed; `complete()` without a reason has no remaining caller; each finish reason is validated by its own condition.

- [ ] T012 [US2] Implement sealed `GenerationTokenEventV1`, Greedy and fixed-seed sampler contracts, and stateful Unicode-safe incremental detokenization for FR-027..FR-030 in `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/generation.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/tokenizer.py`, `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.hpp`, and `tests/python/test_spec175_cpu_fixture.py`.
  - **Fixed tests**: exact Greedy logits; seeded fixed-logit TopK/TopP repeatability; leading-space, empty-delta, multibyte Unicode, EOS, and stop strings across two/three tokens.
  - **Pass**: deployed imports include `tokenizers` but not Transformers/PyTorch; accepted text deltas concatenate to the oracle final text.

- [ ] T013 [US2] Implement exact `KvStateIdentityV1`, persistent ORT session/I/O binding, provider-local candidate/committed KV transaction, clean recompute, and complete field-by-field negative matrix for FR-022 and FR-030..FR-032 in `NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp`, `NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp`, `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.hpp`, `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp`, and `tests/unit-tests/di-qwen-generation-session.t.cpp`.
  - **Fixed match**: every identity field in `data-model.md` exact; no partial or cross-request prefix reuse in V1.
  - **Pass**: each independently mutated field forces recompute/reject, pre-failure KV stays committed, failed candidate never replaces it, and evidence distinguishes hit/miss/recompute.

- [ ] T014 [US2] Implement the one-plan multi-role generation loop and exact internal activation/token-feedback names for FR-021..FR-028, FR-031..FR-036, and FR-047 by extending `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/placement.py`, `NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp`, `examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py`, and `tests/integration-tests/ndnsf-di-streamed-generation.t.cpp`.
  - **Fixed epoch order**: prefill epoch 0 -> token/cursor 1; decode epoch k consumes token k -> token/cursor k+1; End cursor token count+1.
  - **Fixed ownership**: final role samples, publishes internal feedback and external event separately, and claims End/Response; no extra sampling role, per-token Request, or second plan.
  - **Pass**: I01-I03 exact oracles and packet lineage pass with one Request, ACK closure, plan, Selection, prefill, and terminal Response per generation.

- [ ] T015 [US2] Convert the LLM user/workload path from per-token/final-only evidence to the real streamed handle for FR-002..FR-005, FR-017, FR-021, FR-047..FR-048, and FR-055..FR-056 in `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py`, `examples/python/NDNSF-DistributedInference/llm_pipeline/workload.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`, `NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1_evidence.py`, and `tests/python/test_spec175_evidence.py`.
  - **Fixed evidence**: TTFT, every inter-token interval, per-component spans, exact tokens/text/transcript, queues, retries, KV, resource/fallback, End, and final Response; no plaintext prompt/answer/logits/KV in operational logs.
  - **Pass**: the user observes token deltas before final completion, `result()` returns the complete oracle payload, and evidence rejects missing/extra/reordered/misattributed spans.

**Checkpoint**: User Story 2 passes I01-I03; this proves real CPU incremental
streamed generation, not Tiger performance.

---

## Phase 5: User Story 3 - Recover Without Mixing Attempts or Duplicating Output (Priority: P2)

**Goal**: Close deterministic fault, cancellation, security, capacity, and
explicit replacement semantics at integration fidelity.

**Independent Test**: I04-I13 inject one registered fault each and produce the
exact success/failure/logical-transcript verdict in fresh processes.

- [ ] T016 [US3] Close event reorder, duplicate, one-loss retry, permanent-gap, retention expiry, and End-before-gap cases for FR-013..FR-019, FR-038, FR-042, and SC-001..SC-005 by adding deterministic transport interception to the existing integration fixture and cases I04-I07 in `tests/integration-tests/ndnsf-integration-fixture.hpp`, `tests/integration-tests/ndnsf-integration-fixture.cpp`, `tests/integration-tests/invocation-stream-flow.t.cpp`, and `scripts/run_spec175_integration_gate.py`.
  - **Pass**: recoverable cases deliver exact 1..N once; permanent gap fails explicitly; no case skips a cursor or returns partial success.

- [ ] T017 [US3] Close signature, AEAD, name, binding, token, policy, replay, stale-attempt/generation, and End/Response tamper cases for FR-034, FR-038..FR-041, and SC-005 by implementing I09 plus exhaustive mutation cases in `tests/integration-tests/invocation-stream-flow.t.cpp`, `tests/unit-tests/invocation-stream-message.t.cpp`, and the existing token/ABE/security regressions.
  - **Pass**: every invalid event fails before reorder insertion/callback; a valid event under the wrong name/key/attempt cannot be adopted; no debug bypass appears.

- [ ] T018 [US3] Close callback exception, slow consumer, capacity-1 backpressure, cancellation-after-event-3, deadline, and concurrent cancel/publication races for FR-008, FR-020, FR-036, FR-042..FR-044, and SC-005 by implementing I08/I10/I11 in `ndn-service-framework/InvocationStream.cpp`, `tests/integration-tests/invocation-stream-flow.t.cpp`, and `tests/python/test_streamed_invocation_api.py`.
  - **Pass**: no accepted event drops, capacities remain bounded, generation pauses, cancel fences all later application delivery, and terminal/callback occurs at most once.

- [ ] T019 [US3] Implement the explicit one-replacement generation contract for FR-031..FR-038 and SC-005 by adding accepted-prefix sealing, new attempt/key/stream epoch, old-stream fencing, clean prompt-plus-prefix recomputation, logical prefix suppression, default-disabled I13, and opt-in I12 in `NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp`, `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`, `ndn-service-framework/InvocationStream.cpp`, and `tests/integration-tests/ndnsf-di-streamed-generation.t.cpp`.
  - **Pass**: disabled mode never re-executes; opt-in mode uses exactly one new attempt and exact logical transcript; second replacement, mismatched prefix, or stale output fails; no live KV migration is added.

**Checkpoint**: Complete G2 passes in the exact I01-I14 process matrix.

---

## Phase 6: User Story 4 - Qualify Before TigerCluster (Priority: P3)

**Goal**: Make local gates exercise the same signed model/runtime/network path,
then promote one immutable SIF to bounded Tiger functional and performance runs.

**Independent Test**: A failed lower gate prevents submission; a passing
candidate retains one hash through G3-G7 and emits the registered verdict.

- [ ] T020 [US4] Make G1/G2 manifest closure executable and non-skippable for FR-049..FR-050, FR-055, FR-057, and SC-006 by completing `scripts/run_spec175_integration_gate.py`, `tests/python/test_spec175_evidence.py`, and `tests/wscript` registration, running the full native unit binary plus all Python and I01-I14 process cases, and recording suite/case/process/skip/failure inventories under `results/spec175/g1/` and `results/spec175/g2/`.
  - **Pass**: G1 and G2 manifest status `PASS`, zero mandatory skip, three fresh healthy repetitions, full existing unary/Targeted/security/StreamFacade/Spec174 regressions, and no leaked process/resource.

- [ ] T021 [US4] Extend the current local SIF builder and native preflight for FR-030, FR-053, FR-055, FR-057, and SC-007..SC-008 in `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh`, `packaging/ndnsf-di-container/bin/ndnsf-di-spec175-preflight`, `packaging/ndnsf-di-container/lib/spec170_sif_build_boundary.py`, and `tests/python/test_spec175_sif_preflight.py` without adding a Docker/remote-materialization alternative.
  - **Fixed checks**: source/SIF/native hashes; Python 3.10/SOABI/import; RPATH/`ldd`; Boost 1.71; ndn-cxx/SVS Experimental/NFD/MiniNDN; CPU/CUDA ORT; real workload `--help`; bundle cwd/artifacts; writable paths; secret scan; `tokenizers` present; deployed PyTorch/Transformers absent.
  - **Pass**: mutation tests reject stale host `_ndnsf.so`, wrong cwd, missing asset/library/provider, ABI/version/hash drift, fallback, secret, and runtime Transformers; one local candidate is `PROMOTABLE`.

- [ ] T022 [US4] Implement the exact-SIF real MiniNDN star topology and M01-M09 cases for FR-051..FR-052, FR-055..FR-057, and SC-003..SC-008 in `Experiments/NDNSF_DI_StreamedGeneration_Minindn.py`, `tests/python/test_spec175_real_minindn_gate.py`, and the existing MiniNDN process/security helpers, using one controller, repository, user, four separate Providers, router, 100 Mbit/s/10 ms links, real NFD/SVS/ABE, disabled admission, and the frozen tiny ONNX bundle.
  - **Fixed matrix**: three clean processes for each M01-M09 (27/27); seed 1750001, fault seed 1750002; one fault dimension per fault case.
  - **Pass**: exact tokens/final result, Request-to-Response packet lineage, bounded queues/retries, no host NFD/fake transport, and clean process-tree teardown under the G3 SIF digest.

- [ ] T023 [US4] Execute and close G0-G4 for one exact candidate, diagnosing any failure at its owning lower gate and updating regression coverage before rebuilding, then write immutable gate/evidence hashes and the promotion decision in `specs/175-ndnsf-di-streamed-invocation/evidence/local-closure.md`, `results/spec175/g0/`, `results/spec175/g1/`, `results/spec175/g2/`, `results/spec175/g3/`, and `results/spec175/g4/`.
  - **Pass**: `G0 PASS -> G1 PASS -> G2 PASS -> G3 PROMOTABLE -> G4 27/27 PASS`; the candidate manifest names one SIF and all content hashes. Do not begin T024 otherwise.

- [ ] T024 [US4] Implement the single frozen Tiger submission interface and pre-submission checker for FR-053..FR-055, FR-057, and SC-008 in `packaging/ndnsf-di-container/jobs/spec175/submit.sh`, `packaging/ndnsf-di-container/jobs/spec175/run-streamed-generation.sh`, `packaging/ndnsf-di-container/jobs/spec175/qualify-single-gpu.sbatch`, `packaging/ndnsf-di-container/jobs/spec175/qualify-multiprovider.sbatch`, and `tests/python/test_spec175_sif_preflight.py`, reusing current VPN/SSH/Slurm/Apptainer helpers.
  - **Fixed preflight**: lower-gate proof, login/compute probe, exact remote SIF/config/model/workload hashes, rendered Slurm lint, explicit bundle cwd, missing-only content-addressed upload, free space, GPU/driver/ORT compatibility, output path, and secret exclusion before `sbatch`.
  - **Pass**: every mutated mismatch refuses submission before allocation; Tiger never rebuilds/materializes/substitutes the SIF or runtime libraries.

- [ ] T025 [US4] Execute G5 with pinned `Qwen/Qwen3-0.6B@e6de91484c29aa9480d55605af694f39b081c455` canonical ONNX and the exact candidate by running one recorded cold plus three fresh warm processes for each of two prompts through `packaging/ndnsf-di-container/jobs/spec175/submit.sh --gate single-gpu`, preserving job/manifest/log hashes in `specs/175-ndnsf-di-streamed-invocation/evidence/tiger-single-gpu.md`.
  - **Pass**: 6/6 measured exact streams/final results, >=8 tokens unless oracle EOS is earlier, one End/Response, active CUDA ORT, zero CPU fallback, and complete metrics. Do not run G6 on failure.

- [ ] T026 [US4] Execute G6 with pinned `Qwen/Qwen3.6-27B@6a9e13bd6fc8f0983b9b99948120bc37f49c13e9` canonical ONNX, ACK-driven `Stage0[0,21)->P0`, `Stage1[21,42)->P1`, `Stage2[42,64)->P2`, one GPU per complete role, and the exact candidate through `submit.sh --gate multi-provider`, preserving all six measured functional invocations in `specs/175-ndnsf-di-streamed-invocation/evidence/tiger-multiprovider.md`.
  - **Pass**: 6/6 exact streams/final results, one plan/prefill, exact activation/feedback/event lineage, all three GPUs used, zero fallback, no deployed Transformers/PyTorch. Do not run G7 on failure.

- [ ] T027 [US4] Execute and analyze G7 without parameter changes by running three fresh processes, each with one recorded/excluded cold plus ten alternating-prompt warm invocations, through `submit.sh --gate performance`, computing registered TTFT/inter-token/TPOT/tokens-per-second/component/resource summaries and fixed-seed bootstrap interval in `scripts/analyze_spec175_performance.py`, and recording the unfiltered 30-unit verdict in `specs/175-ndnsf-di-streamed-invocation/evidence/tiger-performance.md`.
  - **Fixed verdict**: `PERFORMANCE_PASS` only with all correctness checks, zero fallback, median warm steady-state >=20.0 token/s, and p95 inter-token <=75 ms; otherwise correct runs are `FUNCTIONAL_PASS_PERFORMANCE_MISS` with largest measured bottleneck.
  - **Pass**: analyzer mutation tests reject omitted failures/runs, changed subject hashes, wrong warmup, pooled configurations, or incorrect threshold arithmetic; all 30 valid units remain visible.

- [ ] T028 [US4] Close Spec 175 by rerunning G0 traceability against actual code/tests/evidence, verifying every FR-001..FR-057 and SC-001..SC-012 as implemented/wired/executed/measured at the correct level, updating only the current feature's `traceability.md` and `specs/175-ndnsf-di-streamed-invocation/evidence/closure.md`, and committing the final coherent state on `Experimental`.
  - **Pass**: no unchecked task, uncovered requirement, unsupported claim, dirty in-scope code, missing evidence hash, or superseded candidate; final report states the G7 verdict honestly and identifies any separately scoped future optimization.

---

## Dependencies and execution order

```text
T001 -> T003 -> T004 -> T005 -> T006 -> T007
T002 -------------------------------> T011
T007 -> T008 -> T009 -> T010
T010 + T002 -> T011 -> T012 -> T013 -> T014 -> T015
T015 -> T016 -> T017 -> T018 -> T019 -> T020
T020 -> T021 -> T022 -> T023 -> T024 -> T025 -> T026 -> T027 -> T028
```

- T002 may run in parallel with T001 because it owns only the deterministic
  fixture and its oracle test.
- After T007, T008 and the initial T011 state-machine test preparation touch
  different owners, but the default execution order above is preferred for one
  Luna agent to avoid concurrent shared-worktree edits.
- Tiger tasks are strictly serial and stateful. Never overlap G5-G7 candidates
  or reuse evidence across a changed hash.

## Requirement coverage summary

| Requirement group | Owning tasks |
|---|---|
| FR-001..FR-008 public lifecycle | T004, T008, T009, T010 |
| FR-009..FR-020 event wire/delivery | T003, T005, T006, T016 |
| FR-021..FR-030 generation/roles | T011, T012, T014, T015 |
| FR-031..FR-038 KV/recovery/fencing | T013, T017, T018, T019 |
| FR-039..FR-044 security/resources | T003, T005, T006, T007, T017, T018 |
| FR-045..FR-048 compatibility/evidence | T004, T008, T010, T015 |
| FR-049..FR-050 unit/integration | T001, T002, T020 |
| FR-051..FR-052 MiniNDN | T022, T023 |
| FR-053..FR-057 SIF/Tiger/evidence | T021, T023, T024, T025, T026, T027, T028 |
| SC-001..SC-006 correctness/compatibility | T008..T020 |
| SC-007..SC-009 local/Tiger functional | T021..T026 |
| SC-010..SC-011 performance/repetition | T027 |
| SC-012 high-impact decisions fixed; bounded implementation judgment documented | T001, all task acceptance text, T028 |

## Independent story checkpoints

- **US1**: T008-T010; generic five-event/zero-event/unary cases pass.
- **US2**: T011-T015; tiny ONNX one-/two-/four-role exact generation passes.
- **US3**: T016-T019; I04-I13 recover or fail exactly as registered.
- **US4**: T020-T028; local gates block Tiger, one SIF reaches registered verdict.

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
5. Full local promotion gates.
6. Tiger single-GPU, multi-Provider, then performance.

### Commit rule

Each T### completion gets one focused commit unless two adjacent tasks are
inseparable in the actual dependency graph; any combined commit names both task
IDs and must satisfy both gates. Documentation/evidence updates directly owned by
the behavior remain in that commit. Unrelated dirty documentation and local
assistant/tooling artifacts remain unstaged.
