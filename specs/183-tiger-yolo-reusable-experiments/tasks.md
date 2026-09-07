# Tasks: Reusable TigerCluster YOLO Distributed Inference

**Input**: [spec.md](spec.md), [plan.md](plan.md), [profile contract](contracts/experiment-profile.md), [validation matrix](validation-matrix.md)
**Branch**: `TigerClusterExperiments`
**Status**: 0/17 implementation/qualification tasks complete; PLANNED, no runtime PASS.

任务按行为闭环组织，每项包含必要失败例→实现→聚焦回归→证据，不拆成“写测试/改代码/跑命令”三个机械任务。正式 broad suites/实验必须等 T007；focused red/green 可在实现中执行。所有 Tiger 文件以下用完整仓库相对路径。

## Phase 1: Setup

- [ ] T001 Freeze receiving inputs and unresolved inventory in `Experiments/TigerCluster/docs/yolo-reusable.md` and `specs/183-tiger-yolo-reusable-experiments/evidence/input-inventory.md`: reconcile four repository revisions against `Experiments/TigerCluster/development-handoff.lock.json`, delivery-vs-runtime source, nine native outputs, actual public YOLO APIs, model/package/oracle hashes, local/compute Apptainer and proposed partition/GPU/resources/capacity. Register exact build and test selectors and expected errors from current code; keep missing items WAITING_EXTERNAL_INPUT and historical failures intact. No large build/download or model job. Read-only local inspection first; bounded substrate probe only under the reviewed exception in plan.md.

## Phase 2: Foundational

- [ ] T002 Implement phase-specific candidate closure in `Experiments/TigerCluster/runtime/yolo_profile.py` and `Experiments/TigerCluster/tests/test_yolo_closure.py`: test wrong/changed hash, incomplete inventory, path escape, stale receipt, unbound script/env and stage bypass before implementation; enforce I/R/E identities and invalidation with no self-reference, inputs-before-build/runtime-before-execution/dispatch-before-upload gates. Prove rejected inputs make zero build/upload/staging/sbatch calls with spies at the real command boundary. Depends on T001 interface inventory; gates may report missing physical input without pretending it is present.

## Phase 3: User Story 1 - Reusable Launch Configuration

**Independent Test**: deterministic resolved config from another cwd; fail-before-side-effects; two concurrent submit attempts cannot both launch.

- [ ] T003 [US1] Extend reusable role/container lifecycle in `Experiments/TigerCluster/runtime/baseline.py`, `identities.py`, and planned `yolo_worker.py`, with `Experiments/TigerCluster/tests/test_yolo_runtime.py`: preserve CPU baseline behavior; support explicit GPU/env/mount/cwd mapping, isolated role identities/PIB, owned process groups and bounded peer/readiness checks. Add real short-lived parent/child cleanup tests, wrong cwd/port/shared PIB rejection and no host runtime injection. Reuse common primitives, not a copied supervisor. Depends on T002.
- [ ] T004 [US1] Deliver strict profile and one entrypoint in `Experiments/TigerCluster/profiles/yolo-two-node.json`, `schemas/tiger-yolo-v1.schema.json`, `jobs/yolo/submit.py`, `jobs/yolo/run.sbatch`, and `tests/test_yolo_submit.py`: expose the five contract commands, resolve every field into actual argv/env, reject leftovers, bind immutable bundle, stage-scoped gates and allocate-once journal. Test atomic duplicate-run prevention and SUBMISSION_UNKNOWN recovery without resubmit; register case differences and walltime/timeout budgets. Fill real operational values before enabling submit; do not modify old `profiles/two-node.json`. Depends on T003.

## Phase 4: User Story 2 - Current YOLO Path And Independent Verdict

**Independent Test**: existing application APIs and independent oracle exercised by focused tests; no copying an expected tensor into the distributed output.

- [ ] T005 [US2] Wire the existing secure YOLO application into `Experiments/TigerCluster/apps/yolo.py` and `runtime/yolo_worker.py`, with `tests/test_yolo_application.py`: reuse `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` and installed native Provider interfaces; map the four roles without fabricating ACK/Selection, verify permissions/current status and direct predecessor Data, enforce native input and independent oracle contract. Focused tests must catch lost structured assignment, stale epoch/input, missing backbone-to-head Data, duplicate/late output, and forbidden shared-filesystem activation shortcuts. Any Core/DI repair belongs to its existing source/tests and updates candidate provenance. Depends on T004.
- [ ] T006 [US2] Implement authoritative collection and failure oracles in `Experiments/TigerCluster/runtime/yolo_result.py` and `tests/test_yolo_result.py`: associate every request/attempt/plan/role/node/GPU/edge digest, validate numeric output via the existing contract, require all finite child exit0 plus controlled service shutdown/reap, and separate local/single-node/two-node/expected-rejection verdicts. Reject stale/forged PASS, missing role/response/CUDA, shape/class/tolerance mismatch, timeout-as-auth-rejection, nonzero worker, forced cleanup or corrupt output. Retain first failure and immutable reanalysis. Depends on T005.

## Phase 5: Design-Code Convergence

- [ ] T007 Audit production wiring before formal validation in `specs/183-tiger-yolo-reusable-experiments/evidence/design-code-convergence.md`: compare accepted spec/contract to actual submit→worker→application→Core/DI/Repo→collector and all effective fields through CodeGraph and exact source; register severity, owner and focused regression for each discrepancy. Review all wrappers/helpers and local/remote paths once as a closure, including scripts used by later substrate probes. Close only on PASS with zero controlling semantic/security/wiring/evidence gaps; actual changes reopen this task. Depends on T002–T006. This task is not satisfied by the planning audit.

## Phase 6: User Story 2 - Formal Local Qualification

**Independent Test**: current dependency build, real CPU multi-process graph and same-SIF app path. Record one evidence receipt per independently meaningful gate.

- [ ] T008 [US2] Build and qualify the locked dependency/native/Python closure using existing build owners and `Experiments/TigerCluster/docs/yolo-reusable.md`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/host-unit.md`: clean ABI consumers in isolated build roots, system toolchain/Boost, at most `-j2` total per active build tree; record both Python extension imports, actual entrypoint checks, ldd/readelf/loaded hashes and all selected dependency/NDNSF/Tiger unit results. Preserve build failures; no stale incremental objects or manual PASS manifests. Depends on T007 and complete T001 inputs.
- [ ] T009 [US2] Execute real multi-process CPU integration in `Experiments/TigerCluster/tests/test_yolo_integration.py` and existing NDNSF integration fixtures, recording `specs/183-tiger-yolo-reusable-experiments/evidence/integration.md`: separate identity/bootstrap tests from a prepared authorized fixture; real signed messages, encrypted dependency Data, actual small ONNX execution and final oracle. Cover fresh Controller/epoch, denied role, wrong selection, activation loss/tamper and process cleanup. No mocked inference final PASS. Depends on T008.
- [ ] T010 [US2] Run bounded CPU MiniNDN YOLO through `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` with Spec183-owned case configuration/driver in `Experiments/TigerCluster/tests` and record `specs/183-tiger-yolo-reusable-experiments/evidence/minindn.md`: normal four-role graph plus registered permission/dependency-failure cases, request-to-response IDs/edge hashes, route snapshots and cleanup; freeze the actual same-source host qualification manifest accepted by the existing build gate. Do not rerun all historical campaigns or substitute echo. Depends on T009.
- [ ] T011 [US2] Build one complete local SIF via `Experiments/TigerCluster/adapters/slurm-apptainer/scripts/prepare-development-handoff.py` and `build-local-sif.sh`; record `specs/183-tiger-yolo-reusable-experiments/evidence/local-sif.md`: input gate, verified base and compute-matched Apptainer, container-built nine outputs, every packaged DSO closure, two extension imports/actual entrypoints, then exact-SIF local CPU YOLO. Keep model outside SIF, register final SIF/runtime identity and tests before promotion. Host/SIF hashes need not equal, but each must derive from its declared source/ABI. Depends on T010; reviewed short version-only allocation may precede build under plan exception.

## Phase 7: User Story 3 - GPU And Cross-Node Execution

**Independent Test**: same immutable runtime with actual allocated GPUs; final graph must contain genuine cross-node model dependencies.

- [ ] T012 [US3] Qualify promotion and allocated environment through `Experiments/TigerCluster/jobs/yolo/submit.py` and `runtime/yolo_worker.py`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/tiger-environment.md`: audit final script/profile closure before side effects; verify local/project/staged SIF equality, compute Apptainer, CUDA/ORT, writable capacity, roles/cwd/mounts, NFD and bidirectional signed Data/permission readiness. No build on Tiger/login-node inference, no runtime path patching, zero Provider launch on mismatch. Depends on T011 dispatch receipt; per-allocation subset repeats in every subsequent job.
- [ ] T013 [US3] Run one bounded single-node GPU job with all four Provider identities via `Experiments/TigerCluster/jobs/yolo/submit.py`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/single-node-gpu.md`: 1 warmup + 1 measured real request, observed CUDA per model role, independent numerical oracle and clean shutdown. This closes only SINGLE_NODE_GPU_PASS; resolve concrete failures and rerun affected earlier gates before advancing. Depends on T012.
- [ ] T014 [US3] Execute first normal two-node allocation from the unchanged normal case in `Experiments/TigerCluster/profiles/yolo-two-node.json`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/two-node-first.md`: actual distinct hosts, A backbone/merge and B heads, 1 warmup + 3 measured requests, both directions of dependency edges and full per-request identity/backend/numeric/terminal closure. Stop on first failure and preserve partial results. Depends on T013.

## Phase 8: User Story 4 - Failure Recovery And Reuse

**Independent Test**: precise failure preserved; subsequent clean allocation reuses the exact normal configuration with no hidden repair.

- [ ] T015 [US4] Execute one bounded registered `negative-dependency` job through `Experiments/TigerCluster/jobs/yolo/submit.py`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/tiger-negative.md`: after genuine Selection and before one required activation delivery, stop the owning Provider or use the registered fault Provider to suppress that edge. Expect exact missing-dependency/peer-failure boundary, no successful User result, no reselection, finite abort and full cleanup. Local mutation/unit gates cover other corrupt config/auth/false-PASS combinations; do not expand a remote fault matrix. Depends on T014 and matching fault harness receipt.
- [ ] T016 [US4] Reproduce normal distributed inference in a second fresh allocation via the same `Experiments/TigerCluster/profiles/yolo-two-node.json`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/two-node-reuse.md`: unchanged E/profile/normal-case/SIF/model/harness/oracle, new run/identities/allocation, 1 warmup + 3 measured complete requests, no prior-run private/cache correctness dependency and no per-run large copies. Repeat complete allocation preflight, reconcile both normal runs and first failure logs. Depends on T015; changing behavior requires new identity and requalification, not counting the changed job as unchanged reuse.
- [ ] T017 [US4] Deliver reproducible configuration, operator guide and closure in `Experiments/TigerCluster/docs/yolo-reusable.md`, `Experiments/TigerCluster/README.md`, and `specs/183-tiger-yolo-reusable-experiments/evidence/closure.md`: check all fields and real commands from a clean checkout, offline recompute results, document fixed versions/qualification scope/cache location/first-failure guide, per-run warmup and measured latencies, exact code/config/SIF IDs, pending limitations and safe cleanup. Register full SC/FR evidence and close only if normal repeated runs plus required negatives meet criteria. Depends on T016; no performance superiority claim.

## Dependencies And Execution Strategy

`T001 → T002 → T003 → T004 → T005 → T006 → T007 → T008 → T009 → T010 → T011 → T012 → T013 → T014 → T015 → T016 → T017`。
T001 中的缺 artifact/GPU 访问不阻止使用已知接口进行 focused 实现，但所有实际 build/run 的输入必须齐全；不能跳过 T007 或伪造后续 qualification。

MVP 为 T001–T004：一份可解释、拒错且不会误提交的 profile/launcher。完整用户目标到 T017 才结束。
无 `[P]` 项：当前关键路径共享 profile/runtime 和真实资格，默认串行；可在任务内部并行只读盘点/离线测试，但不并行构建或自动派发 agent，不同时运行多个同 candidate/gate 实验。
合并了“测试→实现→局部验收→证据”机械链；各任务边界来自配置、生命周期、业务接线、判定、生产审计和独立环境验收。

## Current Checkpoint

2026-09-06：仅完成规划与文档结构检查。T001 尚未执行，不把旧 Spec179/180 或 base-SIF 结果填入本 Spec。新任务从 T001 输入/接口清点开始，遵守当前用户仅编写 Spec 和任务的范围。
