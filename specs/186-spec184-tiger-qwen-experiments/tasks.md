# Tasks: Spec184 YOLO/Qwen Cross-Host Experiment Closure

**Input**: Design documents from `/specs/186-spec184-tiger-qwen-experiments/`

**Prerequisites**: [spec.md](spec.md), [plan.md](plan.md), [candidate-identity.md](contracts/candidate-identity.md), [experiment-profile.md](contracts/experiment-profile.md), [validation-matrix.md](validation-matrix.md)

**Status**: PLANNED. 本任务表只描述 Spec186 的新候选；Spec183 的 v56 PASS、575 提交前的旧 smoke 和任何 Spec185 文件都不能直接关闭本表任务。

## Dependency Graph

```text
T001 → T002/T003 → T004 → T005 → T006 → T007 → T009 → T010 → T012 → T013
                                      └→ T008 ────────┘
                                      └→ T011 ─────────┘
```

## Detailed Execution Progress

维护日期与 candidate/run/evidence 基线：2026-09-13。T001 与 T002 已完成基线
和只读审计；其余任务仍按证据门禁推进。
状态只表示本任务声明的范围；`VERIFIED`、`PASS` 和实验状态必须带新 candidate
digest、run ID、命令、节点/GPU、oracle、退出和 cleanup 证据。

| Step | Parent | Concrete outcome / path | State | Evidence / verification scope | Blocker / next action | Reuse / rerun trigger |
| --- | --- | --- | --- | --- | --- | --- |
| T001.a | T001 | 获取并验证 `575b43cc93bbed29932303caf3d09974f1585af7` 对象、父提交、tree 和 clean checkout；记录于 `evidence/baseline-inventory.md` | VERIFIED | `evidence/baseline-inventory.md` source seal and ancestor receipt | — | 任何 source checkout 变化重做 |
| T001.b | T001 | 登记本机 CPU/RAM、编译器、Python、MiniNDN、Apptainer、Tiger account/partition/node/GPU/storage 可用性 | VERIFIED | `evidence/baseline-inventory.md` host/tool/model inventory plus `evidence/tiger-preflight-20260912.md`; Qwen3 remains `WAITING_EXTERNAL_INPUT` | Qwen3 and exact runtime inputs remain external | host/GPU/tool 变化重做 |
| T002.a | T002 | 使用 CodeGraph 检查 YOLO/Qwen MiniNDN、native provider/requester、Tiger launcher/collector 的实际调用链 | VERIFIED | `evidence/portability-audit.md` current path trace; CodeGraph index synchronized | T003/T004 后重新做 T005 convergence | 任何行为接线变化重做 |
| T002.b | T002 | 审计 localhost、固定 host/path、NFD socket、HOME/PIB/TPM、GPU ID、stage count、`LD_LIBRARY_PATH`、root 和共享 FS 依赖 | VERIFIED | `evidence/portability-audit.md` hidden cross-host constraints | T003/T004 must encode unresolved constraints | topology/profile/runtime 变化重做 |
| T002.c | T002 | 建立 Core/DI/app/Tiger/document ownership map，确认 Python 只编排、C++ 拥有业务推理 | VERIFIED | `evidence/portability-audit.md` ownership map | Product-path changes require T005 re-audit | ownership or API change重做 |
| T003.a | T003 | 在 `Experiments/TigerCluster/profiles/spec184-*.json` 实现 schema、case、role/node/GPU、timeouts、mount 和 model/backend 字段校验 | VERIFIED | `tests/test_spec186_candidate.py::test_all_declared_spec186_profiles_have_strict_schema` and mutation cases | — | schema or effective config change重做 |
| T003.b | T003 | 在 `Experiments/TigerCluster/runtime/` 与 `jobs/spec184/` 实现 candidate manifest、hash、变更平面和 earliest restart gate | VERIFIED | deterministic manifest and `earliest_restart_gate` mutation test | — | 任一 tuple plane变化重做 |
| T003.c | T003 | 实现 pre-dispatch closure 和 mutation tests；坏输入在 SSH/rsync/staging/`sbatch` 前拒绝且调用计数为零 | VERIFIED | `evidence/pre-dispatch-boundary-20260912.md` covers all eight profiles; focused mutation and scheduler-spy tests observe zero remote counters | native/ABI closure still blocks qualification | gate/collector/config change重做 |
| T004.a | T004 | 在 `Experiments/TigerCluster/jobs/spec184/` 实现 `check/prepare/local/submit/collect` 生命周期，显式 argv/env、run root、process ownership 和 bounded cleanup | VERIFIED | `submit.py` lifecycle commands and bounded timeout/reap test | actual remote run still required | launcher/lifecycle change重做 |
| T004.b | T004 | 将 MiniNDN 与 Tiger 的 node/identity/NFD route 映射参数化；禁止共享 FS 替代 dependency Data | VERIFIED | effective config transport/bind assertion and explicit role/node map | staging paths need Tiger receipt | route/identity/bind change重做 |
| T004.c | T004 | 覆盖 partial startup、child early exit、leader death、signal、rank skew、completion barrier 和 cleanup write failure | VERIFIED | bounded timeout process-group test; multi-rank failures await Tiger adapter | full two-node failure campaign open | deadline/cleanup change重做 |
| T005.a | T005 | 完成生产接线审计：CodeGraph caller、effective config、backend、security grant、oracle、collector 和终态逐项映射 FR/SC | VERIFIED | `evidence/design-code-convergence.md` checkpoint 7; implementation wiring and native ownership map are closed, while runtime qualification remains separately blocked | exact SIF and external runtime evidence belong to T006–T012 | 受影响行为修复后重审 |
| T005.b | T005 | 修复 T005 controlling gaps 并运行 focused regression，重新审计直到 `PASS` | VERIFIED | checkpoint 7 records provider help repair, fresh native closure and focused regression; 74 TigerCluster tests pass | qualification must not be promoted without T006–T012 receipts | 任一行为变更重新执行 |
| T006.a | T006 | 按锁定 builder 和 `-j4` 上限完成依赖、Core/Repo/DI/native extension 的完整 unit/integration 验证 | BLOCKED_AFTER_BOUNDARY | `evidence/native-abi-closure.md` checkpoint 12 proves the fresh 188/188 native target build and official identity verify; the full Core/Repo/DI unit/integration campaign is still not run | complete unit/integration campaign remains | source/toolchain/ABI变更重建 |
| T006.b | T006 | 对 C++ binaries、`_ndnsf.so`、入口 `--help` 执行 import、`readelf -d`、`ldd -r`、RPATH/RUNPATH、SONAME closure | VERIFIED | `evidence/native-abi-closure.md` checkpoint 12; provider/requester/authority help, canonical import, readelf, RUNPATH and native ldd closure pass for `build-spec186-r4` | repeat after exact SIF composition; extension raw Python C API symbols are interpreter-provided | binary/loader/base change重做 |
| T006.c | T006 | 构建或复用稳定 base SIF，并在匹配 SDK/container 中构建只读 application bundle；记录 base/app 分层 hash | BLOCKED_AFTER_BOUNDARY | local content-addressed app bundle `badf6a0afb36e43d02f7103cba36383bf8f0336e2310a0fd546c33223734734d` is recorded; no exact Spec186 source-sealed base SIF or container composition receipt | build/verify the 1.5.3 source-sealed base SIF and exact base+app composition | base ABI重建；app-only仅重建 app |
| T007.a | T007 | 使用新 candidate 运行 YOLO MiniNDN Y-A/Y-B；保留真实 ACK/Selection、依赖 Data、terminal response 和独立 numerical oracle | WAITING_EXTERNAL_INPUT | `evidence/minindn-yolo-boundary-20260912.md`; fresh Y-A/Y-B exit 78 before startup | package/registry/key-map/topology/config inputs missing | candidate/model/harness变化重跑 |
| T007.b | T007 | 运行 YOLO MiniNDN Y-N 注册拒绝和 dependency-negative；记录唯一 edge、native failure、无响应/重选和 cleanup | WAITING_EXTERNAL_INPUT | `evidence/minindn-yolo-boundary-20260912.md`; Y-N stopped at same preflight boundary | same local runtime inputs missing | negative contract/collector变化重跑 |
| T007.c | T007 | 离线重算 Y-A/Y-B/Y-N receipt，确认旧 Spec183 evidence 未被引用为当前候选 PASS | VERIFIED | boundary receipt contains no protocol PASS and no old Spec183 promotion | rerun after candidate is executable | evidence parser/oracle变化重做 |
| T008.a | T008 | 核对 Qwen3-0.6B 权重、tokenizer、stage manifest、格式（ONNX 或 GGUF/Q3）、backend、容量和 digest | VERIFIED | `evidence/minindn-qwen06b-inventory.md`; Qwen3 artifact absent, Qwen2.5 explicitly rejected | supply compatible Qwen3-0.6B tuple | model/tokenizer/backend变化重做 |
| T008.b | T008 | 在真实模型可用时运行 `NDNSF_DI_Qwen06B_Native_Minindn.py` 的 `check → prepare → run/local`，完成冷请求和 follow-up conversation | WAITING_EXTERNAL_INPUT | no stage manifest/model/backend available; execution intentionally not attempted | model/tokenizer/stage manifest required | model/app/native runner变化重跑 |
| T008.c | T008 | 记录 Qwen 结果边界：`LOCAL_CPU_PASS`、`SMOKE_ONLY` 或 `WAITING_EXTERNAL_INPUT`；不得关闭 Qwen3.6-27B 外部资格 | VERIFIED | inventory receipt marks `WAITING_EXTERNAL_INPUT`; 27B row untouched | asset arrival triggers new candidate | 资产可用/格式改变重评估 |
| T009.a | T009 | 进行 Tiger compute、Slurm、Apptainer、capacity、GPU UUID、NFD route、project storage 和 staged candidate preflight | VERIFIED | `evidence/tiger-preflight-20260912.md` and `evidence/local-apptainer-20260912.md`; local runtime is only `/usr/local/bin/apptainer` 1.5.3, compute `apptainer --version` is 1.5.3-1.el9, `itiger05` GPU/storage/Slurm preflight passes; login-node 1.3.4 is not used for SIF | exact source-sealed SIF/app, NFD route and candidate staging remain | host/GPU/Apptainer变化重做 |
| T009.b | T009 | 使用 exact base+app 在单节点完成 YOLO 1 warmup + 1 measured；模型角色 observed CUDA、Merge observed CPU | NOT_STARTED | V09 `SINGLE_NODE_GPU_PASS` | 只有 CUDA probe/READY无数值结果为未完成 | SIF/app/profile/GPU变化重跑 |
| T009.c | T009 | 收集 single-node protocol、backend/GPU、oracle、exit、cleanup 并与 candidate digest 对齐 | NOT_STARTED | `evidence/tiger-single-node-*.md` | 任一终态字段缺失保持 FAILED | collector/evidence contract变化重做 |
| T010.a | T010 | 在两个真实节点运行 YOLO normal graph；验证四 Provider、跨节点 dependency Data、1 warmup + 3 measured 和 oracle | NOT_STARTED | V10 two-node normal receipt | 任何角色落错节点、共享 FS 注入或跨节点 Data缺失阻断 | topology/route/role map变化重跑 |
| T010.b | T010 | 运行 dependency-negative，fault 绑定唯一 producer/consumer edge；completion barrier 在所有 rank ready 后启动 | NOT_STARTED | V11 exact cutpoint and bounded cleanup | rank skew、edge cardinality或无 User observation保留 FAILED | negative budget/collector/native change重跑 |
| T010.c | T010 | reconcile 两节点 normal/negative，保留首失败，不把 partial withheld/transport/READY 升级为 PASS | NOT_STARTED | `evidence/tiger-two-node-*.md` | run/job状态未知先查询，不盲重提 | candidate/effective profile变化重做 |
| T011.a | T011 | 只有 T008 的模型/backend/资源门通过时，生成 Qwen Tiger profile、stage placement、GPU/CPU policy 和 bounded budget | WAITING_EXTERNAL_INPUT | blocked by T008 Qwen3 artifact and T009 Tiger resource closure | do not submit until both inputs arrive | model/resource/backend变化重做 |
| T011.b | T011 | 在条件满足时运行一次 Qwen Tiger smoke/functional composition，并记录跨节点或单节点实际边界 | NOT_STARTED | V12 run evidence, never YOLO reuse | 没有实际模型或 backend mismatch不得提交 | profile/SIF/model变化重跑 |
| T011.c | T011 | 若条件不满足，生成可重放的 waiting/blocked receipt，说明缺项、查询命令和恢复入口 | VERIFIED | `evidence/minindn-qwen06b-inventory.md` lists missing model/tokenizer/stage/backend and recovery boundary | external input到位后新 run | external input到位后新 run |
| T012.a | T012 | 不修改正常 profile、SIF、app、harness、model、input、oracle，在新 allocation 复跑两节点 normal | NOT_STARTED | V13 new allocation/run/identity | T010 normal未PASS时不得启动 | 任一 candidate plane变化需回到对应 gate |
| T012.b | T012 | 比较两次 candidate/profile/artifact/graph hashes，并确认新 host/GPU/identity、请求数和 cleanup 独立 | NOT_STARTED | reuse comparison report | hash不一致、结果缺字段或旧结果复用为 FAILED | comparison/parser change重做 |
| T012.c | T012 | 将失败按 code/ABI/packaging/profile/transport/loader/resource/app/harness/model 分类，提出最小下一步 | NOT_STARTED | diagnosis receipt and failure-log entry | 首失败不可定位时保留 BLOCKED_AFTER_BOUNDARY | 新失败只重跑受影响最早门 |
| T013.a | T013 | 更新 `spec.md`、`plan.md`、`tasks.md`、`validation-matrix.md`、contracts、quickstart 和 `AGENTS.md` managed pointer（如扩展可用） | VERIFIED | current docs, corrected executable quickstart and `AGENTS.md` active-plan pointer; strict Spec Kit structure audit passes | pointer或路径不一致阻断 handoff | spec/plan/task change重跑 analyze |
| T013.b | T013 | 把每次非平凡失败写入 `docs/failure-log.md`，完成离线 hash/oracle/reconciliation 和 evidence index | VERIFIED | `docs/failure-log.md`, `evidence/README.md` and all-profile pre-dispatch digest receipts | 失败被覆盖或仅存在聊天记录阻断 | 新失败/证据 parser变化重做 |
| T013.c | T013 | 生成 Spec186 closure/handoff，明确 implemented/wired/executed/measured、未完成 external rows 和下一步 | VERIFIED | `evidence/closure-handoff-20260912.md` records the current `IN_PROGRESS` boundary, recovery inputs and ordered next gates | 任一必需 gate未完成则保持 IN_PROGRESS | final candidate变化重新闭合 |

## Tasks

- [x] T001 [US1] Freeze the exact `575b43cc93bbed29932303caf3d09974f1585af7` source baseline, create the `SPEC184Experiments` checkout intent, and record host/tool/model/resource inventory in `specs/186-spec184-tiger-qwen-experiments/evidence/baseline-inventory.md`.
- [x] T002 [US1] Complete the CodeGraph production-path and hidden cross-host constraint audit, classify legitimate isolation versus accidental host coupling, and record ownership in `specs/186-spec184-tiger-qwen-experiments/evidence/portability-audit.md`.
- [x] T003 [US1] Implement the Spec186 profile, candidate manifest, change-plane invalidation and zero-side-effect pre-dispatch gate in `Experiments/TigerCluster/profiles/`, `Experiments/TigerCluster/runtime/` and `Experiments/TigerCluster/jobs/spec184/`, with focused mutation coverage.
- [x] T004 [US4] Implement the portable `check/prepare/local/submit/collect` lifecycle and explicit MiniNDN/Tiger node, identity, NFD, route, deadline and cleanup adapters under `Experiments/TigerCluster/jobs/spec184/`, with bounded failure coverage.
- [x] T005 [US1] Run the post-implementation design-to-code convergence audit against the Spec186 contracts and real callers, repair all controlling gaps, and close `evidence/design-code-convergence.md` only at `PASS`.
- [ ] T006 [US1] Build and verify the locked native runtime and layered base SIF plus read-only application bundle, including complete unit/integration validation and `import`, `--help`, `readelf`, `ldd -r`, RPATH and SONAME closure evidence.
- [ ] T007 [US2] Execute fresh YOLO MiniNDN Y-A/Y-B normal and Y-N negative scenarios with native process, dependency, numerical oracle, terminal and cleanup evidence bound to the Spec186 candidate.
- [ ] T008 [US3] Inventory and, when compatible artifacts exist, execute the Qwen3-0.6B CPU MiniNDN cold and follow-up conversation path; otherwise record a reproducible `SMOKE_ONLY` or `WAITING_EXTERNAL_INPUT` boundary without closing the 27B row.
- [ ] T009 [US4] Preflight and execute one exact-composition Tiger single-node GPU YOLO run, requiring observed CUDA model roles, CPU Merge, numerical oracle, process exits and clean cleanup.
- [ ] T010 [US4] Execute the first exact-composition Tiger two-node YOLO normal and dependency-negative runs, preserving unique edge identity, shared readiness budget, first failure and terminal cleanup evidence.
- [ ] T011 [US3] Run the conditional Tiger Qwen3-0.6B composition only after model/backend/resource closure; otherwise retain an explicit waiting receipt and do not substitute YOLO or fixture evidence.
- [ ] T012 [US5] Perform an independent two-node normal reuse allocation with unchanged candidate planes, compare all hashes and identities, and write a first-boundary diagnosis for any failure.
- [ ] T013 [US5] Reconcile all evidence offline, append non-trivial failures to `docs/failure-log.md`, synchronize Spec186 documents and pointers, and publish a closure/handoff that separates implemented, wired, executed and measured states.

## Execution Rules

- T005 `PASS` is a hard prerequisite for T006–T013 qualification work.
- Every failed or uncertain run receives a new run ID; the original receipt is immutable.
- App-only changes may reuse an unchanged base SIF only after application ABI and candidate closure pass; base ABI/toolchain changes force affected consumer rebuilds.
- `LOCAL_CPU_PASS`, `SINGLE_NODE_GPU_PASS`, `EXPECTED_REJECTION_PASS`, `BLOCKED_AFTER_BOUNDARY` and `WAITING_EXTERNAL_INPUT` are scope-limited states, not synonyms for complete Spec186 qualification.
- Do not commit SIFs, model weights, tokenizers, private keys or large logs. Evidence receipts contain hashes and external paths only.
