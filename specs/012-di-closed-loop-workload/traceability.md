# Traceability: Spec 012

## Requirements

| Requirement | Story | Task | Current code/evidence |
|---|---|---|---|
| FR-001 | US1 | T001 | driver argument/default/positive validation |
| FR-002 | US1 | T001 | synchronous same-ServiceUser request loop |
| FR-003 | US1 | T001 | per-request marker and first-failure break |
| FR-004 | US1 | T001 | workload plus compatible execution markers |
| FR-005 | US1 | T001 | `summarize_workload` metric contract |
| FR-006 | US2 | T002 | MiniNDN command/parser/summary propagation |
| FR-007 | US2 | T002 | layout campaign row/aggregate propagation |
| FR-008 | US2 | T002 | existing aggregate marker retained |
| FR-009 | US3 | T003 | explicit non-goals and later-Spec boundary |
| FR-010 | US3 | T003 | exact historical parameters and limits |
| FR-011 | US3 | T003 | missing-artifact evidence qualification |

## Success Criteria

| Criterion | Task | Evidence level |
|---|---|---|
| SC-001 | T001 | current source plus focused audit validation |
| SC-002 | T001 | current source plus focused audit validation |
| SC-003 | T001, T002 | current source; deterministic fixture coverage required/current where rerun |
| SC-004 | T001 | current source semantics; focused failure fixture required/current where rerun |
| SC-005 | T001, T002 | audit command output |
| SC-006 | T003 | historical command/table provenance; raw artifacts unavailable |
| SC-007 | T003 | document/source claim-boundary scan |

## Evidence Classification

- **Implemented-current**: behavior is present in current source.
- **Tested-current**: only commands recorded in `audit.md` during this audit.
- **Historically reported**: original smoke/campaign numbers in `spec.md`.
- **Unavailable**: original `/tmp` raw run directories and logs.
