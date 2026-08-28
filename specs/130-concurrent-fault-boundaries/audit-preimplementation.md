# Spec 130 Goal-Reset Pre-Implementation Audit

**Date**: 2026-07-21  
**Mode**: code-aware pre-implementation  
**Verdict**: **PASS**

## Controlling Reason

The redefined specification, plan, contract, manifest and tasks match the
requested protocol: Provider-local reserve-before-positive-ACK, Requester-local
selection, release-before-full-jitter retry, and no central/global ordering.
The six unverified Spec 129 boundaries have explicit owners, migration order,
security invariants, real-fault validation and falsifiable success criteria.

This verdict authorizes T001 as the first implementation task. It does not
authorize continuing the existing central prototype, skipping migration gates,
running a formal cell, or claiming any replacement behavior is implemented.

## Findings

| ID | Severity | Dimension | Location | Finding | Required action |
|---|---|---|---|---|---|
| A1 | Resolved | Intent fidelity | `spec.md` Goal Reset; `plan.md` Selected Design | The previous central coordinator contradicted the clarified ACK/reservation protocol. It is now explicitly withdrawn and prohibited. | T001 must surgically remove its exports/callers/modes before other Spec 130 code is accepted. |
| A2 | Resolved | Code reality | `ndn-service-framework/ServiceUser.cpp:4977`, `ServiceUser.cpp:5822`, `ServiceUser.cpp:6352` | Current pre/post-decrypt guards and `selectLateAckAfterAckTimeout` skip a late ACK after a Provider is selected or the pending call closes. | T002/T003 introduce bounded generic liability hooks/tombstones and prove exact-target negative closure. |
| A3 | Resolved | Architecture ownership | `ServiceUser.cpp:5387`, `ServiceUser.cpp:5604`, `ServiceProvider.cpp:3171` | Generic Core currently compares the DI capability and synthesizes/interprets DI plan/reservation state. | T002 migrates maintained callers to the NDNSF-DI adapter; T008 removes dead Core policy branches after parity. |
| A4 | Resolved | Distributed correctness | `core/deployment_control.py:257` | Current expiry releases `COMMITTED` records by time without proving a running executable stopped. | T006 adds explicit EXECUTING/STOPPING/COMPLETING pin transitions and contender tests. |
| A5 | Resolved | Runtime wiring | `core/recovery.py:21`, `core/execution.py:73`; current callers in `plan.py` and the Spec 129 launcher | Retry and dependency helpers exist but maintained high-level client/provider execution is not proven; launcher probes are insufficient. | T005/T007 wire and test maintained production entry points; SC-009 requires a non-test/non-factory/non-probe caller. |
| A6 | Resolved | Evidence integrity | withdrawn `run_spec130_concurrent_fault_matrix.py` and `NDNSF_DI_ConcurrentFaultBoundaries_Minindn.py` | The prior 68-cell design and local fault probes cannot validate the new objective. | T009/T010 replace them with the frozen sixteen-cell multi-host/process/NFD real-fault path; T012 is the only formal run. |
| A7 | Resolved | Security/operations | `contracts/boundary-repair-contract.md` | A post-window tombstone could become a memory/retention attack surface if unbounded. | FR-007/T003 require authorized-Provider limits, pre-publication lifecycle capacity, per-request/per-identity/global quotas and no active-liability eviction. |

## Traceability

| Source boundary | Story / requirements | Design / contract | Tasks | Success / evidence |
|---|---|---|---|---|
| Cancel central ordering | FR-001..FR-003 | plan supersession; contract supersession | T001 | SC-013; negative caller/import evidence |
| Late ACK | US1; FR-004..FR-007 | generic liability/tombstone contract | T002, T003 | SC-001, SC-011; cells 1..3 |
| Two concurrent Requesters | US2; FR-008..FR-012 | Provider-local reservation | T004 | SC-002, SC-004; cells 4..6 |
| Real randomized retry | US2; FR-013..FR-016 | production full-jitter/release barrier | T005 | SC-003, SC-009; cells 10..11 |
| Long task pin | US3; FR-017..FR-021 | EXECUTING/STOPPING/COMPLETING | T006 | SC-005, SC-006; cells 7..9 |
| Real dependencies | US4; FR-022..FR-026 | direct-predecessor stage-data contract | T007 | SC-007..SC-009; cells 12..14 |
| DI back to APP | US5; FR-027..FR-032 | generic opaque hook + DI adapter | T002, T008 | SC-010, SC-011, SC-014; cells 15..16 |
| Real fault evidence | US6; FR-033..FR-038 | sixteen-cell manifest/evidence contract | T009..T012 | SC-012, SC-013; immutable campaign artifacts |
| Final closure | FR-039 and all | post-audit/convergence/freeze | T013 | full traceability and measured verdict |

## Readiness Scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Six requested boundaries are present; central ordering is prohibited. |
| Architecture and ownership | Yes | Generic lifecycle/security in NDNSF; all DI policy in NDNSF-DI. |
| Security/correctness | Yes | Identity, token, replay, encryption, late liability, release barrier, stop-before-release and predecessor gates are explicit. |
| Task executability | Yes | Each task names concrete source/test/evidence paths and acceptance behavior. |
| Task cohesion/granularity | Yes | 13 behavioral tasks; no test/implementation/evidence fragment chain. |
| Validation/evidence | Yes for implementation | Formal evidence is explicitly absent and cannot begin before T011. |
| Migration/rollback | Yes | Adapter-before-deletion, one reachable interpreter and central prototype removal are ordered. |
| Code reality | Yes | Every discovered mismatch is a named migration target; nothing is falsely claimed complete. |

## Metrics

- User stories: 6
- Functional requirements: 39
- Success criteria: 14
- Tasks: 13
- Mechanically fragmented task groups: 0
- Coalescing opportunities: 0
- Requirement coverage: 39/39 FR and 14/14 SC have task/evidence paths
- Unmapped tasks: 0
- Placeholders: 0
- Critical / High / Medium / Low unresolved findings: 0 / 0 / 0 / 0

## Assumptions and Evidence Limits

- No replacement runtime behavior is implemented or production-wired by this
  planning change.
- No new Spec 130 MiniNDN cell has run; all success criteria remain proposed.
- The CodeGraph index was current at audit time and final claims were checked
  against source locations.
- The worktree contains extensive unrelated user changes. Implementation must
  make surgical edits and must not use broad checkout/reset/cleanup.
- The application-neutral hook names may receive mechanical naming refinement,
  but the field/ownership contract in
  `contracts/boundary-repair-contract.md` is fixed.

## Next Actions

1. Execute T001 only: remove the withdrawn central prototype surface and freeze
   the new manifest/hash boundary.
2. Execute T002/T003: establish the generic hook, NDNSF-DI adapter and late-ACK
   closure before changing retry, pin or DAG behavior.
3. Do not launch formal MiniNDN; T011 must close build/binding/security and one
   non-formal real-fault smoke before T012 becomes eligible.
