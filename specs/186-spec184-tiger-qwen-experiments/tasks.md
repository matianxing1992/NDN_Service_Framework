# Tasks: Spec184 YOLO/Qwen Cross-Host Experiment Closure

**Input**: Design documents from `/specs/186-spec184-tiger-qwen-experiments/`

**Prerequisites**: [spec.md](spec.md), [plan.md](plan.md), [candidate-identity.md](contracts/candidate-identity.md), [experiment-profile.md](contracts/experiment-profile.md), [validation-matrix.md](validation-matrix.md)

**Status**: IN_PROGRESS. 本任务表只描述 Spec186 的新候选；Spec183 的 v56 PASS、575 提交前的旧 smoke 和任何 Spec185 文件都不能直接关闭本表任务。当前本机与 Tiger compute 的 Apptainer 运行边界已统一为 1.5.3；r38 已证明 compute 侧可完成 native/SIF build/import，但在 MiniNDN 前的 canonical catalogue wrapper 边界停止；资格实验仍受本地优先 SIF promotion、仓库路由、模型和外部运行输入阻断。

## Dependency Graph

```text
T001 → T002/T003 → T004 → T005 → T006 → T007 → T006.d → T009 → T010 → T012 → T013
                                      └→ T008 ────────┘
                                      └→ T011 ─────────┘
```

## Direct Qualification Path

MiniNDN + TigerCluster YOLO 是本任务表的直接目标。按上图执行时，主资格链只有
`T005 → T006 → T007 → T006.d → T009 → T010 → T012`：先闭合 native/base+app 运行时，
再做真实 MiniNDN，随后完成本地 exact-SIF promotion，再用相同 composition 做 Tiger 单节点 GPU、双节点 normal /
negative，最后做独立复跑。T001–T005、T006.a/b 的静态/build/ABI 子项和 T013
是保护与收口门；T006.c 的 exact base+app composition 是进入真实运行的必要前置，
但这些门的 `VERIFIED`/`PASS` 仍不表示 MiniNDN 或 Tiger 已运行。
T006.d 要求本地 1.5.3 exact-SIF smoke、封存和同 SHA 接收核对；本地路径被工具或
文件系统阻断时，才允许带失败 receipt 的 compute 1.5.3 例外构建。
T008/T011 是条件式 Qwen3-0.6B 辅助链，不能替代 YOLO 主链。没有 terminal、
numerical oracle、退出码和 cleanup 回执的结果只能保持 `WAITING_EXTERNAL_INPUT`、
`BLOCKED_AFTER_BOUNDARY` 或其他声明范围状态。

## Detailed Execution Progress

维护日期与 candidate/run/evidence 基线：2026-09-14。T001 与 T002 已完成基线
和只读审计；其余任务仍按证据门禁推进。
状态只表示本任务声明的范围；`VERIFIED`、`PASS` 和实验状态必须带新 candidate
digest、run ID、命令、节点/GPU、oracle、退出和 cleanup 证据。

**Latest candidate boundary (2026-09-15)**: r83 used source revision
`854c802379b63dc45a96b1a3caf2a7b4a872306f`, the stable base SIF
`sha256:1dd9626748b6fdbe93abf819a944e0628bf7a2b5feddcc562fe7d233f927e74c`,
and Apptainer 1.5.3. The container-native `./waf -j2` build passed `284/284`
and produced SIF
`sha256:275629970d771bee9f635099ec37d130fa89a8cff9211444d81d78de5ec7a7c2`.
The immutable read-only probe passed ten Python imports, native `ldd` and
RUNPATH checks, provider `--help`, the real YOLO MiniNDN replay `--help`, and
replay source-seal presence; the complete receipt is
`evidence/native-sif-r83-closure-20260915.md`. This advances T006.b for the
exact local SIF closure. T006.a remains blocked by the absent Qwen3-0.6B row;
T006.c still lacks a separately sealed matching-SDK application bundle, and
T006.d still lacks local CPU smoke plus immutable upload/same-SHA receive.
MiniNDN and Tiger qualification therefore remain blocked. The r80/r81 runtime
boundary failures and r82 stale-handoff failure remain immutable history.

| Step | Parent | Concrete outcome / path | State | Evidence / verification scope | Blocker / next action | Reuse / rerun trigger |
| --- | --- | --- | --- | --- | --- | --- |
| T001.a | T001 | 获取并验证 `575b43cc93bbed29932303caf3d09974f1585af7` 对象、父提交、tree 和 clean checkout；记录于 `evidence/baseline-inventory.md` | VERIFIED | `evidence/baseline-inventory.md` source seal and ancestor receipt | — | 任何 source checkout 变化重做 |
| T001.b | T001 | 登记本机 CPU/RAM、编译器、Python、MiniNDN、Apptainer、Tiger account/partition/node/GPU/storage 可用性 | VERIFIED | `evidence/baseline-inventory.md` host/tool/model inventory plus `evidence/tiger-preflight-20260912.md`; Qwen3 remains `WAITING_EXTERNAL_INPUT` | Qwen3 and exact runtime inputs remain external | host/GPU/tool 变化重做 |
| T002.a | T002 | 使用 CodeGraph 检查 YOLO/Qwen MiniNDN、native provider/requester、Tiger launcher/collector 的实际调用链 | VERIFIED | `evidence/portability-audit.md` current path trace; CodeGraph index synchronized | T003/T004 后重新做 T005 convergence | 任何行为接线变化重做 |
| T002.b | T002 | 审计 localhost、固定 host/path、NFD socket、HOME/PIB/TPM、GPU ID、stage count、`LD_LIBRARY_PATH`、root 和共享 FS 依赖 | VERIFIED | `evidence/portability-audit.md` hidden cross-host constraints | T003/T004 must encode unresolved constraints | topology/profile/runtime 变化重做 |
| T002.c | T002 | 建立 Core/DI/app/Tiger/document ownership map，确认 Python 只编排、C++ 拥有业务推理 | VERIFIED | `evidence/portability-audit.md` ownership map | Product-path changes require T005 re-audit | ownership or API change重做 |
| T003.a | T003 | 在 `Experiments/TigerCluster/profiles/spec184-*.json` 实现 schema、case、role/node/GPU、timeouts、mount 和 model/backend 字段校验 | VERIFIED | `tests/test_spec186_candidate.py::test_all_declared_spec186_profiles_have_strict_schema` and mutation cases | — | schema or effective config change重做 |
| T003.b | T003 | 在 `Experiments/TigerCluster/runtime/` 与 `jobs/spec184/` 实现 candidate manifest、hash、变更平面和 earliest restart gate | VERIFIED | deterministic manifest and `earliest_restart_gate` mutation test | — | 任一 tuple plane变化重做 |
| T003.c | T003 | 实现 pre-dispatch closure 和 mutation tests；坏输入在 SSH/rsync/staging/`sbatch` 前拒绝且调用计数为零 | VERIFIED | `evidence/pre-dispatch-boundary-20260912.md` and `evidence/runtime-version-policy-20260913.md` cover all eight profiles; focused mutation and scheduler-spy tests observe zero remote counters, and the explicit 1.5.3 runtime contract rejects 1.3.4 profiles | native/ABI closure still blocks qualification | gate/collector/config change重做 |
| T004.a | T004 | 在 `Experiments/TigerCluster/jobs/spec184/` 实现 `check/prepare/local/submit/collect` 生命周期，显式 argv/env、run root、process ownership 和 bounded cleanup | VERIFIED | `submit.py` lifecycle commands and bounded timeout/reap test | actual remote run still required | launcher/lifecycle change重做 |
| T004.b | T004 | 将 MiniNDN 与 Tiger 的 node/identity/NFD route 映射参数化；禁止共享 FS 替代 dependency Data | VERIFIED | effective config transport/bind assertion and explicit role/node map | staging paths need Tiger receipt | route/identity/bind change重做 |
| T004.c | T004 | 覆盖 partial startup、child early exit、leader death、signal、rank skew、completion barrier 和 cleanup write failure | VERIFIED | bounded timeout process-group test; multi-rank failures await Tiger adapter | full two-node failure campaign open | deadline/cleanup change重做 |
| T005.a | T005 | 完成生产接线审计：CodeGraph caller、effective config、backend、security grant、oracle、collector 和终态逐项映射 FR/SC | VERIFIED | `evidence/design-code-convergence.md` checkpoints 7, 15 and 16; implementation wiring, nested profile ownership, executable role semantics and terminal evidence requirements are closed, while runtime qualification remains separately blocked | exact SIF and external runtime evidence belong to T006–T012 | 受影响行为修复后重审 |
| T005.b | T005 | 修复 T005 controlling gaps 并运行 focused regression，重新审计直到 `PASS` | VERIFIED | checkpoints 7–8, 11, 15–20 and `evidence/static-experiment-cycle-20260913.md` record provider help repair, native closure, collector identity repair, streamed collaboration repair, strict nested profile/manifest/terminal receipt validation, resource/run-root boundaries, harness format checks, terminal GPU-role policy and scheduler identity binding; 55 focused Spec186 tests pass, including the no-temporary-toolchain-default regression | qualification must not be promoted without T006–T012 receipts | 任一行为变更重新执行 |
| T006.pre | T006 | 先逐项比对 `successful-tiger-gpu-template.md` 的 base/compiler/Apptainer/multi-stage tuple，记录差异，再在原生编译前运行 `prepare-development-handoff.py render`、shell/Python/boundary 和 `preflight-development-sif.py`，交叉核对 rendered definition、四个源码归档的必需入口与 `tar -xf` 解包根、builder 实际消费的全部 `/src/...` 子路径、definition 显式 `SPEC186_BASE_CAPABILITY_BEGIN/END` 块声明的 base ONNX/Rust/头文件及预装 Python 包能力、APT 后置编译器检查、固定 wheels、NumPy wheel-private DSO/RPATH 和 base-SIF 导入，并审计当前 Core/DI/ONNX/YOLO/Qwen/assembly-worker 链接边界 | VERIFIED | `evidence/successful-template-comparison-20260915.md` records the exact base SHA, r51 renderer output/diff, compiler/toolchain/stage invariants and compute preflight; `evidence/static-build-preflight-20260915.md` records the r70 source seal/definition, expanded consumer/wheel/base checks and exact base identity; `evidence/base-sif-digest-drift-r76-20260915.md` records the later same-size base drift and fail-closed render; `evidence/base-sif-disk-boundary-r78-20260915.md` records the r78 extraction failure and the new 16-GiB/four-times-base capacity gate; `Experiments/TigerCluster/docs/dependency-boundaries.md` records the source-archive consumer/capability guards, current monolithic Waf graph and `--verify-existing` reuse path; 117 full TigerCluster tests pass before the latest template-only gate; this gate still does not qualify MiniNDN/Tiger | native build follows only after this receipt; no SIF/runtime result is implied; existing SIF may be re-verified without recompilation when candidate identity is unchanged | definition, handoff, template tuple, dependency graph or base-SIF change重做 |
| T006.a | T006 | 按锁定 builder 和 `-j4` 上限完成依赖、Core/Repo/DI/native extension 的完整 unit/integration 验证；测试 target census 必须包含 `di-native-assembly-worker` 和五个 `spec182-worker-tool-*` fixture，并以 `NDNSF_SPEC182_BIN_DIR` 绑定同一 build tree | BLOCKED_AFTER_BOUNDARY | `evidence/t006-unit-integration-20260915.md` records the repaired `327/327` Waf target build, full unit `1047/1047` pass, worker protocol `29/29` pass and activation `9/9` pass; integration is `171/173` because the two Qwen real-provider rows cannot open the absent Qwen3-0.6B source artifact | supply the declared Qwen3-0.6B stage/model/tokenizer tuple and rerun the complete integration campaign | source/toolchain/ABI/fixture/model变更重建 |
| T006.b | T006 | 对 C++ binaries、`_ndnsf.so`、入口 `--help` 执行 import、`readelf -d`、`ldd -r`、RPATH/RUNPATH、SONAME closure | VERIFIED | `evidence/native-sif-r83-closure-20260915.md` records the source-sealed local 1.5.3 SIF: ten Python imports, provider/replay help, all packaged native `ldd` checks and no `/src/`, `/tmp/` or `/home/` RUNPATH leak; the r64 host matrix remains supporting history | exact local SIF closure is green; CPU smoke, promotion and MiniNDN/Tiger execution remain separate gates | binary/loader/base change重做 |
| T006.c | T006 | 构建或复用稳定 base SIF，并在匹配 SDK/container 中构建只读 application bundle；记录 base/app 分层 hash | BLOCKED_AFTER_BOUNDARY | `evidence/native-build-r61-20260915.md` records host bundle `4a13a2fbc40d5825c94ecef71bd4522321663567da2fc0c1cd339d8f9851de3c` from source `81096f3e`; its tree digest is self-consistent, but its ELF RUNPATH contains host-only prefixes and the current r64 source seal requires a rebuild. The r64 handoff/preflight receipt records the current base and definition identities; no matching-SDK container app closure or local-first exact composition exists. The r6 and r38 receipts remain historical. | rebuild app inside the matching container against the r64 seal, then complete local 1.5.3 SIF/import/CPU smoke, immutable upload and same-SHA receive verification | base ABI重建；app-only仅重建 app |
| T006.d | T006 | 按本地优先规则完成 SIF exact-SHA promotion：本地 1.5.3 build/import/CPU smoke → 封存 → 上传 → Tiger compute 1.5.3 接收核对；例外必须绑定首失败和新 candidate | BLOCKED_AFTER_BOUNDARY | `evidence/native-sif-r83-closure-20260915.md` records the local 1.5.3 build and immutable import/loader/help closure, while no CPU smoke, immutable upload or same-SHA Tiger receive receipt exists; r38 remains the historical compute-build/import boundary | run the bounded local CPU smoke, then upload and verify the same SIF SHA on Tiger; do not run MiniNDN/Tiger qualification before this chain closes | SIF bytes/definition/upload target/runtime change重做 |
| T007.a | T007 | 使用新 candidate 运行 YOLO MiniNDN Y-A/Y-B；保留真实 ACK/Selection、依赖 Data、terminal response 和独立 numerical oracle | WAITING_EXTERNAL_INPUT | `evidence/direct-target-audit-20260913.md`; staged Y-A/Y-B package, trust/key maps, topology/config pass `validate_inputs` and process-vector construction; no MiniNDN startup yet | complete native/app closure and canonical case inputs required; SIF promotion is the following T006.d gate | candidate/model/harness变化重跑 |
| T007.b | T007 | 运行 YOLO MiniNDN Y-N 注册拒绝和 dependency-negative；记录唯一 edge、native failure、无响应/重选和 cleanup | WAITING_EXTERNAL_INPUT | `evidence/direct-target-audit-20260913.md`; staged Y-N case bundle is sealed, but live negative path is not started | complete native/app closure and canonical case inputs required; SIF promotion is the following T006.d gate | negative contract/collector变化重做 |
| T007.c | T007 | 离线重算 Y-A/Y-B/Y-N receipt，确认旧 Spec183 evidence 未被引用为当前候选 PASS | VERIFIED | boundary receipt contains no protocol PASS and no old Spec183 promotion | rerun after candidate is executable | evidence parser/oracle变化重做 |
| T008.a | T008 | 核对 Qwen3-0.6B 权重、tokenizer、stage manifest、格式（ONNX 或 GGUF/Q3）、backend、容量和 digest | VERIFIED | `evidence/minindn-qwen06b-inventory.md`; Qwen3 artifact absent, Qwen2.5 explicitly rejected | supply compatible Qwen3-0.6B tuple | model/tokenizer/backend变化重做 |
| T008.b | T008 | 在真实模型可用时运行 `NDNSF_DI_Qwen06B_Native_Minindn.py` 的 `check → prepare → run/local`，完成冷请求和 follow-up conversation | WAITING_EXTERNAL_INPUT | no stage manifest/model/backend available; execution intentionally not attempted | model/tokenizer/stage manifest required | model/app/native runner变化重跑 |
| T008.c | T008 | 记录 Qwen 结果边界：`LOCAL_CPU_PASS`、`SMOKE_ONLY` 或 `WAITING_EXTERNAL_INPUT`；不得关闭 Qwen3.6-27B 外部资格 | VERIFIED | inventory receipt marks `WAITING_EXTERNAL_INPUT`; 27B row untouched | asset arrival triggers new candidate | 资产可用/格式改变重评估 |
| T009.a | T009 | 进行 Tiger compute、Slurm、Apptainer、capacity、GPU UUID、NFD route、project storage 和 staged candidate preflight | VERIFIED | `evidence/tiger-preflight-20260912.md`, `evidence/runtime-version-policy-20260913.md`, `evidence/host-m01-r55-g3-20260913.md`, `evidence/tiger-r38-build-boundary-20260914.md` and `evidence/tiger-r38-import-shadow-fix-20260915.md`; compute `/home/tma1/.local/bin/apptainer-1.5.3` 1.5.3、`itiger02` RTX 6000 Ada/GPU UUID、Slurm allocation、r38 final SIF build/runtime import pass and raw runner diagnosis; login `/usr/bin/apptainer` 1.3.4 remains metadata-only | local-first promotion, rebuilt candidate, staged case/catalogue identity and NFD route still need a run-bound receipt | host/GPU/Apptainer/staging变化重做 |
| T009.b | T009 | 使用 exact base+app 在单节点完成 YOLO 1 warmup + 1 measured；模型角色 observed CUDA、Merge observed CPU | BLOCKED_AFTER_BOUNDARY | r38 stopped before MiniNDN at canonical catalogue verification; `evidence/tiger-r38-import-shadow-fix-20260915.md` records the raw source/package shadow cause and local repair, but no rebuilt candidate, CUDA/model-role or numerical result exists | rebuild and reseal the candidate, then rerun the exact entrypoint after T006.d | SIF/app/profile/GPU变化重跑 |
| T009.c | T009 | 收集 single-node protocol、backend/GPU、oracle、exit、cleanup 并与 candidate digest 对齐 | BLOCKED_AFTER_BOUNDARY | r38 has Slurm exit 78 and preserved build/import logs but no protocol, oracle or cleanup receipt for a started MiniNDN run | do not classify r38 as `SINGLE_NODE_GPU_PASS`; rerun after T006.d and T009.b pass | collector/evidence contract变化重做 |
| T010.a | T010 | 在两个真实节点运行 YOLO normal graph；验证四 Provider、跨节点 dependency Data、1 warmup + 3 measured 和 oracle | NOT_STARTED | V10 two-node normal receipt | 任何角色落错节点、共享 FS 注入或跨节点 Data缺失阻断 | topology/route/role map变化重跑 |
| T010.b | T010 | 运行 dependency-negative，fault 绑定唯一 producer/consumer edge；completion barrier 在所有 rank ready 后启动 | NOT_STARTED | V11 exact cutpoint and bounded cleanup | rank skew、edge cardinality或无 User observation保留 FAILED | negative budget/collector/native change重跑 |
| T010.c | T010 | reconcile 两节点 normal/negative，保留首失败，不把 partial withheld/transport/READY 升级为 PASS | NOT_STARTED | `evidence/tiger-two-node-*.md` | run/job状态未知先查询，不盲重提 | candidate/effective profile变化重做 |
| T011.a | T011 | 只有 T008 的模型/backend/资源门通过时，生成 Qwen Tiger profile、stage placement、GPU/CPU policy 和 bounded budget | WAITING_EXTERNAL_INPUT | blocked by T008 Qwen3 artifact and T009 Tiger resource closure | do not submit until both inputs arrive | model/resource/backend变化重做 |
| T011.b | T011 | 在条件满足时运行一次 Qwen Tiger smoke/functional composition，并记录跨节点或单节点实际边界 | NOT_STARTED | V12 run evidence, never YOLO reuse | 没有实际模型或 backend mismatch不得提交 | profile/SIF/model变化重跑 |
| T011.c | T011 | 若条件不满足，生成可重放的 waiting/blocked receipt，说明缺项、查询命令和恢复入口 | VERIFIED | `evidence/minindn-qwen06b-inventory.md` lists missing model/tokenizer/stage/backend and recovery boundary | external input到位后新 run | external input到位后新 run |
| T012.a | T012 | 不修改正常 profile、SIF、app、harness、model、input、oracle，在新 allocation 复跑两节点 normal | NOT_STARTED | V13 new allocation/run/identity | T010 normal未PASS时不得启动 | 任一 candidate plane变化需回到对应 gate |
| T012.b | T012 | 比较两次 candidate/profile/artifact/graph hashes，并确认新 host/GPU/identity、请求数和 cleanup 独立 | NOT_STARTED | reuse comparison report | hash不一致、结果缺字段或旧结果复用为 FAILED | comparison/parser change重做 |
| T012.c | T012 | 将失败按 code/ABI/packaging/profile/transport/loader/resource/app/harness/model 分类，提出最小下一步 | NOT_STARTED | diagnosis receipt and failure-log entry | 首失败不可定位时保留 BLOCKED_AFTER_BOUNDARY | 新失败只重跑受影响最早门 |
| T013.a | T013 | 更新 `spec.md`、`plan.md`、`tasks.md`、`validation-matrix.md`、contracts、quickstart、successful-template comparison 和 `AGENTS.md` managed pointer（如扩展可用） | VERIFIED | current docs, successful-template comparison receipt, corrected executable quickstart and `AGENTS.md` active-plan pointer; strict Spec Kit structure audit passes | pointer或路径不一致阻断 handoff | spec/plan/task change重跑 analyze |
| T013.b | T013 | 把每次非平凡失败写入 `docs/failure-log.md`，完成离线 hash/oracle/reconciliation 和 evidence index | VERIFIED | `docs/failure-log.md`, `evidence/README.md`, immutable r48–r51 failure chain, r53/r55 regression receipts and repaired all-profile pre-dispatch digest receipts including source/app path binding | 失败被覆盖或仅存在聊天记录阻断 | 新失败/证据 parser变化重做 |
| T013.c | T013 | 生成 Spec186 closure/handoff，明确 implemented/wired/executed/measured、未完成 external rows 和下一步 | VERIFIED | `evidence/closure-handoff-20260914.md` records the r38 build/import boundary and ordered next gates; the r83 closure receipt and 2026-09-15 failure-log entry add the repaired local candidate and immutable probe environment correction without upgrading MiniNDN/Tiger/Qwen rows | 任一必需 gate未完成则保持 IN_PROGRESS | final candidate变化重新闭合 |

## Tasks

- [x] T001 [US1] Freeze the exact `575b43cc93bbed29932303caf3d09974f1585af7` source baseline, create the `SPEC184Experiments` checkout intent, and record host/tool/model/resource inventory in `specs/186-spec184-tiger-qwen-experiments/evidence/baseline-inventory.md`.
- [x] T002 [US1] Complete the CodeGraph production-path and hidden cross-host constraint audit, classify legitimate isolation versus accidental host coupling, and record ownership in `specs/186-spec184-tiger-qwen-experiments/evidence/portability-audit.md`.
- [x] T003 [US1] Implement the Spec186 profile, candidate manifest, change-plane invalidation and zero-side-effect pre-dispatch gate in `Experiments/TigerCluster/profiles/`, `Experiments/TigerCluster/runtime/` and `Experiments/TigerCluster/jobs/spec184/`, with focused mutation coverage.
- [x] T004 [US4] Implement the portable `check/prepare/local/submit/collect` lifecycle and explicit MiniNDN/Tiger node, identity, NFD, route, deadline and cleanup adapters under `Experiments/TigerCluster/jobs/spec184/`, with bounded failure coverage.
- [x] T005 [US1] Run the post-implementation design-to-code convergence audit against the Spec186 contracts and real callers, repair all controlling gaps, and close `evidence/design-code-convergence.md` only at `PASS`.
- [ ] T006 [US1] Run the cheap source/definition/base-SIF preflight, then build and verify the locked native runtime and layered base SIF plus read-only application bundle, including complete unit/integration validation and `import`, `--help`, `readelf`, `ldd -r`, RPATH and SONAME closure evidence; keep local 1.5.3 promotion as the default.
- [ ] T007 [US2] Execute fresh YOLO MiniNDN Y-A/Y-B normal and Y-N negative scenarios with native process, dependency, numerical oracle, terminal and cleanup evidence bound to the Spec186 candidate.
- [ ] T008 [US3] Inventory and, when compatible artifacts exist, execute the Qwen3-0.6B CPU MiniNDN cold and follow-up conversation path; otherwise record a reproducible `SMOKE_ONLY` or `WAITING_EXTERNAL_INPUT` boundary without closing the 27B row.
- [ ] T009 [US4] Preflight and execute one exact-composition Tiger single-node GPU YOLO run, requiring observed CUDA model roles, CPU Merge, numerical oracle, process exits and clean cleanup.
- [ ] T010 [US4] Execute the first exact-composition Tiger two-node YOLO normal and dependency-negative runs, preserving unique edge identity, shared readiness budget, first failure and terminal cleanup evidence.
- [ ] T011 [US3] Run the conditional Tiger Qwen3-0.6B composition only after model/backend/resource closure; otherwise retain an explicit waiting receipt and do not substitute YOLO or fixture evidence.
- [ ] T012 [US5] Perform an independent two-node normal reuse allocation with unchanged candidate planes, compare all hashes and identities, and write a first-boundary diagnosis for any failure.
- [ ] T013 [US5] Reconcile all evidence offline, append non-trivial failures to `docs/failure-log.md`, synchronize Spec186 documents and pointers, and publish a closure/handoff that separates implemented, wired, executed and measured states.

## Execution Rules

- T005 `PASS` is a hard prerequisite for the direct T006–T012 qualification path;
  T013 only reconciles evidence and does not turn a preflight into a runtime result.
- Every failed or uncertain run receives a new run ID; the original receipt is immutable.
- App-only changes may reuse an unchanged base SIF only after application ABI and candidate closure pass; base ABI/toolchain changes force affected consumer rebuilds.
- Local SIF build/import/CPU smoke and immutable upload with same-SHA compute verification are mandatory T006.d evidence; a compute-built SIF is an explicitly recorded exception, never an implicit replacement.
- `LOCAL_CPU_PASS`, `SINGLE_NODE_GPU_PASS`, `EXPECTED_REJECTION_PASS`, `BLOCKED_AFTER_BOUNDARY` and `WAITING_EXTERNAL_INPUT` are scope-limited states, not synonyms for complete Spec186 qualification.
- Do not commit SIFs, model weights, tokenizers, private keys or large logs. Evidence receipts contain hashes and external paths only.
