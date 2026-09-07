# Tasks: Reusable TigerCluster YOLO Distributed Inference

**Input**: [spec.md](spec.md), [plan.md](plan.md), [profile contract](contracts/experiment-profile.md), [validation matrix](validation-matrix.md)
**Branch**: `TigerClusterExperiments`
**Status**: 2/17 tasks complete (T001 inventory and T003 focused component acceptance); IN_PROGRESS, no runtime PASS.

2026-09-07 生产路径检查：certifiedGraph 目前只有消费者和合成 fixture，没有
已接线的独立 ORT/图参考生产者。已纠正注释中“生产者已存在”的错误表述；
T005/T006 必须补齐真实 owner、运行时 manifest 绑定和 collector 来源验证，
不得从被测 observations 反推 expected。具体步骤见
[certified-graph-owner-gap.md](evidence/certified-graph-owner-gap.md)。
SSH hostname 检查成功；本轮未提交 Slurm 或启动模型任务。

2026-09-07 identity 拆分调用链补漏：User run_requests 仍直接比较 placement
与 runtime digest；现改为通过绑定 receipt 分别验证，issuer 签发前匹配实际
catalogue 条目，宿主也重验 placement receipt 字段。71 项 application/
provision/prepare 聚焦测试通过（4.54s），含不同摘要正例、串错/缺字段负例。
密码学/model owner 使用替身；单节点公开执行流程仍未接通。

2026-09-07 profile→issuer 输入映射及小文件 staging：明确模板/package/registry/
私钥 locator/epoch/SIF/placement 摘要来源，生成 prepare-input-v2；重验公开
摘要、0600 私钥和独占目标目录，不复制模型或执行外部程序。8 项新真实文件
读写测试连同相关 profile/CLI/provision 回归 83 passed in 16.60s（合成 artifact，
不是真实密钥或 SIF）。最终 local/run 调用和运行资格仍未接通，T004 保持 open。

2026-09-07 准备接线审查：拆分放置候选摘要与运行候选摘要。内部 prepare-input-v2
分别传递 placementCandidateDigest/runtimeCandidateDigest，前者用于 offers，
后者用于 preparation receipt/worker。旧 v1 描述拒绝猜测转换；T004/T005 仍未关闭。
准备生产者/描述解析/进程调用/worker/operator 聚焦回归 32 passed in 2.73s；
生产者测试替换了密码学和模型 owner，进程测试使用假容器，不代表真实 SIF 资格。

2026-09-07 T004/T005 准备调用边界：新增 `yolo_operator.provision_run`，复用
现有 `apps.yolo prepare`、container_command 和 Processes；校验固定 descriptor、
public inputs、SIF/harness 摘要与隔离目录后，单次有界执行离线 issuer，保留
cleanup/logs，并从实际 public/preparation.json 重算摘要和输入绑定。10 项新
测试使用真实短进程、假 Apptainer/合成凭证；连同既有 prepare/operator 测试
24 passed in 2.28s。不是 SIF、密码学签发或模型资格。
尚未接入公开 local/run：最终调用者仍须从同一 profile/已验证候选解析
输入位置和 runtime 参数，并提供已注册的 artifact-policy-authority 私钥。
不得由此组件生成 READY 或绕过 T007/T008–T011。

2026-09-07 real prepare path: removed the impossible dependency on READY from
the content-only checker, which always reports NOT_EVALUATED. prepare now
freezes verified dispatch/harness bytes and an unqualified plan, with no
container, credential generation or remote command. The first unmocked CLI
test then exposed a missing run-directory creation; fixed with exclusive
creation before freezing. Runtime qualification gates are unchanged.
T004 remains open for actual credential preparation and allocation execution.
Focused command/bundle regression: 56 passed in 11.03s; synthetic artifact
fixtures only, no native, model, SIF or Tiger execution.

2026-09-07 actual model-input owner check: signed catalogue verification,
four-role catalogue registration, graph/initializer hashes and fixed
640x640/[1,50,6] reference validation passed on the existing local package.
The external legacy model-manifest summary is not the runtime-generated
canonical manifest; waiting for a replacement signature on that summary was
not justified by the YOLO runtime source. Full adapter import instead fails
on the missing host ndnsf._ndnsf extension. Dispatch manifest ownership and
native/runtime qualification remain open. See
[yolo-input-validation.md](evidence/yolo-input-validation.md).

2026-09-07 T006 retained-verdict regression: collect previously returned an
existing PASS without reopening evidence. Five mutation tests reproduced
false success for missing/invalid/changed handoff, forged qualification and
oracle rejection. collect now always re-runs its authoritative collector and
compares the complete result with the retained verdict without overwriting it.
35 operator tests passed in 8.40s. These are synthetic component tests;
T006/T007 and real model/network validation remain open.

2026-09-07 interrupted-dispatch review: corrected acyclic prerequisites
(hostMinindn → localSif → singleNodeGpu → twoNodeGpu), explicit HH:MM:SS,
case-specific node/task counts, typed GPU GRES and frozen-bundle wrapper path.
Rejected the unfinished sbatch enablement: generic PASS/READY receipt fields
do not validate staging or candidate identity. The command renderer is tested
but not dispatched; submit still stops before journal writes and Slurm.
Focused operator/profile/journal tests: 74 passed in 9.66s.
T004/T007 remain open for semantic receipts, shared-root staging, allocation
runner and unknown-job reconciliation; no native/SIF/Tiger run occurred.

任务按行为闭环组织，每项包含必要失败例→实现→聚焦回归→证据，不拆成“写测试/改代码/跑命令”三个机械任务。正式 broad suites/实验必须等 T007；focused red/green 可在实现中执行。所有 Tiger 文件以下用完整仓库相对路径。

## Phase 1: Setup

- [x] T001 Freeze receiving inputs and unresolved inventory in `Experiments/TigerCluster/docs/yolo-reusable.md` and `specs/183-tiger-yolo-reusable-experiments/evidence/input-inventory.md`: reconcile four repository revisions against `Experiments/TigerCluster/development-handoff.lock.json`, delivery-vs-runtime source, nine native outputs, actual public YOLO APIs, model/package/oracle hashes, local/compute Apptainer and proposed partition/GPU/resources/capacity. Register exact build and test selectors and expected errors from current code; keep missing items WAITING_EXTERNAL_INPUT and historical failures intact. No large build/download or model job. Read-only local inspection first; bounded substrate probe only under the reviewed exception in plan.md.

T001 evidence: [input-inventory.md](evidence/input-inventory.md)。完成清点而非物理输入资格；三依赖精确对象、本地base、签名package/oracle仍缺，compute探测按T007后例外执行。现有host gate绑定旧tiny-onnx、User正常路径一次请求的问题已分派下列任务。

## Phase 2: Foundational

- [ ] T002 Implement phase-specific candidate closure in `Experiments/TigerCluster/runtime/yolo_profile.py` and `Experiments/TigerCluster/tests/test_yolo_closure.py`: test wrong/changed hash, incomplete inventory, path escape, stale receipt, unbound script/env and stage bypass before implementation; enforce I/R/E identities and invalidation with no self-reference, inputs-before-build/runtime-before-execution/dispatch-before-upload gates. Prove rejected inputs make zero build/upload/staging/sbatch calls with spies at the real command boundary. Depends on T001 interface inventory; gates may report missing physical input without pretending it is present.

## Phase 3: User Story 1 - Reusable Launch Configuration

T002 partial checkpoint：[内容完整性实现/21项focused回归](evidence/t002-integrity.md)。check_plane/check_chain已实现，但旧builder dispatch、真实receipt和实际命令边界零副作用测试未完成；保持unchecked，不作为正式资格。

T002追加接收审查修复：在既有`build-local-sif.sh`与host-gate owner增加显式Spec183 workload dispatch；校验实际YOLO日志/数值/源seal的receipt，保留Spec175 v1/v2。新增回归要求未知schema、错workload/源/receipt或失败证据使Apptainer调用数为零。T010产生真实receipt，T011消费；不得填M01假清单，也不得绕过已有source/ABI门。

**Independent Test**: deterministic resolved config from another cwd; fail-before-side-effects; two concurrent submit attempts cannot both launch.

- [x] T003 [US1] Extend reusable role/container lifecycle in `Experiments/TigerCluster/runtime/baseline.py`, `identities.py`, and `yolo_worker.py`, with `Experiments/TigerCluster/tests/test_yolo_runtime.py`: preserve CPU baseline behavior; support explicit GPU/env/mount/cwd mapping, isolated role identities/PIB, owned process groups and bounded peer/readiness checks. Add real short-lived parent/child cleanup tests, wrong cwd/port/shared PIB rejection and no host runtime injection. Reuse common primitives, not a copied supervisor. Depends on T001 and the T002 content-integrity interface; final T002 qualification is required at T007, not before implementation of its consumers. Implementation/focused acceptance: [evidence/t003-worker.md](evidence/t003-worker.md); actual workload wiring remains T004/T005, production audit T007.
- [ ] T004 [US1] Deliver strict profile and one entrypoint in `Experiments/TigerCluster/profiles/yolo-two-node.json`, `schemas/tiger-yolo-v1.schema.json`, `jobs/yolo/submit.py`, `jobs/yolo/run.sbatch`, and `tests/test_yolo_submit.py`: expose the five contract commands, resolve every field into actual argv/env, reject leftovers, bind immutable bundle, stage-scoped gates and allocate-once journal. Test atomic duplicate-run prevention and SUBMISSION_UNKNOWN recovery without resubmit; register case differences and walltime/timeout budgets. Fill real operational values before enabling submit; do not modify old `profiles/two-node.json`. Depends on T003.

## Phase 4: User Story 2 - Current YOLO Path And Independent Verdict

**Independent Test**: existing application APIs and independent oracle exercised by focused tests; no copying an expected tensor into the distributed output.

- [ ] T005 [US2] Wire the existing secure YOLO application into `Experiments/TigerCluster/apps/yolo.py` and `runtime/yolo_worker.py`, with `tests/test_yolo_application.py`: reuse `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` and installed native Provider interfaces; map the four roles without fabricating ACK/Selection, verify permissions/current status and direct predecessor Data, enforce native input and independent oracle contract. Focused tests must catch lost structured assignment, stale epoch/input, missing backbone-to-head Data, duplicate/late output, and forbidden shared-filesystem activation shortcuts. Any Core/DI repair belongs to its existing source/tests and updates candidate provenance. Depends on T004.

T005 partial checkpoint：新增 `runtime/yolo_operator.py` 作为唯一 rank-level production seam：校验已解析 run plan、准备收据摘要、bundle/public/output 边界、节点角色、端点、CPU/GPU 分配参数和 request budget；构造 `NodeRuntime.from_preparation`，为 startup/completion 使用同一 run/candidate/probe 绑定但不同目录的 `StartupBarrier`，随后只调用既有 `apps.yolo.run_normal_node`。它不生成 ACK、Selection、模型输出或 verdict。`test_yolo_operator.py` 已用 lifecycle double 验证拒绝路径和实际参数绑定；应用回归还验证正常 lifecycle 顺序和启动失败清理，完整 TigerCluster suite **798 passed in 33.44s**。证据见 [t005-operator.md](evidence/t005-operator.md)。本 checkpoint 不代表真实 SIF、Provider、MiniNDN 或 GPU 资格，T005 仍保持 unchecked，后续必须补真实 application/collector 接线和端到端证据。
- [ ] T006 [US2] Implement authoritative collection and failure oracles in `Experiments/TigerCluster/runtime/yolo_result.py` and `tests/test_yolo_result.py`: associate every request/attempt/plan/role/node/GPU/edge digest, validate numeric output via the existing contract, require all finite child exit0 plus controlled service shutdown/reap, and separate local/single-node/two-node/expected-rejection verdicts. Reject stale/forged PASS, missing role/response/CUDA, shape/class/tolerance mismatch, timeout-as-auth-rejection, nonzero worker, forced cleanup or corrupt output. Retain first failure and immutable reanalysis. Depends on T005.

## Phase 5: Design-Code Convergence

T005/T006追加调用次数约束：正常ACK-driven User一次只执行一请求。warmup/measured用独立User调用、request/attempt/输出目录及数值文件，Provider持续运行；focused test从生产入口证明实际调用次数，不能仅检查legacy sequential参数存在。

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

实现顺序：`T001 → T002 内容完整性接口 → T003 → T004 配置/冻结/提交状态接口 → T005 → T006 → T004 五命令最终接线/验收 → T002 集成验收 → T007 → T008 → T009 → T010 → T011 → T012 → T013 → T014 → T015 → T016 → T017`。
T005依赖T004已有输入/运行接口，不要求其尚待T005/T006才能实现的实际argv和collect提前完成。T004保持unchecked，最终生产bundle必须包含真实应用/collector/run.sbatch；禁止生成占位文件让冻结检查假通过。T007仍同时要求T002–T006全部闭合，不减少任何资格门。
T002 的真实启动边界来自 T004，receipt 语义校验来自 T006；原先要求 T002 全部完成才实现消费者会造成循环依赖。允许先实现消费者不等于放开资格门：T002保持unchecked，T007必须同时验收T002–T006。T010才产生真实host receipt；之前仅可用明确标注的测试fixture验证拒错逻辑。
T001 中的缺 artifact/GPU 访问不阻止使用已知接口进行 focused 实现，但所有实际 build/run 的输入必须齐全；不能跳过 T007 或伪造后续 qualification。

T001–T004形成 profile/launcher 实现骨架；可操作 MVP 还要求 T006 判定器、T002集成验收和T007审计，不能把骨架作为可提交实验的版本。完整用户目标到 T017 才结束。
无 `[P]` 项：当前关键路径共享 profile/runtime 和真实资格，默认串行；可在任务内部并行只读盘点/离线测试，但不并行构建或自动派发 agent，不同时运行多个同 candidate/gate 实验。
合并了“测试→实现→局部验收→证据”机械链；各任务边界来自配置、生命周期、业务接线、判定、生产审计和独立环境验收。

## Current Checkpoint

2026-09-07 canonical Sync and component regression checkpoint：`applicationName + '/sync'`
（例如 `/appname` → `/appname/sync`）已由 profile、projection、NFD route 和 startup
validation 共用；不得从 Provider prefix 或旧 `/group` 推导。重新运行完整
`Experiments/TigerCluster/tests`：806 passed（32.84s）；仍为 component-only，未改变
T007 的 BLOCKED/NOT READY 状态。当前仍等待 candidate-bound signed model manifest、
Spec183 profile、native/SIF 和真实 MiniNDN/Tiger 证据。

2026-09-07 exact-SIF probe 修复：历史 base SIF 的 `ndnsf` 导入证明默认只读 home
会失败；preflight 现在为每次 probe 提供临时隔离 `--home`，相关 12 项 builder/
preflight 回归通过。该修复不改变历史 base 的资格，也不关闭 T008/T011。

随后重跑 TigerCluster 全部组件测试并合并 Spec183 preflight/builder 选择器：818
passed（38.15s）。这仍是 component-only 证据，T007 仍需真实 profile、candidate
manifest、native/SIF、MiniNDN 和跨节点运行收敛。

2026-09-07 输入接收推进：四个锁定源码已在隔离 `/tmp/ndnsf-spec183-src` 工作树按
`development-handoff.lock.json` 精确 checkout，五个锁定 wheel 已下载并逐一校验，
`prepare-development-handoff.py verify` 返回 `SOURCE_READY`，source seal 为
`sha256:9129d07298f5754823f3bc2bf9c10fea7416adbb1e4168ced3dc82750948612c`。
远端 Spec180 YOLO 包虽含匹配历史 graph/weights/oracle，但缺少 registry 引用的
`model-manifest-authority.pub`，且含私钥材料；仅保留为输入审计，不得进入 Spec183
公共 bundle。锁定 base SIF 已只读传输到本地 ignored cache，并按完整内容校验为
`sha256:b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`；它仍
不能替代 Spec183 本地构建或 T007/T008/T011 资格。

T006 native identity checkpoint: the actual native reader now rejects missing,
malformed or wrong-role `modelDigest`/`artifactDigests`; nine negative mutations
first passed incorrectly and now reject. This is structural evidence, not a
certified-package comparison. Source audit found ORT_ENABLE_BASIC and
`session.disable_cpu_ep_fallback=1` unless explicitly allowed, but these settings
still need real runtime qualification. `bindNativeRunnerPreparationContext`
falls back from absent modelManifestDigest to planDigest: do not equate a valid
SHA-shaped modelDigest with independent model identity. Raw ONNX node counts
must not be compared directly with optimized ORT profile event counts.
T006 remains open for independently bound model/graph coverage and final operator
wiring; no build/SIF/Tiger inference gate is closed by these checks.
Focused checkpoint: **823 passed in 54.82s** across TigerCluster component tests
and the six registered Python backend/public-recipient/numerical/identity files;
JUnit: `Experiments/TigerCluster/results/t006-native-model-identity-r1/junit.xml`.
Synthetic observation mutations are not actual native model execution evidence.

Follow-up checkpoint: public retained assignments now use envelope
`yolo-public-assignments-v2`; each role carries canonical model-manifest and
artifact digests from the typed Selection assembly. The User writer rejects an
incomplete identity before creating the retained file, and the collector rejects
old/malformed envelopes or wrong-role model bindings. The complete focused suite
passed **843 tests in 45.51s**; JUnit:
`Experiments/TigerCluster/results/t006-graph-coverage-r1/junit.xml` (ignored
runtime output retained locally; the durable model-binding JUnit remains
`Experiments/TigerCluster/results/t006-model-binding-r1/junit.xml`).
This remains structural evidence, not an independent signed-package or
optimized-graph comparison.

The collector now accepts an optional externally staged
`tiger-yolo-certified-graph-v1` document. When supplied, it requires exact
per-role model/artifact bindings, backend, and the certified post-optimization
node-name set; it never derives expected coverage from Provider logs. Four-role
coverage tests passed in the 120-test retained/projection subset. Final operator
wiring must make this document mandatory before T006 can close.

Follow-up checkpoint: `finalize_expected_rejection` now provides a separate
fail-closed terminal boundary for the registered `negative-dependency` case.
It requires one committed Selection, zero reselection, a bound
`DEPENDENCY_DATA_MISSING` or `PEER_FAILURE` edge after Selection, no response,
and bounded non-forced cleanup. The focused retained-execution suite passed
**53 tests** and the complete registered focused suite passed **852 tests**;
JUnit: `Experiments/TigerCluster/results/t006-negative-rejection-r1/full-junit.xml`.
This is still component-only evidence: no real negative runtime was executed,
the operator collector is not wired to it, and T006/T007 remain unchecked.

The public component collector now owns the complete normal request loop via
`collect_normal_verdict`; it requires the registered warmup/measured reference
schedule and a certified graph before calling the fail-closed final boundary.
The resulting `tiger-yolo-final-verdict-v1` still represents component-only
evidence: the operator CLI is not wired to real retained paths, so T006/T007
remain open and no native/SIF/Tiger execution is authorized.

2026-09-07 retained allocation/GPU join: collect_retained_request now forwards
the external journal/profile allocation expectations to the dependency
collector. GPU node entries require trusted allocationDigest/gpuProbeDigest,
not caller-authored gpuBinding. read_retained_device_binding revalidates
run/prep/candidate, raw Slurm records, observed task data, probe nonce/log,
allocation link and probe-before-Provider launch order against the node receipt.
Two nodes must agree on job/step/host list/uid and have distinct hosts/UUIDs.
20 source-shaped real-file join tests pass. Trusted staging/final operator,
certified graph, negative path and real native/SIF/Tiger validation remain;
T006 is still partial, no runtime PASS. See t006-slurm-allocation.md follow-up.

2026-09-07 Slurm task binding: read-only site queries confirmed 24.05.2,
/usr/bin/scontrol and data_parser/v0.0.41. No active user job. Nonexistent job
and step queries return exit0 with empty/null records, so new validation
requires exactly one matching running record. capture_task_allocation and
NodeRuntime.verify_allocation check journal job/comment, uid, partition/GPU
type, hosts/ranks/tasks and selector before GPU probe or Provider launch.
Allocation JSON is candidate/run-bound; GPU probe links its digest. Actual
positive allocation test, trusted offline receipt join and final operator
inputs remain pending. See evidence/t006-slurm-allocation.md. No task closed.

2026-09-07 independent CUDA probe: run_normal_node now runs the finite
NodeRuntime.probe_gpu_device before network/Provider startup in GPU cases.
The frozen runtime/yolo_gpu_probe.py queries CUDA runtime count=1 and maps
ordinal0 through PCI to a driver UUID; no NVML-index shortcut. The same
container_command/selector/HOME/cleanup owner is reused, with a fresh nonce,
bounded startup budget and exclusive gpu-probe.json/log. A failed child cannot
qualify even if it prints matching JSON. This is CUDA visibility evidence,
not Slurm allocation attestation. Actual job/step/node receipt binding and
offline trusted consumption remain pending. No native/SIF/GPU test run.
Also fixed node receipts incorrectly requiring native Provider PID witnesses
for finite management/probe commands borrowing Provider HOMEs; actual
persistent Provider witness checks remain mandatory. T006 remains partial.

2026-09-07 retained device collection: the retained request -> dependency ->
role path now invokes validate_device_binding. GPU node entries require an
externally supplied gpuBinding {uuid, visible}; model roles receive their
owner node's binding, while Merge/CPU require no GPU binding and empty device
claims. A red real-reader fixture demonstrated CPU visibility was previously
accepted; corrected and covered alongside missing/mismatched CUDA bindings.
These are source-shaped records, not executed GPU evidence. The actual
allocation receipt producer and trusted staging are STILL PENDING, as are
live-Worker device join, certified graph and final operator. No task closed.

2026-09-07 device identity component: validate_device_binding compares native
CUDA UUID/list, CUDA_VISIBLE_DEVICES and runtime device ordinal against
independently resolved allocation/launch expectations; requires the native
cuda-runtime-pci+driver-uuid source. One exposed GPU means runtime ordinal 0,
even when its host selector is 3 or a UUID. CPU/Merge reject GPU claims or
visibility. Thirty-six source-shaped tests pass; no actual GPU probe. Next
wire independently verified allocation receipts and role mapping into the
final collector; do not derive expected UUID from the observation itself.

2026-09-07 PID binding wired: Worker now generates a fresh 64-hex nonce for
each Provider launch and executes the in-container witness before execing
the native Provider. Node receipt v3 retains launchNonce and host PID;
writer/reader require exactly one matching nonce/role/namespace-PID marker.
Native readers use that observed namespace PID while host PID remains the
cleanup identity. Live and retained collectors pass the nonce explicitly.
73 affected tests pass, including real namespace mismatch/reader regression
without skips. Earlier ordinary-process launcher fixtures now emulate the
bundle Python module lookup explicitly. Exact-SIF gate still required; this
is not Tiger qualification. Continue GPU/graph and final operator work.

2026-09-07 critical PID namespace audit: Apptainer --containall isolates PID
(confirmed by local installed exec --help); native getpid() is not the host
Popen PID currently used by role collectors. A real unshare user/PID namespace
test reproduced differing host/container PIDs. New yolo_launch_witness emits
a host-generated nonce/role/container PID and execs the Provider, preserving
that PID; included in required frozen harness inventory. Six actual-process
tests passed with no skip locally. NEXT REQUIRED: wire nonce/witness into
Worker launch, node receipt and native readers; do not merely drop PID checks
or disable isolation. Current collectors remain unsuitable for actual SIF
until this wiring is complete. T007 must block promotion on this issue.

2026-09-07 retained request join: collect_retained_request derives the User
output directory from node0 and the frozen invocation index, validates the
real lifecycle/numerical pair, recomputes role and Selection digests and count
bindings, then invokes four-role retained execution/dependency checks with
the lifecycle execution-plan identity. Runtime release candidate digest and
catalogue placement candidate digest are distinct explicit arguments. Normal
cases require attempt-1 (no retry/reselection). Nine tests use the real
lifecycle reader and explicit numerical/native doubles. This remains a
component join; GPU/graph/allocation/final normal operator pending.

2026-09-07 retained cleanup semantics: offline receipt reading now invokes
shared validate_cleanup_records (also used by live Worker cleanup), recomputes
cleanupSummary and verifies every frozen User invocation and unique request ID.
Seven red cases previously accepted hash-consistent but semantically invalid
receipts; now rejected. Sixty affected tests passed, including real OS cleanup
boundaries. This closes an offline composition gap, not final experiment
qualification; trusted staging, GPU/graph and complete operator remain pending.

2026-09-07 retained execution join: collect_retained_role_execution reads
verified node receipts, uses their PID/log binding, enforces request/attempt/
execution-plan identity and role-local current ORT profile. No reconstructed
Worker. collect_retained_dependencies combines the exact one/two-node role
layout, all four Providers and public DATA_V1 dependency agreement, comparing
log hashes again. Twenty-one tests added: real retained file readers with
fixture ownership/observations, plus explicit cross-node dispatch doubles.
Final normal operator, trusted staging, GPU/certified graph and full cleanup
receipt semantics still pending; no experiment PASS.

2026-09-07 node log receipt v2: actual write_worker_receipt now seals each
launched process's regular bounded log path/bytes/hash after cleanup.
read_node_log_receipt consumes transferred output independently of Worker
instances, requiring externally trusted receipt digest plus frozen plan,
candidate, preparation and rank bindings. It verifies exact launch fields,
log content and service coverage; no fabricated Worker ownership. Twenty
receipt tests passed with actual files and fixture ownership. Remote trusted
receipt collection, full cleanup/requests/allocation and final verdict remain
pending. v1 is historical and rejected by the new reader.

2026-09-07 owned dependency collection: `collect_owned_dependency_result`
requires all four roles from closed, reverified prepared Worker ownership
objects, correct node rank/mode coverage and actual launcher-derived paths.
It joins existing native PID/profile validation with public dependency checks
and compares per-log hashes across both reads. Nine join tests use boundary
doubles, alongside existing real OS process role tests; no native qualification.
ORT source audit confirms the configured post-Selection preparation factory
creates a new runner/session per invocation with unique profile prefix;
keep strict current-request profile binding. Warmup is not proof of reuse of
an already loaded ORT session. Complete operator/GPU/graph/final receipt join
and T007 remain pending.

2026-09-07 cross-role join: `read_public_dependency_contract` validates bounded
User evidence against external request/attempt/plan/role/Provider facts and
requires exact agreement of producer outputs and consumer inputs.
`collect_dependency_result` connects those expected edges to DATA_V1 log pairs,
with exact log-role coverage. APPLICATION_INPUT is retained separately: native
handler pre-satisfies it from authenticated request ingress, not dependency IO.
Tests use actual typed projections and synthetic log records; owned-PID/final
normal operator integration remains pending. T006 stays unchecked.

2026-09-07 User retention wiring: generic public projection now lives in
`ndnsf_distributed_inference/sdk/public_evidence.py` (no dependency from the
maintained YOLO example on Tiger harness files). `--retain-public-assignments`
defaults off; the frozen Tiger User argv enables it. Actual User helper checks
role coverage and all request/attempt/execution-plan/Provider bindings before
exclusive bounded 0600 output, never raw assignment wire. Nine retention
tests exercise the actual function with real typed Selection bytes and a
handle fixture. Cross-role collector join and native qualification remain
pending; no task completion claim.

2026-09-07 public dependency projection: `runtime/yolo_projection.py` now
decodes real typed V3 Selection wire and emits an explicit non-secret allowlist
bound to request/attempt/execution-plan/Provider. Native attempt-session and
multi-tensor/redistribution scope rules verified against source. Eleven new
tests plus eight log-pair tests passed; User retention and final collector
wiring remain pending. T006 stays unchecked; no native/SIF/Tiger PASS.
See `evidence/t006-dependency-pairs.md`.

2026-09-07 发现并修复V3 numerical planDigest错用outercarrier：新增handle.execution_plan_digest读取coordinator已绑定runtime摘要，YOLO numerical/终端marker改用该属性；不改任一digest计算。4属性kernel+数值related共41通过，native handle未验证。公开dependency projection仍待做；见evidence/t006-lifecycle-component.md。

2026-09-07 依赖trace启用NDNSF_DI_DEPENDENCY_OBJECT_TRACE=1；collector按外部sealed edges/session配对publish/fetch DATA_V1、role/name/scope/bytes，8新fixtures+Worker共39通过。须补真实sealed-plan public投影并绑定四条edge与planDigest，negative/实际跨节点仍未验证。见evidence/t006-dependency-pairs.md；T006未闭合。

2026-09-07 collect_request_result合并lifecycle与实际response重分析，严格对冻结graph/catalogue，numerical所用plan/result/request/attempt全部来自已验证lifecycle。4新增交叉绑定负例/正例，数值suite15通过；REQUEST_RESULT_COMPONENT_ONLY，native/GPU/edge/cleanup仍未闭合。

2026-09-07 two-rank real-file barrier回归：请求pending时对端不得close、正常双方退出、请求失败双方保留记录。修复跨phase failure盲区：completion持续检查startup失败但不复用旧deadline，finite User也监控common failure lane。6 orchestration通过，runtime/应用仍double，非NFD/SIF验证；见evidence/t005-normal-node-owner.md。

2026-09-07 internal normal node owner接线：network→startup→rank0 requests→both-rank completion→close→node receipt；completion独立目录/后启动预算、同run/candidate/probe绑定，User时限clamp剩余预算；失败通知+本地node-failure记录。3新orchestration double测试+application共45通过；实际双rank/negative/fullvalidator/operator/T007仍未完成。见evidence/t005-normal-node-owner.md。

2026-09-07 node回执组件：复核preparation、case/rank/output、全部persistent service与normal User index覆盖，cleanup通过后exclusive0600写run/plan/prep/candidate/launch argv hash与cleanup。9 contract测试通过；Worker/prep为double，完整probe/management/requestId/推理/edge/allocation仍须验证。NODE_CLEANUP_COMPONENT_ONLY，negative-dependency未放行；最终operator待接线。

2026-09-07 T006角色路径/启动关联：仅将/output子路径映射到role-owned输出，拒绝traversal/symlink/任意host路径；collect_role_execution从已关闭Worker唯一launch PID与日志接native+ORT校验，CPU/GPU/Merge三种分支。12 focused通过（真实OS PID/log/cleanup，合成execution/profile，非模型运行）。节点回执/完整inventory/物理GPU/edge/最终collector继续待办。

2026-09-07 cleanup校验对接真实NodeRuntime launch/close清单，不信任外部childCount；拒绝缺失/重复/PID错/forced/unreaped/lease残留/服务提前退出，有限请求必须exit0。12新用例且三种模式已有真实OS schedule测试接入，相关54通过（Apptainer仍double）。完整planned inventory/节点回执/最终collector待接线，T006未完成；见evidence/t006-worker-cleanup.md。

2026-09-07 T006接入bounded native日志唯一记录选择、ORT profile独立解析/节点分配逐项核对/请求绑定与文件hash，补CPU/Merge正例；相关46项通过。profile仅每session首次capture，warm复用行为需核验，旧profile继续拒绝；物理GPU/graph节点覆盖/edge/cleanup/最终collector仍未完成。详见evidence/t006-native-observation.md。

2026-09-07 native观察组件：发现旧collector不兼容Boost PropertyTree bool/uint64字符串且hardcode example identity；新增严格decoder+真实Provider/PID/request/attempt/plan绑定校验，29 focused通过。不把日志声明当实际GPU/edge证据；ORT profile/物理GPU/依赖/cleanup/完整collector仍待做。见evidence/t006-native-observation.md。

2026-09-07 GRAPH_READY catalogueDigest source接通：YOLO builder既有验签/图/权重校验后保存签名body摘要，V3发出它并拒绝非法非空digest。真实LifecycleJournal文件→collector回归通过，连同候选映射/边界共34 focused通过；仍须最终collector对冻结catalogue做精确比较及真实native验证。T006保持unchecked。

2026-09-07 V3 candidate身份source修复：Yolo26Splitter重建catalogue条目的runtime candidate，精确digest唯一匹配后返回注册ID/digest；V3 lifecycle调用resolver，runtime计划/命名/digest不变。4项隔离production-kernel回归通过；真实adapter/planner未验证。另发现GRAPH_READY缺少catalogueDigest，writer未拒绝缺字段，须由verified catalogue补齐；T006仍未闭合。

最终复核：timestamp巨大整数负例补齐后25项lifecycle测试，完整focused集合480 passed / 45.24s（results/t006-lifecycle-r2/junit.xml）；未执行真实native/inference，候选身份缺口仍待修复。

2026-09-07 T006新增bounded lifecycle组件：按维护中journal的10个事件严格核对外部case/request/attempt/candidate绑定、字段/顺序/计数/digest/有限时间，拒绝duplicate JSON、symlink与超限输入；24 focused用例通过。保持LIFECYCLE_COMPONENT_ONLY，不能证明Provider执行/edge/cleanup。此前发现的 V3 planner `PLACEMENT_DECISION` candidateId/candidateDigest 混淆已由 adapter 的 `describe_candidate_identity` 解析器修复，并由 `tests/python/test_spec183_candidate_identity.py` 覆盖；相关回归 **34 passed**。T006/T007继续unchecked；见evidence/t006-lifecycle-component.md。

2026-09-07 T006数值重分析组件已实现：User显式opt-in保留0600/≤1MiB响应bin并记录digest，Spec183启用且绑定prepared candidate env；离线重算真实响应而非信任matched flag。共用纯NumPy tensor decoder，adapter exports按需加载使oracle/codec不依赖_ndnsf导入；operator NumPy锁定1.24.4。扩展focused455 passed（35.86s），包括实际producer函数/codec/数学/文件和独立import进程，但参考值为fixture、无模型/native/SIF/Tiger运行。T006完整lifecycle/role/node/GPU/edge/cleanup及operator/T007待完成；见evidence/t006-numerical-reanalysis.md。

2026-09-07 configure_network已接入实际NFD/nfdc启动链，生产nfd-ready/routes-ready：配置端口/两侧endpoint绑定、真实socket类型检查、有限管理子进程、剩余startup预算、peer失败、无覆盖receipt；管理进程借用空闲Provider HOME，不与NFD并发用PIB。route_commands显式接收appName/sync；旧baseline默认/group保留。7新增组件测试（OS进程/socket真实，NFD/nfdc为double），总419 passed（26.61s）。最终operator仍须绑定真实allocation并依次调用configure_network→start_workload→真实requests/collector；T005/T006/T007未完成，无native/SIF/Tiger运行。详见evidence/t005-nfd-network-setup.md。

2026-09-07 启动协调组件接入Controller/Repo/Provider/双向探测实际helper，原子且run/candidate/probe绑定barrier、共享startup预算、peer失败传播；必须两侧network receipt匹配后才启动Controller，完整Provider ready后返回RUNTIME_READY。用户明确Sync为`/<appName>/sync`：plan.applicationName→runtime.application_name→group，不从provider_prefix推导；错`/group`路由拒绝。10新增组件用例，总412 passed（26.13s）。NFD实际路由安装/最终outer worker、完整collector/T007仍未闭合，无SIF/Tiger运行；见evidence/t005-startup-coordination.md。

2026-09-07 双向签名Data readiness组件已实现：apps/yolo_network.py，两rank并行、同一新probeId、真实RSA验签、精确name/payload、独立有限窗口；复用finite-role进程/HOME管理，探测不挂载model/GPU，退出后允许真正Provider启动。身份从prepared plan传入，不假设等于role名字。父层校验精确回执与退出，正式operator仍须同时接受两个方向；harness清单更新15文件。13项新增组件测试，总402 passed（25.00s），其中加密真实但Face是内存double。实际NFD/SIF/Tiger仍NOT_RUN；T005/T006/T007未闭合。详见evidence/t005-network-readiness.md；下一步完整worker协调/启动barrier及请求collector，不扩大实验。

2026-09-07 Repo readiness组件已接入真实NetworkDistributedRepoClient.capability()接口，normal/FirstResponding、禁用Targeted fallback；有限User独占HOME且不挂载模型。启动probe有独立nonce/回执/调用目录、monotonic预算及外层进程deadline，拒绝过期/异Repo/非零退出/symlink证据，cleanup完成才写READY。18项新增测试，总389 focused（22.60s）；native RPC仍为测试double，真实SIF/NDN运行NOT_RUN。跨节点readiness、完整operator和T006仍待完成；T005/T007保持unchecked，不提交Tiger作业。

2026-09-07 Controller publication readiness接入真实receipt共享校验器，MiniNDN和Tiger复用runtime/yolo_result.py，拒绝重复artifact行；宿主校验不导入SIF-only路径。Provider readiness绑定精确identity/单role READY，source已确认在权限安装之后，不代表model/CUDA ready。8项新增测试，总371 focused（22.30s）。yolo_result.py仅完成publication检查，T006完整collector仍待实现；正式operator、Repo/跨节点readiness及T005/T007仍未闭合。

2026-09-07 NodeRuntime.from_preparation已消费receipt/inventory，在构造前绑定mode/rank/role/run输出，并在每次role启动/User调用前重查；不在100ms存活轮询中hash。3项真实文件边界测试，总363 focused（20.70s）。正式operator仍须接用该factory并独立验证SIF/gates；直接constructor仅低层生命周期测试用途。T005/readiness/T006/T007未完成。

2026-09-07公开准备清单已写入receipt：精确文件集合/逐文件hash，拒绝未知目录、特殊文件、symlink、private PEM和缺失；verify_preparation绑定外部receipt hash及run/candidate再重算。10项文件fixture测试，总360 focused（20.85s），最终类型收紧后10项再次通过。外层operator/worker消费仍待接线，完整SIF准备及身份/模型资格未执行；T005/T007仍未完成。

2026-09-07内部prepare CLI/固定输入descriptor已接通，共享容器启动器只允许离线准备挂载/inputs，禁止普通worker/网络/GPU带入私有输入。处理Apptainer预建空root HOME，仍拒绝旧内容；7项边界测试通过。正式submit.py prepare资格入口、public manifest inventory/readiness/T006仍待闭合，完整native准备未执行。详见t005-public-recipients.md。

2026-09-07内部prepare编排已接入apps/yolo.py：固定空挂载、输入hash、原adapter签名图校验、真实issuer与各类材料、原Y-B policy/Repo权限/native plan和catalogue batch生成器。5项配置投影测试通过，总343 focused（20.50s）。完整prepare尚未实际执行，最终CLI/public manifest绑定/readiness/T006仍未闭合；不能根据代码接线宣称PREPARED或T005完成，T007须审调用边界、T008/T011实跑。

2026-09-07准备路径审查发现Controller会向只读/config写publication receipt；已增加显式receipt参数，Spec183改写/output/runtime-publication-receipt.json，输入保持只读且拒绝覆盖。338 focused通过（20.61s），文件行为测试使用真实函数体+模拟传输，不代表签名发布通过。Controller源码变化要求新source seal/SIF。准备主流程及readiness仍待完成，T005 partial。

2026-09-07 pinned trust导入组件完成：保留registry原始bytes和catalogue/model/authority公钥，校验candidate digest、public hash、epoch及authority私钥匹配，只将私钥放User HOME。`authority.pub`是native定位别名，不再要求重写registry的publicKeyPath；此前checkpoint该措辞已被修正。6项真实crypto/加载测试，总337 focused（21.89s），类型收紧后6项再次通过。尚需最终prepare接线、完整模型签名验证、runtime publication/readiness与T006；T005仍partial。

2026-09-07 offer准备组件已实现：四个独立Ed25519签名密钥，实际certificateName/keyLocator、Provider/service、candidate和Trust Schema绑定；复用独占写入和HOME lease。5项新增测试，总331 focused（20.04s）。尚未接最终prepare或真实ACK验证；下一步authority/catalogue输入认证和完整准备/readiness，后续T006。T005仍partial，详见t005-public-recipients.md。

2026-09-07身份issuer现从实际证书Data记录certificateName/keyLocatorPrefix，避免offer policy猜测名称；5项wire解析测试通过，总326 focused（21.32s）。这是证书结构解析，不是签名认证/实际issuer通过。下一步offer密钥及policy准备接线、模型/authority认证和readiness。T005仍partial，见t005-public-recipients.md。

2026-09-07真实recipient生成组件已加入既有identities.py：每Provider独立密钥/私有map，User独立requester seed和public map；独占写入及HOME lease，拒绝覆盖/身份重复。4项真实crypto测试通过，总321 focused（21.13s）。尚未接最终prepare；模型/authority/offer材料认证、readiness及T006仍待实现，T005不关闭。详见t005-public-recipients.md。

2026-09-07 native recipient启动接线已按真实C++环境变量完成，私钥map精确限定单Provider自身HOME；User和Provider共用`/config/contracts/trust-root-registry-v1.json`，publicKeyPath契约为`contracts/authority.pub`。新增6项拒错，317 focused通过（20.91s），证据追加于t005-public-recipients.md。下一步生成并认证这些真实材料及readiness；不能把fixture路径检查当作native grant通过。T005仍partial。

2026-09-07公共recipient接线：User支持public-only map，Spec183启动强制显式protected epoch及自身requester/authority密钥；311 focused通过。真实User seam新增两项测试因缺少`_ndnsf`在setup失败，未证明grant集成通过；T008须完整重跑。见[evidence/t005-public-recipients.md](evidence/t005-public-recipients.md)。下一步signed准备/registry布局/native Provider recipient/readiness，再T006；T005/T007保持未完成。

2026-09-07控制面接线：apps/yolo.py已复用Controller/Repo入口，role-local policy/store、显式capacity、bounded JSON拒错；见[evidence/t005-control-launch.md](evidence/t005-control-launch.md)。新审查发现现有User默认plaintext-v1且旧protected seam读取Provider私钥map，未满足Spec183隔离；下一步优先实现公共recipient key map并绑定显式protected epoch/requester/authority/native recipient输入。沿用既有可信in-process authority边界，不新增网络授权服务，不共享Provider私钥。完成后继续signed准备/readiness/T006；T005及T007保持未完成，不放行Slurm。

2026-09-07源码发现：YOLO声明CPU/CUDA备选，但_v3_role_specs只取首项，GPU-only模型Provider或CPU Merge无法同时满足。已在原DI coordinator保留明确的ONNX CPU/CUDA family后再由ACK选择；9个kernel测试复现/修复并保留资源/角色/CPU-only负例，见[evidence/t005-backend-selection.md](evidence/t005-backend-selection.md)。T008必须在原生绑定构建后以`SPEC183_REQUIRE_NATIVE_PLANNER_IMPORT=1`重跑该文件，禁止AST模式代替完整import。该源码变化使旧runtime/source seal失效；handoff交付SHA只作provenance，新SIF必须含修复。T005仍partial，Controller/Repo准备和T006未完成。

2026-09-07追加User接线：apps/yolo.py复用真实one-shot入口，NodeRuntime顺序持有User HOME并保留Provider，修正裸requestId与错误output预览路径。两节点1+3、单节点1+1均经进程边界聚焦测试；首失败停止。见[evidence/t005-user-schedule.md](evidence/t005-user-schedule.md)。T005仍partial；下一步准备signed material、Controller/Repo和真实permission/catalogue readiness，再T006。不得把测试validator替代正式collector；negative-dependency暂拒绝普通scheduler。

2026-09-07追加：T005 partial已将安装版native Provider启动参数接入NodeRuntime，四角色CPU/GPU与启动前拒错共15项新增测试；完整Tiger focused集合 **275 passed in 16.18s**。见[evidence/t005-native-launch.md](evidence/t005-native-launch.md)。下一步仍为真实应用coordinator、Controller/Repo/User准备与安全readiness、逐请求执行，然后T006；T005未完成，不放行T007/Slurm。

2026-09-07：T001/T003完成（2/17），T004 partial新增脚本bundle freeze/verify并接入dispatch真实入口，最新 **260 passed in 17.17s**，见[evidence/t004-harness.md](evidence/t004-harness.md)。下一步转T005实际应用/argv/cache/安全模型传输，再T006，回填T004完整五命令和T002资格。生产bundle缺真实应用/collector/run.sbatch，不制造占位文件；不新增Provider模型旁路挂载。三依赖精确commit已隔离接收，原工作树未改；source封装、签名模型包和本地base仍待完成。T007及全部正式环境门未通过，无构建/上传/Slurm/模型执行。

2026-09-07 T002 dispatch checkpoint：新增 Spec183 专用 host-gate receipt
validator，固定 `applicationName + '/sync'`（applicationName 自带前导 `/`）、四 Provider、shared-backbone
图和 normal/permission-rejection/negative-dependency 三类 case，并绑定
source seal 与 evidence 文件 hash。`build-local-sif.sh` 通过
`--workload-kind spec183-yolo --spec183-host-gate` 进入该路径；无效
schema/workload/source/receipt 或混用旧参数时，在 Apptainer version/build
之前拒绝。Spec175 原有入口与 version 命令顺序保持不变。host-gate 与
builder 回归 **22 passed**；仍为 component-only/side-effect-boundary
证据，真实 T010 receipt、T011 SIF 与 T007 生产审计未完成，T002 继续
unchecked。
此前 r3 合并 focused selectors 为 **874 passed in 51.75s**；JUnit 仍只记录
本地组件/命令边界，不能替代实际 MiniNDN 或 Tiger 运行。最新 r5 结果见
下方 preflight checkpoint。

2026-09-07 T002 preflight checkpoint：新增真实的两阶段
`ndnsf-di-spec183-preflight`。输入阶段验证 archive-backed source seal 和
完整 Spec183 harness（包括实际 `jobs/yolo/run.sbatch`）；SIF 阶段验证精确
digest/labels，并在候选 SIF 内检查 Python/native imports、ORT CPU provider、
entrypoints 和 `ldd`。缺失 harness、参数或运行时闭包时 fail closed；26 项
新增/相关边界测试通过。合并 focused selectors 后 **878 passed in 51.67s**
（`results/t002-yolo-dispatch-r5/full-junit.xml`）。这没有制造 SIF 或集群
证据；T002、T007、T010、T011 仍未闭合。
2026-09-07 application Sync naming hardening：新增
`runtime.yolo_profile.application_sync_prefix()`，由 projection、NFD route
setup 与 startup validation 共享校验 `applicationName + '/sync'`；拒绝相对名、
尾部斜杠、重复分隔符和 `/group` 退化。相关回归 71 项通过；合并注册 focused
selectors 后 **885 passed in 55.18s**。这是 routing-integrity/component evidence，
不是 native/SIF/MiniNDN/Tiger qualification；T005/T006/T007 仍未闭合。

2026-09-07 T007 production-wiring audit：新增
[design-code-convergence.md](evidence/design-code-convergence.md)。审计结果为
**BLOCKED / NOT READY**：组件边界和 885 项回归记录为 component-only，真实
profile、`run.sbatch`、五命令、候选 artifact、native/SIF/MiniNDN/Tiger 执行仍缺；
因此不提前关闭 T007，也不授权提交集群作业。

2026-09-07 T004 operator boundary checkpoint：`jobs/yolo/submit.py` 现在暴露
`check/prepare/local/submit/collect`，并由严格的隐藏 `run` 接收
`jobs/yolo/run.sbatch` 的 allocation。入口统一要求显式 profile、run-id、output
和 case；结构完整但缺少真实 dispatch/local-SIF/staging receipt 时只返回
`INCOMPLETE/NOT_EVALUATED`，不创建 run、不冻结 bundle、不调用 Apptainer/Slurm。
`prepare` 已接通未来 qualified dispatch 下的不可变 harness/run-plan 冻结路径，
`collect` 当时只绑定已有 verdict；当前真实 application/worker 仍未接线，
因此 T004 继续 unchecked。相关边界回归与 journal/bundle 测试 **57 passed**；
`run.sbatch` 已加入 harness 清单，但无 allocation 或未完成 T012 时 fail-closed。

2026-09-07 T006 collector handoff checkpoint：`collect` 不再只读取任意外部
`verdict.json`；它要求与 prepared run 绑定的 `collection-input.json`，严格解析
normal/expected-rejection 两种 handoff，并分别调用 `collect_normal_verdict` /
`finalize_expected_rejection`。成功结果带 `collectorSchema` 且原子不可覆盖写入；
收集失败只保留首个 `collection-failure.json`。新增 2 个命令边界回归，
`test_yolo_submit.py` 23 passed；这仍是 worker/fixture handoff 证据，没有真实
native、GPU、MiniNDN 或 Tiger receipt，因此 T006/T007 继续 unchecked。

2026-09-07 T006 handoff wiring checkpoint：新增
`runtime.yolo_operator.finalize_normal_collection()` 与
`runtime.yolo_collection.publish_normal_handoff()`。外层 coordinator 只有在完整
rank 返回后才能发布 `collection-input.json`；发布器重新读取并绑定每个真实
`node-receipt.json`，固定 node root、reference 目录、candidate/preparation digest，
GPU case 还要求 allocation/probe 文件和期望字段。6 个新增边界测试通过，随后
`test_yolo_submit.py` 23 passed；详见
[t006-collector-handoff.md](evidence/t006-collector-handoff.md)。这仍未连接实际
Slurm worker/collector，不能关闭 T006/T007，也不能声称有 native/GPU/MiniNDN/Tiger
证据。

2026-09-07 substrate input checkpoint：VPN/SSH 只读检查成功；远端 `bigTiger`
分区可见 `rtx_5000`/`rtx_6000`/`h100_80gb`，一个有界 `srun` 在 `itiger02` 实测
compute Apptainer `1.5.3-1.el9`，而登录节点仍为 `1.3.4`。因此本地 SIF 构建
必须等待并匹配 compute 版本，不能使用登录节点版本。远端项目目录未发现
Spec183 profile、签名模型包或 collector 输入，仅有旧 Spec170/Spec180 材料；
这些事实已记录到 [input-inventory.md](evidence/input-inventory.md)，不改变
T002/T007 的资格状态，也未启动模型或 SIF 构建。

2026-09-07 matching tool/input checkpoint：本机显式 `/opt/apptainer/1.5.3/bin/apptainer`
与 compute 节点版本一致；对锁定历史 base SIF 完成 `sif list`、label 和 `/bin/true`
执行。source handoff `verify` 返回 `SOURCE_READY`，临时 definition 可正常渲染，
但在 Spec183 dispatch/model gate 闭合前没有调用 `apptainer build`。从 Tiger 只读取
公开 YOLO package（不含 private key），逐项 hash 与历史记录一致，catalogue
Ed25519 signature 已验证；其外部 model manifest 仍错误绑定 `atomic-v1` 且没有签名
envelope，不能直接作为 shared-backbone Spec183 dispatch 输入。该残余缺口记录于
[input-inventory.md](evidence/input-inventory.md) 和
[spec180-candidate-reuse-audit.md](evidence/spec180-candidate-reuse-audit.md)；T004/T007/T008+
仍未完成，未提交任何 Tiger 作业。
