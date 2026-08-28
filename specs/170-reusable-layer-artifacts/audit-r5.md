# Spec Kit Audit Report: Corrected Spec 170

**Audit date**: 2026-08-19  
**Scope**: `spec.md`, `plan.md`, `tasks.md`, `research.md`, `data-model.md`,
`implementation-guide.md`, `quickstart.md`, `experiment-plan.md`,
`traceability.md`, `contracts/`, and the current placement/runtime seams.

## Verdict

`CONDITIONAL PASS`

The corrected documents consistently define ACK-driven
`PreSplitFirstStrategy`, one stage/rank per execution role, a one-to-one
role/Provider map, Provider-local canonical ONNX assembly, ONNX Runtime-only
deployment, and consumer-pull NDN tensor exchange. The deterministic structural
audit passes with complete requirement traceability and no Critical or High
finding. Current source and old campaign workloads do not yet implement that
architecture; this is explicitly represented by T003-T021 and therefore blocks
runtime acceptance, SIF freezing, and TigerCluster submission, but does not
block beginning the dependency-ordered implementation.

## Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| AUD-170-001 | Medium | Code reality | `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/application.py:172` | The unmodified public application path still constructs `LayerReuseFirstStrategy`; this contradicts the documented normal V3 default. | Complete T004: construct `PreSplitFirstStrategy`, retain reuse only as post-feasibility scoring, and prove custom V3 replacement plus no V2 fallback. |
| AUD-170-002 | Medium | Architecture and ownership | `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py:748`, `:831` | `PlacementProposalV3` checks role-key coverage but not uniqueness of Provider values, and `ProviderSelectionProjectionV3` still carries a tuple of roles. The code can therefore represent a same-Attempt multi-role Provider. | Complete T003 and T005 with fail-closed one-to-one validation and an exactly-one-role Provider projection in Python, native codecs, and mutation tests. |
| AUD-170-003 | Medium | Validation and migration safety | `specs/170-reusable-layer-artifacts/jobs/workloads/spec170_v3_local_two_gpu_user.py:45`, `spec170_v3_hybrid_user.py:75` | Existing D2a/hybrid workloads assign several roles to one or two Providers and use legacy `device_set`; running them would reintroduce superseded semantics. | Replace them under T006/T015/T018 and prevent them from closing Gate A/B/C or any Tiger row before the corrected cases pass. |
| AUD-170-004 | Low | Evidence integrity | `specs/170-reusable-layer-artifacts/tasks.md:10`, `traceability.md:3` | Historical same-Provider/NCCL evidence remains in the tree. The new correction boundary rejects it, but final documentation must keep the distinction visible. | Preserve raw evidence for provenance; under T027 label it superseded and bind every closing claim to corrected hashes. |

## Traceability Gaps

| Source/Requirement/Task | Missing link | Impact |
|---|---|---|
| None at document level | All FR-001..FR-075 are mapped to implementation tasks and gates. | No pre-implementation traceability blocker. |
| T003-T021 | Passing implementation and runtime evidence does not exist for the corrected architecture yet. | No correctness, performance, SIF, or Tiger claim may be made until the corresponding block closes. |

## Readiness Scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | The corrected architecture matches the latest user-approved ownership and ONNX/NDN boundaries. |
| Architecture and ownership | Yes for implementation | Contracts are explicit; current code divergence is owned by T003-T006. |
| Security/correctness | Yes for implementation | Names, signed manifests, attempt/plan/epoch fencing, replay handling, cancellation, and fail-closed behavior are required by T011-T014. |
| Task executability | Yes | Tasks name concrete files, tests, ordering blocks, and closing evidence. |
| Task cohesion/granularity | Yes | Each task closes one contract slice plus its tests; no mechanical micro-task chain is required. |
| Validation/evidence | Conditional | Unit, integration, MiniNDN, exact-SIF, and Tiger gates are specified but not rerun under the corrected design. |
| Migration/rollback | Yes | V2 is isolated and explicit; V3 does not silently fall back; frozen-candidate invalidation is defined. |
| Code reality | No | Findings AUD-170-001 through AUD-170-003 are current implementation gaps. |

## Metrics

- User stories: 6
- Functional requirements: 75
- Success criteria: 38
- Tasks: 28
- Mechanically fragmented task groups: 0
- Coalescing opportunities: 0 controlling opportunities
- Requirement coverage: 75/75 functional requirements mapped
- Unmapped tasks: 0
- Placeholders: 0
- Critical / High / Medium / Low findings: 0 / 0 / 3 / 1

## Assumptions And Evidence Limits

- CodeGraph is usable but reports pending additions; source facts cited above
  were read from the current worktree and must be re-indexed after implementation.
- Existing successful local SIF and Tiger rows predate this correction and do
  not verify one-role/one-Provider NDN tensor execution.
- GPU/Tiger availability is not required to begin implementation, but the exact
  locally qualified SIF is required before remote submission.
- No current performance result proves the corrected architecture; performance
  is intentionally downstream of correctness and transport closure.

## Next Actions

1. Close T003 and T004 first with failing ownership/default-path tests, then
   implement the Python placement changes.
2. Close T005 and T006 across Python/native Selection and admission before any
   artifact or dataflow execution work.
3. Implement Provider-local canonical ONNX assembly and the ONNX Runtime-only
   boundary (T007-T010).
4. Implement and fault-test the NDN tensor name, manifest, consumer-pull, and
   readiness contracts (T011-T014).
5. Replace superseded workloads and rerun local integration/MiniNDN/exact-SIF
   gates before considering any TigerCluster job.

