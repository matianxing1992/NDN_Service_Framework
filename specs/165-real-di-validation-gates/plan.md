# Implementation Plan: Real DI Validation Gates

**Feature**: `165-real-di-validation-gates`  
**Date**: 2026-07-30  
**Spec**: [spec.md](spec.md)

## Summary

Spec 165 replaces ambiguous smoke-test success with a fail-closed local
deployment gate. The default gate must prove four properties before any
TigerCluster run is authorized:

1. every case declares what was real and what was simulated;
2. a pinned Qwen3-0.6B workload performs multi-request, multi-token inference
   through real MiniNDN;
3. the candidate container executes the same workload and evidence contract;
4. long operations use authenticated progress to renew an idle deadline while
   retaining an immutable absolute deadline.

The implementation reuses the current NDNSF collaboration protocol and
`ServiceOperationStatus`. Generic progress admission and deadline state belong
to NDNSF; Qwen generation, model identity, and DI lineage interpretation belong
to NDNSF-DI. Existing fake and unit checks remain useful but cannot satisfy the
deployment gate.

## Current MiniNDN-first checkpoint — 2026-08-01

The strict Gate-B profile now passes with the reused content-addressed Qwen3
artifacts in both NLSR (`results/spec165-minindn-first/20260801T204709Z-28c987c4`)
and static-route (`results/spec165-minindn-first-static/20260801T205214Z-e2281bb3`)
profiles. Both commands fix the AI_Lab topology and convergence waits, require
`--require-real-model`, and validate all three selected roles plus per-stage
artifact/execution evidence. TigerCluster job 181811 was canceled while this
local recheck was prioritized. The first current-source aggregate,
`results/spec165-minindn-first-full/20260801T205752Z-7bc46c27`, retained the
CUDA-wheel CPU-fallback timeout and a container-only absolute topology-path
defect. A CPU-only ONNX Runtime overlay was then built as
`ndnsf-di:spec165-minindn-cpu-gate`, digest
`sha256:11200f32ce8fc037152f9590bb0e65958642d6cbd9a3b6c14e3e94abb5c962c0`.
The focused Gate-C run
`results/spec165-minindn-first-container-cpu/20260801T212933Z-412b76d8`
passed the unchanged workload. The complete A-D aggregate
`results/spec165-minindn-first-full-cpu/20260801T213143Z-1d7b91c8` now passes
all four gates, sets `externalValidationAuthorized=true`, and still records
`tigerClusterSubmitted=false`. This authorizes a separately recorded
TigerCluster preflight, not automatic submission. Generic capability probe
`181812` passed on `itiger07`; it did not load Qwen or start NDNSF-DI. The
existing remote SIF is bound to an older image/source, so a clean current-source
CUDA app/native overlay must be built and materialized before the standalone
preflight. Its already sealed foundation, CUDA runtime, model snapshot, and
content-addressed stage bundle are reuse inputs and MUST NOT be rebuilt,
downloaded, or exported again.

The accepted six-file stage bundle is now stored once at
`results/_artifacts/qwen-stage-bundles/sha256/ab0695659d5d0a589d89349737d214d2d06b1c3697e013d4409a6a376c41358e`.
Its 6,014,071,792 payload bytes were imported by hard link with zero duplicate
payload bytes; current and future run directories retain direct links and
manifests instead of owning another model tree.

## Technical Context

**Language/Version**: Python 3.10+ experiment and validation tooling; C++17
NDNSF runtime only where generic status admission requires extension.

**Primary Dependencies**: MiniNDN, NFD, python-ndn, NDNSF Python bindings,
Transformers/ONNX Runtime as selected by the explicit backend, Qwen3-0.6B
snapshot `e6de91484c29aa9480d55605af694f39b081c455`, Docker/Apptainer candidate
runtime.

**Storage**: immutable local Hugging Face snapshot; JSON/JSONL evidence and CSV
measurement outputs under a unique result directory.

**Testing**: deterministic Python unit tests, negative contract fixtures,
real-process MiniNDN integration, and the same-workload candidate-container
integration.

**Target Platform**: Linux development host for local gates; TigerCluster is a
later, explicitly authorized external-validity stage.

**Project Type**: C++ framework plus Python NDNSF-DI orchestration and
experiment harnesses.

**Performance Goals**: this feature gates correctness and evidence integrity,
not a throughput claim. Every accepted measured invocation retains at least
eight token events and all required latency fields.

**Constraints**: no mutable model download during a gate; no fake payload may
satisfy real deployment evidence; CPU execution is allowed only when declared;
no silent CPU/GPU fallback; idle renewal cannot extend the hard deadline.

**Scale/Scope**: one User, one Controller/security carrier, three Provider
roles, two prompts, one warmup plus three measured requests per prompt, and one
candidate-container repetition of the same logical workload.

## Constitution Check

### Pre-design gate

- **Spec-driven**: PASS. The feature owns one coherent validation boundary and
  its four gates are explicit in `spec.md`.
- **MiniNDN default**: PASS. Real MiniNDN is the mandatory local network path;
  host NFD and TigerCluster cannot satisfy it.
- **Security in the real path**: PASS. Lineage and progress admission reuse the
  authenticated Request/ACK/Selection/status/Response path.
- **Generic framework boundary**: PASS. NDNSF owns generic collaboration
  progress and deadline semantics; NDNSF-DI owns model-specific meaning.
- **Evidence integrity**: PASS. The aggregate is fail-closed and binds source,
  model, image, run, and request identities.
- **Cohesive tasks**: PASS. Tasks are organized by delivered behavior, not by
  file.

## Architecture

### Gate composition

```text
default local gate
  ├─ Gate A: fidelity contract and fail-closed aggregation
  ├─ Gate B: pinned Qwen3 + real MiniNDN + request lineage
  ├─ Gate C: same workload in candidate container
  └─ Gate D: progress admission + idle/hard deadlines
       └─ all pass -> TigerCluster becomes eligible, never auto-submitted
```

Gate results are independent records. An aggregate accepts only records from
the current run whose schema, source revision, model identity, workload
identity, and mandatory-case identifiers match the gate policy.

### Runtime flow

```text
User establishes requestId
  -> Request
  -> authenticated ACK collection
  -> Selection bound to request/attempt/plan/model
  -> provider operation status and intermediate stage events
  -> multi-token generation
  -> Response with the same lineage
```

Each event is appended to an ordered lineage record before it can change the
request state. A mismatch is retained as rejected evidence and cannot complete
the request or renew its idle deadline.

### Deadline flow

```text
start
  idleDeadline = start + idleBudget
  hardDeadline = start + hardBudget

valid advancing status
  idleDeadline = min(now + idleBudget, hardDeadline)

now >= hardDeadline -> HARD_TIMEOUT
else now >= idleDeadline -> STALLED
terminal result -> first terminal wins
```

The clock is injected. Authentication and binding validation happen before
deadline admission. Duplicate, reordered, non-advancing, forged, wrong-bound,
or post-terminal events are observable but do not renew the deadline.

## Phase 0: Research Decisions

The decisions and rejected alternatives are recorded in
[research.md](research.md):

- introduce a first-class fidelity record instead of inferring fidelity from
  script names;
- pin the already available Qwen3-0.6B snapshot and prohibit implicit download;
- extend the existing MiniNDN harness rather than create a simulated parallel
  path;
- use one workload manifest for host and container gates;
- compose renewable idle and immutable hard deadlines;
- preserve the NDNSF/NDNSF-DI ownership boundary;
- defer TigerCluster until local evidence passes.

## Phase 1: Design and Contracts

### Data model

[data-model.md](data-model.md) defines fidelity records, model and workload
identities, invocation lineage, generation measurements, progress observations,
deadline state, and aggregate verdicts.

### Contracts

- [contracts/fidelity-and-aggregate.md](contracts/fidelity-and-aggregate.md)
  defines versioned evidence and fail-closed aggregation.
- [contracts/invocation-lineage.md](contracts/invocation-lineage.md) defines
  request identity propagation and rejection semantics.
- [contracts/progress-deadline.md](contracts/progress-deadline.md) defines
  progress admission and dual deadlines.
- [contracts/default-gate-cli.md](contracts/default-gate-cli.md) defines the
  default command, workload profile, exit codes, and TigerCluster boundary.

### Reproducibility

[experiment-plan.md](experiment-plan.md) freezes the material passport,
measurement unit, warmup/measured counts, retained metrics, failure accounting,
and monitoring rules. [quickstart.md](quickstart.md) is the operator contract.

## Implementation Strategy

### Slice 1: Gate A — evidence truthfulness

Add a shared schema/validator and a default aggregate runner. Relabel existing
quick checks by fidelity without deleting them. The runner refuses skipped,
stale, malformed, simulated, or cross-run substitutes for mandatory cases.

### Slice 2: Gate D — generic progress deadlines

Implement deterministic progress admission and deadline state against the
existing generic operation-status fields. Prove all boundary and race cases
with an injected clock. Integrate the accepted status stream without adding
model-specific semantics to NDNSF.

### Slice 3: Gate B — real Qwen3 MiniNDN

Extend the current DI MiniNDN workload to accept a workload manifest, resolve
the pinned local model, execute two prompts with warmups and measured requests,
retain at least eight token events per measurement, and produce complete
lineage and metric evidence. Negative lineage cases must run in the default
test suite.

### Slice 4: Gate C — candidate container

Run the identical workload manifest in the candidate image with explicit
mounts and resource limits. Record image digest, model mount identity, backend,
exit/OOM status, and the same result schema. Absence of a candidate image is a
blocking failure, not a skip.

### Slice 5: Aggregate closure

Wire all four gates into one default command, compare machine and human
accounting, run the local acceptance matrix, and keep TigerCluster disabled.

## Migration and Rollback

- Existing unit, fake, startup, and socket checks remain callable and retain
  their historical semantics; only their fidelity classification changes.
- The new aggregate becomes the deployment-authorizing command. Legacy quick
  suites remain diagnostic and cannot independently authorize deployment.
- Evidence schemas are versioned. Unknown schema versions fail closed.
- Rollback consists of disabling the new aggregate entry point while retaining
  its evidence; it must not reclassify lower-fidelity results as deployment
  proof.
- No wire-format migration is planned unless implementation proves the existing
  `ServiceOperationStatus` fields insufficient. Any such change requires a
  separate audited amendment.

## Security and Threat Model

| Threat | Required control |
|---|---|
| Fake result presented as deployment proof | explicit real/simulated inventory and minimum fidelity policy |
| Stale or cross-run evidence | bind run ID, source revision, model/workload identity, timestamps, and case ID |
| Request confusion | canonical User-created request ID plus attempt/plan/model bindings at every admission |
| Forged or replayed progress | authenticate first; require correct binding and monotonic epoch/sequence/work |
| Infinite liveness extension | immutable hard deadline |
| Premature stall during valid preparation | renewable idle deadline on admitted advancing progress |
| Hidden backend fallback | explicit requested/actual placement and fallback count |
| Container substitution | immutable image digest and same workload digest |

## Verification Strategy

1. Schema/unit tests reject every malformed or substituting fidelity record.
2. Deterministic clock tests cover valid progress, duplicates, reorder,
   forgery, wrong binding, stall, hard cap, races, and post-terminal events.
3. Real MiniNDN executes the frozen Qwen3 workload and validates all request,
   token, answer, metric, and lineage counts.
4. Candidate container executes the same workload digest and schema.
5. Negative fixtures prove no fake, skip, one-token, stale, or mismatched
   evidence can satisfy the aggregate.
6. The final audit compares spec, plan, tasks, contracts, code ownership, and
   produced evidence before any TigerCluster work is permitted.

## Project Structure

### Documentation

```text
specs/165-real-di-validation-gates/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── experiment-plan.md
├── quickstart.md
├── contracts/
│   ├── default-gate-cli.md
│   ├── fidelity-and-aggregate.md
│   ├── invocation-lineage.md
│   └── progress-deadline.md
├── checklists/requirements.md
├── tasks.md
└── audit.md
```

### Source code

```text
Experiments/
├── NDNSF_Run_Minindn_Quick_Checks.py
├── NDNSF_DI_LlmPipeline_Minindn.py
├── NDNSF_DI_Run_Local_Deployment_Gates.py
└── ndnsf_validation/
    ├── fidelity.py
    ├── lineage.py
    ├── deadlines.py
    └── workload.py

tests/
├── test_validation_fidelity.py
├── test_validation_lineage.py
├── test_progress_deadline.py
└── test_local_deployment_gate.py

ndn-service-framework/
├── ServiceProvider.hpp
├── ServiceProvider.cpp
├── ServiceUser.hpp
└── ServiceUser.cpp
```

**Structure decision**: validation policy and DI workload logic live under
`Experiments/ndnsf_validation`; existing harnesses are extended in place.
Generic C++ files are changed only if required for authenticated status
admission, not to add Qwen-specific behavior.

## Complexity Tracking

No constitution violation requires an exception.

## Post-Design Constitution Check

PASS. The design keeps MiniNDN as the default network proof, embeds security
and negative cases in the real path, preserves generic/model-specific
ownership, defines rollback, and produces cohesive implementation slices.
