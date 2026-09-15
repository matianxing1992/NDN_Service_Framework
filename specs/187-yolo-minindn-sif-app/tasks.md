# Tasks: YOLO MiniNDN SIF+APP Fast Path

**Spec**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Status**: PLANNED

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001 Candidate closure and pair mutation gate](#t001-candidate-closure-and-pair-mutation-gate) | PARTIAL | — | 静态门与 mutation check 通过；仍需 regular base SIF/host-gate 才能跑完整闭合序列；[b187-local-closure.md](evidence/b187-local-closure.md) | 2026-09-15 15:08 -05:00 |
| [T002 C++ YOLO selector and MiniNDN caller wiring](#t002-c-yolo-selector-and-minindn-caller-wiring) | NOT_STARTED | T001 | 需 selector/build registration；未运行 | 2026-09-15 14:53 -05:00 |
| [T003 Local YOLO pair build and two-run gate](#t003-local-yolo-pair-build-and-two-run-gate) | NOT_STARTED | T002 | 需 convergence PASS、base SIF；未运行 | 2026-09-15 14:53 -05:00 |
| [T004 TigerCluster same-candidate promotion](#t004-tigercluster-same-candidate-promotion) | NOT_STARTED | T003 | 仅 LOCAL_PASS 后执行；未运行 | 2026-09-15 14:53 -05:00 |
| [T005 QWEN deferral and delivery record](#t005-qwen-deferral-and-delivery-record) | DONE | — | QWEN 明确保持 TODO，未进入 YOLO candidate；[qwen-deferred.md](evidence/qwen-deferred.md) | 2026-09-15 15:16 -05:00 |
| [T006 Design-code convergence and final evidence](#t006-design-code-convergence-and-final-evidence) | NOT_STARTED | T001,T002 | 必须先于 T003/T004 正式验收 | 2026-09-15 14:53 -05:00 |

## Current Checkpoint

2026-09-15 15:08 -05:00：T001 的 changed-base 零副作用门已实现；定向 Python 检查 3 passed，冻结只读 review-agent 返回 STATIC_PASS。regular base SIF 不存在，历史 images/spec180-runtime-r119.sif 为 dangling symlink；因此 T001 保持 PARTIAL，完整 handoff/render/build 闭合待外部 base 与 host-gate 输入。下一步继续 T002 的 C++ selector 接线。

## Logical Batches

| Batch ID | Members | Stable exit | Shared selector / build | Status |
| --- | --- | --- | --- | --- |
| B187-LOCAL-CLOSURE | T001 | closure gate rejects invalid candidate inputs before side effects and accepts a verified pair tuple | existing Tiger script checks and mutation fixtures | PARTIAL |
| B187-LOCAL-YOLO | T002,T003 | two identical-candidate local C++/MiniNDN terminal YOLO runs | Spec187YoloMiniNdn; Apptainer 1.5.3 candidate | NOT_STARTED |
| B187-TIGER | T004 | one bounded same-candidate TigerCluster run | run-sif-app.sh and same selector | NOT_STARTED |
| B187-DEFERRED | T005 | QWEN listed as TODO without entering candidate | docs checks | DONE |
| B187-CONVERGENCE | T006 | fresh audit PASS before formal local/cluster evidence | CodeGraph plus exact source and symbol checks | NOT_STARTED |

## Task Details

### T001 Candidate closure and pair mutation gate

**Design binding**: FR-001..FR-003, FR-006, FR-011; existing prepare-development-handoff.py, build-local-sif.sh, build-sif-app.py, validate-sif-app.py and run-sif-app.sh. Preserve their path/digest ownership and add only missing composition or mutation checks.

**Outcome**: one command sequence validates source, base, definition, APP, profile and mounts; stale symlinks, host library fallback and changed digests cause zero build/upload/run side effects.

**C++/native acceptance**: N/A for the closure gate itself; native behavior remains T002/T003.

**Risk class / Dynamic profile**: high / none; invariant is zero external side effects on rejected input.

### T002 C++ YOLO selector and MiniNDN caller wiring

**Design binding**: FR-005, FR-009, FR-012; add a registered C++ selector under tests/integration-tests and tests/wscript, and wire Experiments/NDNSF_DI_YoloAckDriven_Minindn.py only as MiniNDN/NFD/identity/process orchestration. The selector must own request/ACK/Selection/Provider/Response assertions and the Face/io_context/scheduler lifetime barrier.

**Outcome**: a named C++ production target invokes the real DI path through the maintained YOLO case; Python-only markers cannot close the task.

**Risk class / Dynamic profile**: high / asan-ubsan; invariant is authenticated selection, terminal result and no active owner after drain.

### T003 Local YOLO pair build and two-run gate

**Design binding**: FR-002..FR-006; reuse the four existing TigerCluster entrypoints and quickstart.md. Do not add a new base builder in this task.

**Outcome**: local candidate SIF+APP is built or materialized from a regular base SIF and the C++ selector passes twice with the same pair identity.

**Risk class / Dynamic profile**: high / asan-ubsan; dynamic card freezes nominal, missing/changed path, invalid identity and cleanup cases.

### T004 TigerCluster same-candidate promotion

**Design binding**: FR-007..FR-009; existing run-sif-app.sh and Slurm wrapper only. No rebuild or profile/model substitution.

**Outcome**: one bounded cluster run uses the exact LOCAL_PASS pair; scheduler and facility failures remain separate.

**Risk class / Dynamic profile**: medium / none; invariant is digest/profile equality before request.

### T005 QWEN deferral and delivery record

**Design binding**: FR-010; update only Spec187 scope/checkpoint/evidence references.

**Outcome**: QWEN is TODO and cannot be read by YOLO candidate or acceptance.

**Risk class / Dynamic profile**: none / none; documentation-only.

### T006 Design-code convergence and final evidence

**Design binding**: FR-011, FR-012; inspect actual production call graph, effective configuration, source/build registration, C++ selector and evidence paths with CodeGraph and exact source checks.

**Outcome**: severity-classified convergence report returns PASS; any controlling gap creates a repair task before T003/T004 formal validation.

**Risk class / Dynamic profile**: high / none; invariant is requirement-to-production-path-to-evidence agreement.

## Batch Quality Record

| Batch ID | Coverage matrix | Static findings | Compile/build misses | Runtime/test misses | Dynamic validation | Build scope / target / -j / elapsed / exit | Review trace / closure decision | Behavior result | Evidence / remaining |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| B187-LOCAL-CLOSURE | production callers, implementation, tests, build, migration: `build-sif-app.py`, `test_sif_app.py`; evidence lane in [b187-local-closure.md](evidence/b187-local-closure.md) | no P0–P3; STATIC_PASS | not run; regular base unavailable | focused offline checks: 3 passed; SIF/runtime not observed | NOT_RUN; dynamic card awaits regular base | not run | review-agent STATIC_PASS on frozen diff; OPEN_FOR_NEXT_BATCH | PARTIAL | regular base SIF and host-gate manifest remain |
| B187-LOCAL-YOLO | planned in T002/T003 | not observed | not run | not run | NOT_RUN | not run | review-agent not run; OPEN_FOR_NEXT_BATCH | PLANNED | evidence/b187-local-yolo.md to be created |
| B187-TIGER | planned in T004 | not observed | not run | not run | NOT_RUN | not run | review-agent not run; OPEN_FOR_NEXT_BATCH | PLANNED | evidence/b187-tiger-yolo.md to be created |
| B187-DEFERRED | documentation lane covered; other lanes N/A by scope | N/A by docs-only scope | N/A | N/A | N/A | N/A | review-agent N/A; CLOSED_FOR_VALIDATION | DONE | QWEN remains TODO; [qwen-deferred.md](evidence/qwen-deferred.md) |
| B187-CONVERGENCE | planned in T006 | not observed | not run | not run | NOT_RUN | not run | review-agent not run; OPEN_FOR_NEXT_BATCH | PLANNED | convergence report pending |

### Batch Retrospective

- static: T001 review-agent STATIC_PASS; no P0–P3 findings.
- compile/link: not run; selector source closure and target registration are pending.
- runtime/test: focused Python checks passed, but no SIF, MiniNDN or Tiger run has started.
- unobserved: regular base SIF, host-gate manifest, local pair build, C++ selector, local YOLO run and cluster run.

## Dependencies & Execution Order

T001 → T002 → T006 → T003 → T004. T005 is independent documentation work and must not add a dependency to the YOLO path. Each code task receives its own static review before the batch composition review; T003 and T004 cannot start without convergence PASS and the preceding acceptance result.
