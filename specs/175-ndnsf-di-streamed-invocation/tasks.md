# Tasks: NDNSF-DI Streamed Invocation Local Closure

**Input**: `spec.md`, `plan.md`, `contracts/`, and current source/evidence.

The four closure tasks below are the complete frozen Spec175 ledger and are
closed. Historical task lists and Tiger gates are provenance, not executable
Spec175 work. Any post-handoff source or harness change belongs to Spec180 and
must be re-audited there.

**No active task remains in Spec175 (revision 105).** The active feature pointer must remain
on Spec180 while current implementation or qualification work continues.
Spec175 evidence may be consumed as a sealed reference but may not be relabeled
as current Spec180 evidence.

**Revision 108 closure receipt (2026-09-03)**: The absence of a current
NDNSF-DI YOLO experiment is a Spec180 G0/G1 input and execution block, not an
unchecked Spec175 item. The frozen Spec175 ledger must remain four closed
tasks; adding a temporary rebuild, SIF, or Tiger task here would create a
second source of truth and reintroduce the delay this handoff is intended to
prevent.

**Revision 110 closure receipt (2026-09-03)**: The later Spec180 Y-A
diagnostic found a split `.local-boost171`/`/usr/local` `libndn-cxx` closure
and socket EOF before a request. This remains entirely Spec180-owned. Spec175
must not add a task or rebuild to absorb it; Spec180 must rebuild and qualify
its own complete native closure.

The current Spec180 delay is an input/ordering block owned by Spec180, not a
missing task in this ledger. Do not add a replacement Spec175 task for a YOLO
candidate, QWEN-F, SIF, CUDA, or Tiger run.

If a later repair changes a Spec175-owned source or contract, open a new source
subject with a new seal instead of reopening T020--T023. This is a source and
evidence rule, not an optional workflow preference.

**Revision 105 execution rule**: Do not create temporary Spec175 tasks for a
missing Spec180 candidate, QWEN-F, SIF, CUDA, or TigerCluster run. The four
checked tasks below are closed only for the frozen source seal; they do not
qualify the shared working tree or any later model candidate.

## Phase 1: Design-Code Convergence

- [X] T020 [US1] Freeze FR-001..FR-010 and SC-001..SC-005 and audit the real streamed/Qwen production paths; repair each controlling discrepancy with its focused failing test and implementation, then require a fresh `PASS` in `specs/175-ndnsf-di-streamed-invocation/audit.md`, `ndn-service-framework/`, `pythonWrapper/`, `NDNSF-DistributedInference/`, and the directly affected focused tests. Evidence: `evidence/design-code-audit-current-20260902.md`, `evidence/t020-source-seal-current-20260902.json`, `evidence/t020-g0-current-20260902.json`.

**Checkpoint**: No complete suite or MiniNDN result is accepted before T020
returns `PASS`.

## Phase 2: Complete Local Suites

- [X] T021 [US1] From one current source identity, run and record the complete relevant C++ unit/integration and Python suites, including unary/Targeted compatibility, stream ordering/terminal safety, Qwen prefill/decode, state ownership, continuation, cancellation, replay, and no-runtime-Transformers checks in `tests/unit-tests/`, `tests/integration-tests/`, `tests/python/`, and `specs/175-ndnsf-di-streamed-invocation/evidence/`. Evidence: `evidence/t021-local-suites-current-20260902.md`, `evidence/t021-cpp-unit-current-20260902-r3.log`, `evidence/t021-python-gate-current-20260902-r4.json`, `evidence/t021-integration-gate-current-20260902-r1.json`.

## Phase 3: CPU/MiniNDN Production Path

- [X] T022 [US2] Execute the cold streamed-generation and two-turn continuation/mismatch cases through real NFD, NDN-SVS, security, production ACK/Selection, Provider roles, NDN activation/feedback, ordered Tokens, terminal Responses, child-exit propagation, and cleanup in `Experiments/NDNSF_DI_StreamedGeneration_Minindn.py`, `Experiments/NDNSF_DI_LlmPipeline_Minindn.py`, `scripts/run_spec175_integration_gate.py`, and `specs/175-ndnsf-di-streamed-invocation/evidence/`. Evidence: `evidence/t022-local-minindn-current-20260902.md`, `evidence/t022-source-seal-current-20260902-r1.json`, and the five candidate-bound result roots.

## Phase 4: Closure and Handoff

- [X] T023 [US3] Validate same-source traceability, issue `LOCAL_FUNCTIONAL_PASS` or `LOCAL_UNQUALIFIED`, and freeze the consumed APIs, invariants, accepted evidence, and deferred obligations in `specs/175-ndnsf-di-streamed-invocation/traceability.md`, `specs/175-ndnsf-di-streamed-invocation/handoff-to-spec180.md`, and `specs/175-ndnsf-di-streamed-invocation/evidence/local-closure-current.md`. Evidence: `evidence/local-closure-current.md` and `evidence/t023-contract-gate-current-20260902.json`.

## Dependencies

```text
T020 design-code PASS -> T021 complete local suites
                      -> T022 CPU/MiniNDN
T021 + T022           -> T023 closure and handoff
T023                  -> Spec180 formal qualification
```

T021 and T022 are serial acceptance gates because both must use the unchanged
post-audit source identity. Focused repair tests inside T020 are permitted.

## Transferred Work

The former Tiger tasks T025/T028, exact-SIF qualification, CUDA Qwen execution,
YOLO adapter migration, and cross-model qualification are not active Spec175
tasks. They are replaced by the task ledger in
`specs/180-ack-driven-cross-model-qualification/tasks.md`.
