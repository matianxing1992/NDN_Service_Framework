# Traceability: Real DI Validation Gates

## Requirements to implementation and evidence

| Requirement | Owner | Acceptance |
|---|---|---|
| FR-001 | T001 | versioned fidelity fixture |
| FR-002 | T001 | required-field matrix |
| FR-003 | T001 | immutable model identity rejection |
| FR-004 | T001, T002 | malformed/stale/cross-run aggregate matrix |
| FR-005 | T002 | mandatory skip/unavailable/timeout failures |
| FR-006 | T002 | lower-tier non-substitution |
| FR-007 | T002, T009 | default Gate A-D inventory |
| FR-008 | T002, T009 | remote authorization stays false on any failure |
| FR-009 | T006 | real MiniNDN process/topology evidence |
| FR-010 | T006, T007 | real Qwen artifact and no-fake admission |
| FR-011 | T006 | pinned local-only model preflight |
| FR-012 | T006 | two prompt records |
| FR-013 | T006 | two warmups and six measured records |
| FR-014 | T006, T007 | minimum token-event admission |
| FR-015 | T006, T007 | answer and timing completeness |
| FR-016 | T006, T007 | failures retained in distributions |
| FR-017 | T006, T007 | backend/placement/fallback proof |
| FR-018 | T005, T006 | User-created canonical request ID |
| FR-019 | T005, T006 | attempt/plan/model admission bindings |
| FR-020 | T005, T006 | ordered lineage record |
| FR-021 | T005, T007 | negative lineage matrix |
| FR-022 | T008 | same workload digest in container |
| FR-023 | T008 | image/resources/OOM/backend evidence |
| FR-024 | T002, T008 | startup checks remain lower tier |
| FR-025 | T003, T004 | idle and hard deadline state |
| FR-026 | T003, T004 | valid-progress-only renewal |
| FR-027 | T003, T004 | complete progress binding |
| FR-028 | T003, T004 | non-renewal negative matrix |
| FR-029 | T003 | distinct STALLED/HARD_TIMEOUT outcomes |
| FR-030 | T003 | first-terminal-wins races |
| FR-031 | T003 | injected-clock tests without sleeps |
| FR-032 | T004 | NDNSF/NDNSF-DI ownership review |
| FR-033 | T001, T006, T008, T009 | reproducibility manifest |
| FR-034 | T002, T009 | identical JSON/Markdown accounting |
| FR-035 | T002, T009 | no TigerCluster submission |
| FR-036 | T010 | strict real-model MiniNDN profile, role and stage evidence |
| FR-037 | T011 | content-addressed hard-link bundle and run-local retention manifest |

## Success criteria to closure

| Criterion | Owner | Closure evidence |
|---|---|---|
| SC-001 | T001, T002, T009 | aggregate negative matrix |
| SC-002 | T006, T007 | accepted MiniNDN run counts |
| SC-003 | T006, T007 | measurement completeness validator |
| SC-004 | T005, T006, T007 | accepted lineage report |
| SC-005 | T005 | lineage negative matrix |
| SC-006 | T008 | candidate-container workload comparison |
| SC-007 | T003 | deterministic deadline matrix |
| SC-008 | T003, T004 | renewal/hard-cap boundary evidence |
| SC-009 | T001, T002, T007, T008 | non-substitution tests |
| SC-010 | T002, T009 | remote authorization boundary |
| SC-011 | T011 | zero duplicate payload bytes and source-run-independent reuse |

## Closure Evidence

| Tasks | Evidence |
|---|---|
| T001–T005 | `evidence/t001-fidelity-contract.md` through `evidence/t005-lineage.md` |
| T006–T008 | `evidence/t006-qwen3-minindn.md` through `evidence/t008-container.md` |
| T009 | `evidence/t009-local-closure.md`; historical canonical run `results/spec165-local-gates/20260731T052926Z-5debf140` |
| T010 | `evidence/t010-minindn-first-strict-recheck-20260801.md`; Gate-B runs `results/spec165-minindn-first/20260801T204709Z-28c987c4` and `results/spec165-minindn-first-static/20260801T205214Z-e2281bb3`; current full aggregate `results/spec165-minindn-first-full-cpu/20260801T213143Z-1d7b91c8` |
| T011 | `evidence/failure-and-retention-audit-20260801.md`; bundle `results/_artifacts/qwen-stage-bundles/sha256/ab0695659d5d0a589d89349737d214d2d06b1c3697e013d4409a6a376c41358e` |

The historical canonical aggregate binds all 35 requirements and 10 success criteria to
one source revision, model digest, workload digest, candidate image, and four
passing mandatory Gate records. It does not contain TigerCluster evidence.

The current strict MiniNDN-first recheck adds FR-036. The current-source
aggregate now passes all four mandatory gates with the CPU-compatible candidate
image and sets `externalValidationAuthorized=true`; it still contains no
TigerCluster evidence and does not submit external work.

The retention extension adds FR-037 and SC-011. It changes artifact ownership
and storage only; it does not replace or rerun any Gate A-D evidence.

No requirement or success criterion is intentionally deferred. TigerCluster
execution is excluded by FR-035; only its future eligibility signal is in
scope.
