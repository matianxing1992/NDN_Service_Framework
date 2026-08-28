# Traceability: Coherent NDNSF-DI User API

| Requirement group | Design authority | Planned task(s) | Acceptance evidence |
|---|---|---|---|
| FR-001-FR-004 composition, direct request, and shared optional prewarm | `plan.md`; public API and handle contracts | T001-T004 | Owner/import tests; cold creator journey; same-coordinator identity/recovery tests |
| FR-005-FR-010 signed intent, references, ACTIVE/ON_DEMAND discovery, and requestable deployments | `data-model.md`; deployment/handle contract | T001, T004, T005 | Definition/revision validation; cold and warm discovery; forged/expired/revoked record cells |
| FR-011-FR-015 coherent request timing, handles, result, and local separation | public API and handle contracts | T004, T008-T009 | Signature/timing negatives; durable result/cancel/events/recovery; compatibility cells |
| FR-016-FR-022 ACK/Selection/preparation/status/readiness semantics | plan protocol; data model; collaboration-status and deployment/handle contracts; `research.md` F11-F12 | T001, T003-T007, T009-T010 | Offer codec; pre-context/context generic status; signed query/watch/wait; per-role DI projection; snapshot recovery; exact readiness; cold/warm/partial-failure MiniNDN cells |
| FR-023-FR-024 provider authority and RunnerAdapter separation | optimizer/provider contract | T003, T006-T007 | Serving-without-admin; lifecycle authorization; runner isolation tests |
| FR-025-FR-027 optimizer contracts | optimizer/provider contract; `research.md` | T007 | External optimizer fixture; type/determinism/candidate/evidence gates; optional READY preference |
| FR-028-FR-029 exports and one owner | public API contract; `research.md` F1-F2 | T001-T002, T006-T007 | Allowlist/import/no-dynamic-delegation tests |
| FR-030 compatibility window | compatibility contract | T008-T009 | Migration map; warning-once; normalized parity tests |
| FR-031 docs/examples | `quickstart.md`; compatibility contract | T001, T009 | Executable English/Chinese snippets and packaged import tests |
| FR-032 security configuration | public API and collaboration-status contracts; plan security model | T002-T010 | Missing-key/state; forged definition/offer/status/readiness/receipt; cross-binding/replay/fence negatives |
| FR-033 MiniNDN acceptance | plan validation section | T010 | Candidate-bound matrix with exact commands, counts, and results |
| FR-034 Spec 111 invariants | plan dependency direction; all contracts | T002-T010 | Architecture imports; consistency/recovery/orphan regressions; final audit |

## Exact Requirement Index

| Requirement | Task | Requirement | Task | Requirement | Task |
|---|---|---|---|---|---|
| FR-001 | T002 | FR-002 | T002 | FR-003 | T001, T004 |
| FR-004 | T004, T009 | FR-005 | T001, T004-T005 | FR-006 | T004-T005 |
| FR-007 | T004-T005 | FR-008 | T005 | FR-009 | T005 |
| FR-010 | T004-T005 | FR-011 | T004 | FR-012 | T004 |
| FR-013 | T004 | FR-014 | T004, T008 | FR-015 | T004, T008 |
| FR-016 | T001, T003-T007 | FR-017 | T001, T003-T005 | FR-018 | T003-T006 |
| FR-019 | T001, T003-T005 | FR-020 | T001, T003, T006 | FR-021 | T003-T006 |
| FR-022 | T003-T005, T007 | FR-023 | T003, T006, T008 | FR-024 | T006-T007 |
| FR-025 | T007 | FR-026 | T007 | FR-027 | T007 |
| FR-028 | T001-T002, T006-T007 | FR-029 | T001-T002, T006-T008 | FR-030 | T008-T009 |
| FR-031 | T001, T009 | FR-032 | T002-T010 | FR-033 | T010 |
| FR-034 | T002-T010 |  |  |  |  |

## Success Criteria Coverage

| Criterion | Proof |
|---|---|
| SC-001 | Executable creator cold-request quickstart and two-statement count |
| SC-002 | Executable remote ON_DEMAND/ACTIVE requester quickstart and invalid-record matrix |
| SC-003 | Canonical signature inspection, timing negatives, and unchanged base-message inventory |
| SC-004 | Explicit export-manifest tests across canonical packages |
| SC-005 | Class ownership/import graph and no-`__getattr__` contract test |
| SC-006 | External optimizer fixture and typed SDK validation; READY preference remains optional |
| SC-007 | Extracted English/Chinese examples in CPU-only/optional-missing jobs |
| SC-008 | Local/MiniNDN cold, warm, pre-context progress, query/watch/wait, progress-loss, readiness, cancel, and recovery matrix |
| SC-009 | Compatibility warning and normalized evidence parity report |
| SC-010 | Strict structure, cross-artifact analysis, cohesion review, and semantic audit |
