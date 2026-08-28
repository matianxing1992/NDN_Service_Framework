# Final Traceability: UAV Sensor Stream Generality

**Closure**: COMPLETE / MEASURED NEGATIVE  
**Post-implementation audit**: BLOCK  
**Formal result**: `results/spec144-uav-sensor-stream-20260724T165132Z`

## Functional Requirements

| Requirement | Status | Exact evidence |
|---|---|---|
| FR-001 | PASS | T001/T009 Spec 127/128 definitions and canonical-result roots match; no historical runner invoked |
| FR-002 | PARTIAL | T004/T005 use Mapping v2 announce/prepare/publish and adaptive exact-name consumption, but FR-011 protection is absent |
| FR-003 | PASS | all 16 telemetry cells attempted/produced exactly 1,200 measured 20 Hz samples with the 256/384/512-byte cycle |
| FR-004 | PASS | telemetry deterministic tests and formal definitions use one source, zero repair |
| FR-005 | PASS | compact state fields and monotonic admission tested; existing Drone/Ground Station `GetStatus` containers unchanged |
| FR-006 | PASS | all 16 acoustic cells attempted/produced 1,500 measured 40 ms blocks with 2/3/4 sources |
| FR-007 | PASS | exact 512-byte boundary and generic two-repair deterministic cases pass |
| FR-008 | PASS | codec/capture/playout/interpretation remain APP concerns; Core receives opaque bytes |
| FR-009 | PASS | both consumers use Latest plus actual Core `AdaptiveSampleAtomic`; cell summaries record Core fetch decisions |
| FR-010 | PASS | 127-case security contract plus wrong provider/session/name/kind/extent deterministic tests pass |
| FR-011 | **FAIL** | CodeGraph proves `makeSources` bytes go to signed Data without encryption; see N-001 |
| FR-012 | PASS | all cell summaries report attempted/produced/delivered, admission errors, mean/p50/p95/p99/max, and gap |
| FR-013 | PASS | all cell summaries report Mapping/Data/novelty, Payload initial/retry, and future Interest/hit counters |
| FR-014 | PARTIAL | raw retry/timeout/Nack/late/deadline/exhaustion/recovery counters exist, but derived recovery `successRate` has mismatched units |
| FR-015 | PASS | source admission/consumed repair, protection-only repair, nonproductive, and unresolved outcomes conserve in the terminal ledger |
| FR-016 | PASS | one zero-loss and five each loss/reorder/combined cells per workload, all with 60-second measured windows |
| FR-017 | PASS | 32 unique paths/receipts, 32 terminal cells, one invocation each, no automatic retry |
| FR-018 | PASS | separate preflight paths; frozen formal source/binary/config/threshold/analyzer identities |
| FR-019 | PASS | CodeGraph plus executable-selector scan found zero prohibited Core/binding branches |
| FR-020 | PASS | pre-formal generic test timing defect was fixed/tested/audited; post-formal issues preserved |
| FR-021 | PASS | build, forced binding rebuild, 375 native, 50 stress, 19/27/8/8 Python, security, preflight, and audit gates passed before formal start |
| FR-022 | PASS | independent telemetry/acoustic verdicts are negative and shared generality is withheld |

## Success Criteria

| Criterion | Status | Exact evidence |
|---|---|---|
| SC-001 | **FAIL** | deterministic admission checks pass, but FR-011 confidentiality does not |
| SC-002 | PASS | telemetry zero-loss 1200/1200; p95 16.504 ms; p99 18.449 ms; gap 77.086 ms |
| SC-003 | **FAIL** | loss 5/5 and reorder 4/5 pass, but combined is 2/5 |
| SC-004 | PASS | acoustic zero-loss 1500/1500; p95 31.518 ms; p99 47.826 ms; gap 85.796 ms |
| SC-005 | **FAIL** | acoustic loss/reorder/combined are each 0/5 |
| SC-006 | PASS | every accepted cell reports provider-confirmed future hit 100% |
| SC-007 | PASS | every accepted cell has 100% Mapping novelty and meets its nonproductive-Interest gate |
| SC-008 | PASS | 32 declared/terminal/single-invocation unique cells; no retries or replacements |
| SC-009 | **FAIL** | raw ledgers are retained, but the frozen recovery success ratio is semantically invalid |
| SC-010 | PASS | zero Core/binding workload-semantic selectors |
| SC-011 | PASS | four Spec/result 127/128 roots unchanged; no historical runner invoked |
| SC-012 | PASS | both family verdicts false; no shared generality claim made |

## Task and Evidence Closure

| Task | Status | Evidence |
|---|---|---|
| T001 | complete | `evidence/baseline-manifest.md`, `evidence/pre-implementation-audit.md` |
| T002 | complete | `evidence/metric-contract.md`; native/Python metric suites |
| T003 | complete | `evidence/runner-contract.md`; runner suite 8/8 |
| T004 | complete with post-audit security finding | `evidence/telemetry-deterministic.md`; N-001 |
| T005 | complete with post-audit security finding | `evidence/acoustic-deterministic.md`; N-001 |
| T006 | complete | `evidence/preflight-and-freeze.md` |
| T007 | complete, negative | `evidence/telemetry-formal.md`; telemetry family FAIL |
| T008 | complete, negative | `evidence/acoustic-formal.md`; acoustic family FAIL |
| T009 | complete, audit BLOCK | `evidence/neutrality-audit.md`, `audit.md`, `completion-summary.md` |

## Formal Cell Accounting

| Workload/profile | Accepted | Attempted | Required | Exact 95% interval | Treatment |
|---|---:|---:|---:|---:|---|
| telemetry/zero-loss | 1 | 1 | 1 | [0.025000, 1.000000] | PASS |
| telemetry/loss | 5 | 5 | 4 | [0.478176, 1.000000] | PASS |
| telemetry/reorder | 4 | 5 | 4 | [0.283582, 0.994949] | PASS |
| telemetry/combined | 2 | 5 | 4 | [0.052745, 0.853367] | FAIL |
| acoustic/zero-loss | 1 | 1 | 1 | [0.025000, 1.000000] | PASS |
| acoustic/loss | 0 | 5 | 4 | [0.000000, 0.521824] | FAIL |
| acoustic/reorder | 0 | 5 | 4 | [0.000000, 0.521824] | FAIL |
| acoustic/combined | 0 | 5 | 4 | [0.000000, 0.521824] | FAIL |

## Frozen Integrity

| Artifact | SHA-256 |
|---|---|
| Spec 144 campaign summary | `01e95d9e79ccf879829a02d981d451e1cd35582aca86b57944330b2cdc2df738` |
| Spec 144 campaign manifest | `5f980d857fb66f70e1939e408f35c780b14d4c202aad226d844b469da0f6a517` |
| Spec 144 campaign CSV | `c9fd955a7a5880888c91608ebfe3566d44501f3e280c4e9918c2bc0de1052a96` |
| Spec 127 definition root | `6756fba7fad110f267863f4bb9f5cb1abec5c68515642ba383235a6a6729136d` |
| Spec 128 definition root | `6f532069382d0dc6b3d853c3ab9251e71993e985ae98a1a57c955d719e03faee` |
| Spec 127 result root | `ef35eb227db611253afe99d312e3eaa18e8c7123898570815cde8756ffd96787` |
| Spec 128 result root | `0a47cc3da6862f261d687615344dd8e35865e53556c28d31b1758a5eda876c1d` |
| Spec 145 campaign summary | `8bad7f90d12647f2904e208000e189ca73a5369ebb33f359a8f07a5560ca2566` |
| Spec 145 reference run | `6d842b6b538ef112a2d0a961fda129d950a9e546769a07974049841bd8055243` |

The frozen Spec 144 campaign must not be rerun or reinterpreted. Open findings
belong to a new Spec.
