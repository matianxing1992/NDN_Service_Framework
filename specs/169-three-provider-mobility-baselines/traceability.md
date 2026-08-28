# Traceability Matrix

| Source | Requirement | Design/Contract | Task | Acceptance Evidence |
|---|---|---|---|---|
| Reviewer BASE-1 | FR-001, FR-006, FR-010 | `plan.md` gRPC baseline; experiment contract | T002, T004 | three endpoints, active probe/failover tests and smoke |
| Fair recovery policy | FR-003, FR-004, FR-005 | `research.md` Decisions 2-3 | T002, T003, T004 | no-oracle source audit; deadline/attempt tests |
| gRPC accounting | FR-006, FR-007 | `contracts/experiment-contract.md` | T002 | focused A-B/A-B-C/status/accounting tests |
| NSC timeout semantics | FR-008, FR-009 | `plan.md` NSC baseline | T003 | timeout/Nack/late-callback focused tests |
| Matched baseline configuration | FR-002, FR-010, FR-012 | formal schedule contract | T004, T005 | traces, manifests and six terminal cells |
| Reproducibility | FR-011, FR-013, FR-014 | quickstart and artifact contract | T004, T005 | two smoke markers and three campaign aggregate files |
| Honest traffic comparison | FR-015 | `plan.md` evidence boundary | T004, T005 | separated application and wire metrics |
| User no-rerun direction | FR-016 | scope boundary and formal schedule | T001, T005 | frozen hashes and zero NDNSF cells/processes |

## Success Criteria Coverage

| Criterion | Tasks | Proof |
|---|---|---|
| SC-001 | T002, T003 | focused tests/build |
| SC-002 | T004 | one gRPC and one NSC smoke, each with `SMOKE_OK` |
| SC-003 | T005 | exactly six unique terminal cell records |
| SC-004 | T002-T005 | structured client summaries and cell analyzer |
| SC-005 | T004, T005 | immutable campaign manifest and schedule validation |
| SC-006 | T001, T005 | pre/post frozen NDNSF hashes |
