# Spec180 Audit Evidence — Iteration 69

**Date**: 2026-09-03  
**Subject**: current `Experimental` worktree, before T011 implementation  
**Mode**: design/code checkpoint; not T014 convergence and not qualification

## Verdict

**CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED**.

The document and contract gates are usable for continued implementation, but
the candidate is not qualified. The real NFD/NDN-SVS ACK-to-Response driver is
still unwired, and the findings below must be closed before T014 can return
`PASS`.

## Reproducible checks

| Check | Result |
|---|---|
| `python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py specs/180-ack-driven-cross-model-qualification --strict` | PASS; 25 FR, 9 SC, 4 user stories, 20 tasks |
| `python3 scripts/spec180_contract_gate.py --project-root . --feature-dir specs/180-ack-driven-cross-model-qualification` | PASS; `contractReady=true`, `qualificationReady=false`, no issues |
| combined runner/local-gate/inventory/contract focused slice | 37 passed |
| current `tests/python/test_spec180_*.py` collection | 122 passed, 19 existing exporter/runtime warnings |
| `python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-A` after preflight | fails closed with `ACK_DRIVEN_MININDN_DRIVER_NOT_WIRED`; no protocol process or PASS marker |
| `scripts/run_spec180_case.py` in the repository | absent; `run-functional.sh` therefore correctly fails closed before remote execution |

The focused and full Python counts are implementation evidence only. No live
MiniNDN, exact-SIF, Slurm, or Tiger result is attached to this candidate.

## Current findings

| ID | Severity | Evidence | Required correction / owner |
|---|---|---|---|
| A180-102 | HIGH | `tools/ndnsf-di/export_spec180_yolo26_onnx.py:317-325` derives cuts from `int(len(node_names) * 0.55)` and fixed indexes; `adapters/yolo/adapter.py:62-85` repeats the same partition and checks only boundary names | Replace index/percentile derivation with signed semantic node sets, branch ownership, tensor interfaces, dependency edges, and full-model equivalence evidence. Reject heuristic-only cuts. T004/T005. |
| A180-103 | HIGH | `packaging/ndnsf-di-container/jobs/spec180/run-functional.sh:56-63` probes `/bundle/scripts/run_spec180_case.py`; that file is absent while the maintained local runner is `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | Create and test one thin T011-owned in-image dispatcher, or keep the remote gate fail-closed. It must route to maintained YOLO/Qwen entrypoints without a second protocol implementation or ambient paths. T011/T013. |
| A180-104 | HIGH | `scripts/validate_spec180_results.py:48-86` accepts `protocolOracle`, `resultOracle`, `runtimeOracle`, `redaction`, and `cleanup` as strings; a bounded minimal manifest containing only those labels returned `True` | Require structured candidate-bound evidence records with relative paths, SHA-256 digests, schemas, lifecycle/request data, runtime/device data, child exits, and cleanup details. Reject label-only manifests. T013/T020. |
| A180-105 | MEDIUM | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:560-579` checks secret-like names/values but accepts a generic `payload` field; a bounded generic-payload probe succeeded | Enforce milestone-specific allowed fields and recursive non-secret scalar validation; reject payload/content/token/byte/credential fields regardless of value spelling. Add focused negative coverage. T011/T014. |
| A180-106 | MEDIUM | FR-003 and the preflight case-plan both mention catalogue validation before ACK while also saying candidate enumeration starts after ACK | Clarify that only opaque signed revision/digest metadata may be retained before `ACK_CLOSED`; candidate records, requirements, feasibility, and Provider binding remain unavailable until the authenticated ACK snapshot closes. No runtime change is implied. T002/T005/T011. |

## Gate decision

The revised requirements, plan, tasks, contracts, and traceability now name the
owners and closing regressions for every finding. T011 remains the controlling
implementation blocker; T014 must rerun the code-aware convergence audit after
the driver, semantic cut validation, strict lifecycle schema, dispatcher, and
structured result validator are implemented. Only then may T015 run the full
local inventory, followed by the single SIF and Tiger gates.
