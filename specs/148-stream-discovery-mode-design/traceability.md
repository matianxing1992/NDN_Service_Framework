# Requirement Traceability: Predictive Stream API Replacement

| Requirement | Tasks | Acceptance |
|---|---|---|
| FR-001 sole `start()` | T003-T005, T013 | exact descriptor; old overload absent |
| FR-002 validated `push()` | T003, T006, T008 | authority/name/epoch/signature/budget tests |
| FR-003 exact signed wire | T006, T019 | byte hash equal; zero Core source signing |
| FR-004 atomic `flush()` | T007-T008 | group/frontier/concurrency tests |
| FR-005 complete repair metadata | T007, T011 | unequal-length recovery |
| FR-006 descriptor/checkpoint | T003, T005, T013 | C++/Python parity |
| FR-007 consumer validation | T010 | validator and fail-closed tests |
| FR-008 generic adaptive prefetch | T009, T021 | reuse proof; forbidden-branch scan |
| FR-009 repair/retry/skip | T011-T012 | ordering and counters |
| FR-010 concurrency/stop | T008 | race and post-stop tests |
| FR-011 language parity | T013-T014 | identical surfaces/defaults/errors |
| FR-012 old API removed | T001-T004, T013-T014 | negative compile/import/source gates |
| FR-013 all callers migrated | T001, T014-T019 | full build and tests |
| FR-014 frozen evidence protected | T020-T021 | scoped diff/result audit |
| FR-015 binary rollback | T020 | pinned pre-change hash/manifest |
| FR-016 real UAV path | T015-T018 | two-endpoint path evidence |
| FR-017 fresh MiniNDN evidence | T018-T020 | new result directory and complete metrics |
| FR-018 matched comparison | T020 | immutable hashes and matched config |
| FR-019 real processes/topology | T018, T020 | controller+GS on memphis; Drone on ucla |
| FR-020 evidence levels | T018-T020 | preflight/smoke/formal clearly separated |
| FR-021 no new-binary dual path | T017, T019-T021 | selector/API absence and pinned old subject |
| FR-022 endpoint runtime proof | T015-T018, T020 | structured provider/consumer markers |
| FR-023 two formal cells/artifacts | T020-T021 | zero-loss + fixed impairment manifests |

| Success criterion | Tasks |
|---|---|
| SC-001 replacement surface only | T002, T004, T013-T014 |
| SC-002 exact source wire | T006, T019 |
| SC-003 edge/recovery correctness | T007-T011, T019 |
| SC-004 full build/regressions | T014, T019 |
| SC-005 UAV predictive smoke | T015-T018, T020 |
| SC-006 complete metrics | T012, T020 |
| SC-007 admissible old/new claim | T020-T021 |
| SC-008 real UAV smoke | T015-T018, T020 |
| SC-009 two complete formal cells | T020-T021 |

## Post-implementation status

| Criterion | Status | Evidence |
|---|---|---|
| SC-001 | PASS | removed-surface tests and `evidence/source-scan.log` |
| SC-002 | PASS | exact-wire unit tests and both formal-cell wire hashes |
| SC-003 | PASS at unit level | full native suite passes; impaired system-level recovery remains blocked under SC-009 |
| SC-004 | BLOCK | native suite passes; full Python suite reports 2 failures and 10 errors |
| SC-005 | BLOCK | zero-loss UAV path passes; impaired path delivers only 1.424% |
| SC-006 | PASS | both cell summaries contain the required metric families |
| SC-007 | N/A | no old/new performance-superiority claim was made |
| SC-008 | PASS | real controller, Drone, and Ground Station ran on the contracted MiniNDN nodes |
| SC-009 | BLOCK | zero-loss cell passes; immutable light-loss/reorder cell fails |

Canonical evidence:
`results/spec148-predictive-uav-formal-20260726T034348Z/`.
