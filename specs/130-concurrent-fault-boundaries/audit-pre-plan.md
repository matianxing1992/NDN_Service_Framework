# Spec 130 Goal-Reset Audit

**Date**: 2026-07-21  
**Scope**: User goal reset, withdrawn central Spec 130 artifacts, frozen Spec 129
boundary and current code facts  
**Verdict**: **PASS FOR REPLANNING; WITHDRAWN DESIGN BLOCKED**

## Controlling Finding

The prior plan selected an epoch-scoped central conflict-admission coordinator,
but the intended NDNSF-DI protocol uses Provider-local reserve-before-positive-
ACK and Requester-local ACK-window selection. The central plan changed the
protocol instead of validating the remaining Spec 129 boundaries. All central
implementation/audit authority is therefore withdrawn.

The replacement objective is necessary and independent: current code evidence
shows an already-selected Requester can skip a late ACK, generic Core interprets
DI literals and constructs DI plan/member state, committed expiry can release by
timer without proving a worker stopped, retry/dependency helpers lack maintained
production callers, and the prior runner used local probes for important fault
outcomes.

## Intent Traceability

| Requested boundary | Replacement artifact |
|---|---|
| Cancel central ordering | `spec.md` FR-002/FR-003; `plan.md` Selected Design; T001 |
| Late ACK | US1, FR-004..FR-007, T003, cells 1..3 |
| Two concurrent Requesters | US2, FR-008..FR-012, T004, cells 4..6 |
| Real randomized backoff | FR-013..FR-016, T005, cells 10..11 |
| Long task resource pin | US3, FR-017..FR-021, T006, cells 7..9 |
| Real multi-stage dependencies | US4, FR-022..FR-026, T007, cells 12..14 |
| Move DI interpretation to APP | US5, FR-027..FR-032, T002/T008, cells 15..16 |
| Real failure validation | US6, FR-033..FR-038, T009..T012 |
| Freeze Spec 129 | FR-001, manifest policy, T001/T012/T013 |

## Evidence Boundary

- `implemented`: partial central coordinator code and prototype runner/tests
  exist, but are unaccepted and must be removed/quarantined.
- `implemented but not production-wired`: retry and dependency helper classes.
- `code-confirmed defect/risk`: late ACK skip, DI literal/plan interpretation in
  generic Core, timer-only committed release.
- `measured`: Spec 129 frozen evidence only; it does not measure these Spec 130
  boundaries.
- `proposed`: every replacement Spec 130 runtime behavior and all sixteen live
  cells until implementation and formal execution.

## Gate

Replanning is authorized. Runtime implementation remains blocked until the
redefined spec/plan/tasks/contracts/manifest pass strict structure,
cross-artifact analysis and a fresh code-aware pre-implementation audit. The
old `audit-preimplementation.md` verdict is superseded and must be replaced,
not amended as though the design were continuous.
