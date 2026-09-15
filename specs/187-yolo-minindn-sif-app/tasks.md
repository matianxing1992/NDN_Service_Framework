# Tasks: YOLO MiniNDN SIF+APP Fast Path

**Spec**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Status**: PLANNED

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001 Candidate closure and pair mutation gate](#t001-candidate-closure-and-pair-mutation-gate) | PARTIAL | — | 静态门与 mutation check 通过；仍需 regular base SIF/host-gate 才能跑完整闭合序列；[b187-local-closure.md](evidence/b187-local-closure.md) | 2026-09-15 15:08 -05:00 |
| [T002 C++ YOLO selector and MiniNDN caller wiring](#t002-c-yolo-selector-and-minindn-caller-wiring) | PARTIAL | T001 | r7 STATIC_PASS；C++ target compile/link 与独立 served-provider selector 通过；through-MiniNDN 仍需 candidate-bound native config/input；[b187-local-yolo.md](evidence/b187-local-yolo.md) | 2026-09-15 15:44 -05:00 |
| [T003 Local YOLO pair build and two-run gate](#t003-local-yolo-pair-build-and-two-run-gate) | WAITING_EXTERNAL_INPUT | T002 | regular base SIF、host-gate manifest、convergence PASS 和两次 through-MiniNDN run 均未具备；17:48 外部输入复核仍未找到有效 base；[b187-local-yolo.md](evidence/b187-local-yolo.md) | 2026-09-15 17:48 -05:00 |
| [T004 TigerCluster same-candidate promotion](#t004-tigercluster-same-candidate-promotion) | WAITING_EXTERNAL_INPUT | T003 | T003 尚未 LOCAL_PASS；未启动 Slurm/Apptainer；[b187-tiger-yolo.md](evidence/b187-tiger-yolo.md) | 2026-09-15 17:48 -05:00 |
| [T005 QWEN deferral and delivery record](#t005-qwen-deferral-and-delivery-record) | DONE | — | QWEN 明确保持 TODO，未进入 YOLO candidate；[qwen-deferred.md](evidence/qwen-deferred.md) | 2026-09-15 15:16 -05:00 |
| [T006 Design-code convergence and final evidence](#t006-design-code-convergence-and-final-evidence) | DONE | T001,T002 | 静态收敛 PASS；正式 local/cluster 资格仍依赖外部 candidate 输入；[convergence-20260915-r1.md](evidence/convergence-20260915-r1.md) | 2026-09-15 15:32 -05:00 |

## Current Checkpoint

2026-09-15 17:51 -05:00：T002 的 C++ selector 已注册并接入 Spec187 native mode；阶段证据现按 request/attempt/plan 关联，并以 epochMs 核对 ACK → Selection commit → Provider accepted → Provider execution 顺序。r7 官方 review-agent 返回 STATIC_PASS；受影响目标以 `-j4` 编译通过（1m6.118s），独立 served-provider selector 通过，缺输入 selector 按预期 fail-closed。已补齐 T001–T006 的显式 FR/SC requirement coverage，分析器追踪到 12/12 FR 与 6/6 SC。真实 through-MiniNDN 请求仍需 candidate-bound config/input，T001 仍缺 regular base SIF/host-gate，T003 及后续批次保持 WAITING_EXTERNAL_INPUT/PARTIAL。

## Logical Batches

| Batch ID | Members | Stable exit | Shared selector / build | Status |
| --- | --- | --- | --- | --- |
| B187-LOCAL-CLOSURE | T001 | closure gate rejects invalid candidate inputs before side effects and accepts a verified pair tuple | existing Tiger script checks and mutation fixtures | PARTIAL |
| B187-LOCAL-YOLO | T002,T003 | two identical-candidate local C++/MiniNDN terminal YOLO runs | Spec187YoloMiniNdn; Apptainer 1.5.3 candidate | PARTIAL |
| B187-TIGER | T004 | one bounded same-candidate TigerCluster run | run-sif-app.sh and same selector | WAITING_EXTERNAL_INPUT |
| B187-DEFERRED | T005 | QWEN listed as TODO without entering candidate | docs checks | DONE |
| B187-CONVERGENCE | T006 | fresh audit PASS before formal local/cluster evidence | CodeGraph plus exact source and symbol checks | DONE |

## Task Details

### T001 Candidate closure and pair mutation gate

**Design binding**: FR-001..FR-003, FR-006, FR-011; existing prepare-development-handoff.py, build-local-sif.sh, build-sif-app.py, validate-sif-app.py and run-sif-app.sh. Preserve their path/digest ownership and add only missing composition or mutation checks.

**Requirement coverage**: FR-001, FR-002, FR-003, FR-004, FR-006, FR-011.

**Success criteria**: SC-001, SC-003, SC-005.

**Outcome**: one command sequence validates source, base, definition, APP, profile and mounts; stale symlinks, host library fallback and changed digests cause zero build/upload/run side effects.

**C++/native acceptance**: N/A for the closure gate itself; native behavior remains T002/T003.

**Risk class / Dynamic profile**: high / none; invariant is zero external side effects on rejected input.

### T002 C++ YOLO selector and MiniNDN caller wiring

**Design binding**: FR-005, FR-009, FR-012; add a registered C++ selector under tests/integration-tests and tests/wscript, and wire Experiments/NDNSF_DI_YoloAckDriven_Minindn.py only as MiniNDN/NFD/identity/process orchestration. The selector must own request/ACK/Selection/Provider/Response assertions and the Face/io_context/scheduler lifetime barrier.

**Requirement coverage**: FR-005, FR-009, FR-012.

**Success criteria**: SC-002, SC-005.

**Outcome**: a named C++ production target invokes the real DI path through the maintained YOLO case; Python-only markers cannot close the task.

Spec187 native mode requires absolute `SPEC187_NATIVE_SELECTOR`,
`SPEC187_NATIVE_REQUEST_CONFIG`, `SPEC187_NATIVE_REQUEST_INPUT` and
`SPEC187_NATIVE_REQUEST_OUTPUT` inputs. The runner validates them before
`start_network()` and launches the C++ selector as the MiniNDN User process;
there is no configurable test filter or post-run DummyClientFace substitute.

**Risk class / Dynamic profile**: high / asan-ubsan; invariant is authenticated selection, terminal result and no active owner after drain.

### T003 Local YOLO pair build and two-run gate

**Design binding**: FR-002..FR-006; reuse the four existing TigerCluster entrypoints and quickstart.md. Do not add a new base builder in this task.

**Requirement coverage**: FR-002, FR-003, FR-004, FR-005, FR-006.

**Success criteria**: SC-001, SC-002, SC-003, SC-005.

**Outcome**: local candidate SIF+APP is built or materialized from a regular base SIF and the C++ selector passes twice with the same pair identity.

**Risk class / Dynamic profile**: high / asan-ubsan; dynamic card freezes nominal, missing/changed path, invalid identity and cleanup cases.

### T004 TigerCluster same-candidate promotion

**Design binding**: FR-007..FR-009; existing run-sif-app.sh and Slurm wrapper only. No rebuild or profile/model substitution.

**Requirement coverage**: FR-007, FR-008, FR-009.

**Success criteria**: SC-004, SC-005.

**Outcome**: one bounded cluster run uses the exact LOCAL_PASS pair; scheduler and facility failures remain separate.

**Risk class / Dynamic profile**: medium / none; invariant is digest/profile equality before request.

### T005 QWEN deferral and delivery record

**Design binding**: FR-010; update only Spec187 scope/checkpoint/evidence references.

**Requirement coverage**: FR-010.

**Success criteria**: SC-006.

**Outcome**: QWEN is TODO and cannot be read by YOLO candidate or acceptance.

**Risk class / Dynamic profile**: none / none; documentation-only.

### T006 Design-code convergence and final evidence

**Design binding**: FR-011, FR-012; inspect actual production call graph, effective configuration, source/build registration, C++ selector and evidence paths with CodeGraph and exact source checks.

**Requirement coverage**: FR-011, FR-012.

**Success criteria**: SC-005.

**Outcome**: severity-classified convergence report returns PASS; any controlling gap creates a repair task before T003/T004 formal validation.

**Risk class / Dynamic profile**: high / none; invariant is requirement-to-production-path-to-evidence agreement.

## Batch Quality Record

| Batch ID | Coverage matrix | Static findings | Compile/build misses | Runtime/test misses | Dynamic validation | Build scope / target / -j / elapsed / exit | Review trace / closure decision | Behavior result | Evidence / remaining |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| B187-LOCAL-CLOSURE | production callers, implementation, tests, build, migration: `build-sif-app.py`, `test_sif_app.py`; evidence lane in [b187-local-closure.md](evidence/b187-local-closure.md) | no P0–P3; STATIC_PASS | not run; regular base unavailable | focused offline checks: 3 passed; SIF/runtime not observed | NOT_RUN; dynamic card awaits regular base | not run | review-agent STATIC_PASS on frozen diff; OPEN_FOR_NEXT_BATCH | PARTIAL | regular base SIF and host-gate manifest remain |
| B187-LOCAL-YOLO | production callers, implementation, state/lifecycle, build/source closure, tests/evidence: [b187-local-yolo.md](evidence/b187-local-yolo.md) | no P0-P2 after r7 review; token/JSON boundaries and epoch order checked | target compile/link passed; first compile miss and unpreloaded loader boundary are retained | independent C++ served-provider selector passed; through-MiniNDN selector failed closed on missing config before Runtime open | NOT_RUN for real MiniNDN/SIF | `spec187-yolo-minindn`, `build-spec185-b0c-normal`, `-j4`, 1m6.118s, rc=0 | review-agent r7 STATIC_PASS; OPEN_FOR_NEXT_BATCH | PARTIAL | regular base SIF, native requester config/input, convergence and two local runs remain |
| B187-TIGER | no execution because T003 has no LOCAL_PASS; [b187-tiger-yolo.md](evidence/b187-tiger-yolo.md) | N/A before local gate | not run | not run | NOT_RUN | not run | review-agent N/A; BLOCKED_BY_LOCAL_GATE | WAITING_EXTERNAL_INPUT | T003 LOCAL_PASS and external TigerCluster access remain |
| B187-DEFERRED | documentation lane covered; other lanes N/A by scope | N/A by docs-only scope | N/A | N/A | N/A | N/A | review-agent N/A; CLOSED_FOR_VALIDATION | DONE | QWEN remains TODO; [qwen-deferred.md](evidence/qwen-deferred.md) |
| B187-CONVERGENCE | production/callers, implementation, state/lifecycle, build/source closure, evidence: [convergence-20260915-r1.md](evidence/convergence-20260915-r1.md) | no P0-P2 after r7 review | target compile/link passed | missing-input selector fail-closed; real MiniNDN/SIF not observed | NOT_RUN for formal qualification | build boundary recorded in B187-LOCAL-YOLO | review-agent r7 STATIC_PASS; CLOSED_FOR_VALIDATION | DONE (static) | external candidate inputs and formal runs remain |

### Batch Retrospective

- static: T001 and T002 review-agent gates are STATIC_PASS; r3/r4 found the output-collision and marker-correlation issues, r6/r7 confirmed their repairs.
- compile/link: `spec187-yolo-minindn` linked successfully in the existing DI build tree with `-j4` after retaining the first `CollaborationPlan` digest compile miss.
- runtime/test: focused Python checks and the independent C++ served-provider selector passed; the through-MiniNDN selector has a correlated stage oracle but only a fail-closed missing-input run so far.
- unobserved: regular base SIF, host-gate manifest, candidate SIF/APP, candidate-bound C++ requester config/input, two real MiniNDN runs, and cluster run.

## Dependencies & Execution Order

T001 → T002 → T006 → T003 → T004. T005 is independent documentation work and must not add a dependency to the YOLO path. Each code task receives its own static review before the batch composition review; T003 and T004 cannot start without convergence PASS and the preceding acceptance result.
