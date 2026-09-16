# Tasks: YOLO MiniNDN SIF+APP Fast Path

**Spec**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md) | **Status**: PLANNED

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001 Candidate closure and pair mutation gate](#t001-candidate-closure-and-pair-mutation-gate) | PARTIAL | — | base smoke 和 APP SDK loader 正反例通过；34 脚本测试通过/1 skipped；ONNX/Rust 容器闭包、完整 APP/host-gate 与 pair 验收仍待执行；[SDK](evidence/b187-app-sdk.md)、[base repair](evidence/b187-base-repair.md)、[closure](evidence/b187-local-closure.md) | 2026-09-15 |
| [T002 C++ YOLO selector and MiniNDN caller wiring](#t002-c-yolo-selector-and-minindn-caller-wiring) | PARTIAL | T001 | r7 STATIC_PASS；C++ target compile/link 与独立 served-provider selector 通过；through-MiniNDN 仍需 candidate-bound native config/input；[b187-local-yolo.md](evidence/b187-local-yolo.md) | 2026-09-15 15:44 -05:00 |
| [T003 Local YOLO pair build and two-run gate](#t003-local-yolo-pair-build-and-two-run-gate) | PARTIAL | T002 | 本机负责增建完整 base SDK、NDNSF consumer、candidate config/input 和两次 through-MiniNDN run；不要求用户提供可本机生成的输入；[candidate](evidence/b187-complete-candidate.md) | 2026-09-15 |
| [T004 TigerCluster same-candidate promotion](#t004-tigercluster-same-candidate-promotion) | WAITING_EXTERNAL_INPUT | T003 | T003 尚未 LOCAL_PASS；未启动 Tiger/Slurm；[b187-tiger-yolo.md](evidence/b187-tiger-yolo.md) | 2026-09-15 |
| [T005 QWEN deferral and delivery record](#t005-qwen-deferral-and-delivery-record) | DONE | — | QWEN 明确保持 TODO，未进入 YOLO candidate；[qwen-deferred.md](evidence/qwen-deferred.md) | 2026-09-15 15:16 -05:00 |
| [T006 Design-code convergence and final evidence](#t006-design-code-convergence-and-final-evidence) | PARTIAL | T001,T002 | 两层交付改变构建边界，需重新核对受影响调用与契约；历史静态 PASS 保留，不能覆盖新方案；[convergence-20260915-r1.md](evidence/convergence-20260915-r1.md) | 2026-09-15 |

## Current Checkpoint

2026-09-16 r11 封装复制因缺少 fakeroot 无法读取三个容器私有运行目录，已中止并保留失败日志；r12 已补权限映射重试，原 final rootfs 与生产库保持不变。4 个恢复入口回归通过；最终镜像和模型验收仍未完成，T001/T003 PARTIAL。

2026-09-16 r11：base SDK 4278 项及实际编译 probe PASS，修复后的默认隔离 native verifier exit 0，已进入最终 SIF 封装；未重编 NDNSF。17 个脚本回归通过。最终 SIF 单元/YOLO 尚未运行，T001/T003 保持 PARTIAL，见 [candidate](evidence/b187-complete-candidate.md)。

2026-09-16 r9 默认容器加载失败已定位为继承 SDK 环境覆盖项目库目录。正在审查 92 环境修复及最终 SIF 默认环境门；仅恢复已完成的 r8 final rootfs，不重编生产库。用户明确最终镜像须自包含，不依赖宿主源码/库/home；测试仅挂载声明的测试输入和输出。T001/T003 保持 PARTIAL，C++ 单元与 YOLO 实测尚未通过。

2026-09-15 标准 r8 的完整 builder/final post 通过，但最终 SIF copy 因磁盘峰值不足失败，当前无有效候选。保留 final rootfs；只恢复封装并复验身份/ABI，不重编 Core/DI。最终镜像、容器单元、YOLO 仍未验收，T001/T003 PARTIAL。

2026-09-15 标准 r8 正在构建：正常两阶段入口/SDK/configure 已通过，C++ `-j4` 进行中。此前恢复产物已归档验证；不采用被拒绝的 final 恢复路径。容器单元 runner 与文档静态/组合通过，实际单元/YOLO 与最终 SIF 仍待验收。

2026-09-15 r7：Repo binding 与 builder ABI/import 检查通过；恢复 final definition 被正常交付门禁拒绝宿主二进制输入，尚无最终 SIF。配置根本修复已提交 `040c2dfd`，25 tests passed。下一步用修正后的正式两阶段模板重建 NDNSF 层、复用封存 base，再验收容器单元/YOLO；不放宽交付门禁。

2026-09-15 r6：Core/DI C++ 293 steps 与主 Python binding 通过；Repo binding metadata 在显式库目录契约处失败。正在修复 template 调用参数并准备从该步骤恢复；最终候选及容器内 C++ 单元/YOLO 验收仍未通过，T001/T003 保持 PARTIAL。

2026-09-15 source closure checkpoint `016daa38`，13 tests passed；恢复准备静态/组合通过，已复用 r3 rootfs/configure/Rust 继续原 C++ `-j4` 构建。最终 SIF、容器内 C++ 定向单元测试和 YOLO runner 尚待完成，T001/T003 保持 PARTIAL。

2026-09-15 B187-NDNSF-CONSUMER / PARTIAL：build-r3 的 SDK/configure/Rust 已通过，Waf 因源码归档遗漏 DI `.pc.in` 模板停止，尚未开始 C++ 编译。原始日志和 rootfs 保留；修复封存清单并静态复审后复用现场。未生成候选或推理 PASS；见 [candidate evidence](evidence/b187-complete-candidate.md)。

2026-09-15 B187-CONSUMER-SCRATCH 静态/组合审查与 25 个定向测试通过（3.75s）；开始新 bundle-r5 的候选重试，实际原生编译/验收仍 PARTIAL。修改只影响容器 scratch 与失败保留，不改变封存 base。

2026-09-15 B187-CONSUMER-SCRATCH / PARTIAL：已恢复，修复容器临时目录隔离，r1 静态/组合通过；补充失败现场保留的 r2 增量审查后统一测试，再生成新 definition 重试。封存 base 不变；[candidate evidence](evidence/b187-complete-candidate.md)。

2026-09-15 storage cleanup DONE；候选构建仍暂停：已归档并逐文件核对 9 月 6 日两个旧 native build 后释放原目录；旧临时 checkout 的 RELEASE 副本与保留件比较一致后删除。Codex 两份故障备份无损压缩；82 个超过 30 天未更新的会话经 archive 内容比较及活动检查后压缩归档，可恢复，未改数据库。当前可用约 23 GiB；封存 base、模型、密钥和当前/近期会话保留。下一步先修复 build-r2 的临时目录权限/隔离边界，再继续构建。

2026-09-15 B187-NDNSF-CONSUMER / PARTIAL，按用户要求先暂停构建、清理磁盘：build-r2 已通过 base SDK 复验，随后在清理旧 `/tmp/nac-abe-build` 等目录时因权限拒绝停止，尚未编译仓库目标。原始 log/record 保留；清理完成后先修复构建临时目录隔离，不重建或修改封存 base。

2026-09-15 B187-NDNSF-CONSUMER / PARTIAL：新 `bundle-r4` 与 definition 已封存，使用不可变 base `8ebfc464…` 启动完整候选 build-r2。只重建仓库目标，外部 SDK 沿用 base；原生 runner r2 已静态/组合通过，待本轮候选内实际编译运行。原始日志 `.codex-tmp/spec187-app-build-20260915/build-r2/build.log`；构建中不计 PASS，见 [candidate evidence](evidence/b187-complete-candidate.md)。

2026-09-15 B187-BASE-SDK / DONE（base only）：最终 SIF SHA-256 `8ebfc4646a5f96109a8480b120e684ee3923bf067d2e49aef53b42d29e5acdd9`，4,087,824,384 bytes。最终镜像基础/SDK 原生检查通过，坏库覆盖按摘要拒绝；C++/ORT YOLO CPU 三次 oracle 对照通过。已封存于仓库外 `ndnsf-artifacts/base-sif/<sha256>/`，完整清单校验与移动后镜像复验通过、文件只读；构建 lock 已绑定新 base。T001/T003 保持 PARTIAL，下一步是 NDNSF consumer 实际构建与本地完整候选验收；不声明 MiniNDN/Tiger。见 [封存证据](evidence/b187-base-sdk-sealed.md)。

2026-09-15 B187-BASE-SDK / PARTIAL：r5 修复后已在保留 rootfs 通过真实 SDK C++/Rust/Python/GStreamer/ELF 验证及基础 NumPy/NFD smoke；consumer r2 静态门与 46 项定向测试通过（5.59s）。当前封装最终 base SIF，按用户要求先验收并永久封存，再继续 NDNSF candidate；尚不声明最终镜像 PASS。见 [完整候选记录](evidence/b187-complete-candidate.md)。

2026-09-15 B187-BASE-SDK / PARTIAL：首次增建已编完外部库，但 C++ probe 缺 NAC include 子目录而失败；保留 FAIL record 与 rootfs，修正并复审后在已有构建现场复验。T001/T003 不计完成；[失败边界](evidence/b187-complete-candidate.md)。

2026-09-15 B187-BASE-SDK / PARTIAL：r4 不可变快照通过只读 `STATIC_PASS / B187-BASE-SDK_COMPOSITION_PASS`；`test_dependency_sdk.py` 与 `test_base_runtime.py` 共 24 passed（1.19s）。正在从现有 base 增建 `images/base-sdk-20260915-r1`，实际 SDK 验收尚未完成；随后接入 NDNSF consumer。见 [完整候选记录](evidence/b187-complete-candidate.md)。

2026-09-15 B187-BASE-SDK / PARTIAL：用户接受 `base SIF + NDNSF` 两层，并要求从现有 base 增建依赖。脚本新增同一 base 入口的 dependency-bundle 模式，待静态门和真实构建验收；NDNSF consumer 下一批接线。本机 itiger-ndnsf-ops 安装版原为 2932 行过时副本，已基于仓库简版补齐规则并同步，skill validator 通过；旧安装版备份在 `.codex-tmp/spec187-two-layer-20260915/installed-skill-before.md`。T001/T003 仍未完成。

2026-09-15 分层纠正：用户确认非 NDNSF＋APP 的依赖应在 base 内构建并验证。暂停完整候选重试，先补齐 base 的运行时与 SDK 闭包，避免 APP 阶段重复构建通用依赖。首轮容器已通过 ONNX/NAC-ABE/SVS/NDNSD 编译，停于 NDNSD metadata 检查；候选未生成，T001/T003 保持 PARTIAL。先前 base PASS 仅覆盖当时 smoke 集合，不代表完整构建依赖闭包；见 [完整候选记录](evidence/b187-complete-candidate.md)。

2026-09-15 B187-NATIVE-INPUTS：官方 ONNX/Rust 与 Cargo.lock 离线 vendor 已封存，干净源码 handoff `SOURCE_READY`；23 个定向脚本测试通过。旧构建已归档并经内容比较后释放。当前继续构建完整 NDNSF+APP 候选，T001/T003 未完成；这些准备输入由本机生成，不再要求用户提供。见 [完整候选记录](evidence/b187-complete-candidate.md)。

2026-09-15 B187-YOLO-BASE：同一 base 中实际 C++/ORT YOLO26n CPU 推理三次通过（约 182/106/91 ms），50 行输出均满足独立 oracle 容差，最大绝对误差 0.000366211。此结果仅 `YOLO_CPU_MODEL_SMOKE_ONLY`；没有生成最终 NDNSF+APP candidate 或运行 MiniNDN。SDK 已适配，剩余 ONNX/Rust 容器构建输入、host gate 和完整 APP 接线仍由 T001/T003 承接；不把可由现有源码/缓存生成的输入归为用户必须提供的资料。见 [本机 YOLO 记录](evidence/yolo-base-cpu-20260915.md)。

2026-09-15 B187-BASE：r4 STATIC/COMPOSITION_PASS；15 个定向测试通过，实际 SIF 构建与最终镜像 C++ SDK/NumPy/NFD/ELF smoke 全部通过，SHA-256 `7b4b501033f2db876ccf5c19a9637a58b8b232cf4d555f5b8a225638e180837c`。缺失 OpenBLAS 的实际 overlay 反例被拒绝。额外旧门禁测试 6 项 fixture 失败另记，未伪造全套 PASS。仅 base 前置完成，T001/T003 保持原状态；见 [base repair](evidence/b187-base-repair.md)。

2026-09-15 17:51 -05:00：T002 的 C++ selector 已注册并接入 Spec187 native mode；阶段证据现按 request/attempt/plan 关联，并以 epochMs 核对 ACK → Selection commit → Provider accepted → Provider execution 顺序。r7 官方 review-agent 返回 STATIC_PASS；受影响目标以 `-j4` 编译通过（1m6.118s），独立 served-provider selector 通过，缺输入 selector 按预期 fail-closed。已补齐 T001–T006 的显式 FR/SC requirement coverage，分析器追踪到 12/12 FR 与 6/6 SC。真实 through-MiniNDN 请求仍需 candidate-bound config/input，T001 仍缺 regular base SIF/host-gate，T003 及后续批次保持 WAITING_EXTERNAL_INPUT/PARTIAL。

## Logical Batches

| Batch ID | Members | Stable exit | Shared selector / build | Status |
| --- | --- | --- | --- | --- |
| B187-LOCAL-CLOSURE | T001 | closure gate rejects invalid candidate inputs before side effects and accepts a verified pair tuple | existing Tiger script checks and mutation fixtures | PARTIAL |
| B187-BASE | T001 prerequisite | pinned stable base builds and passes native SDK/NumPy smoke | test_base_runtime.py; build-base-sif.py; container C++ base-smoke | DONE |
| B187-APP-SDK | T001 prerequisite | explicit SDK copies load from APP; missing library rejects without fallback | template/handoff/APP tests; container C++ loader probe | DONE (SDK only) |
| B187-YOLO-BASE | T001 prerequisite | real YOLO26n CPU output matches independent oracle inside base | run-yolo-cpu-smoke.py; C++ yolo-cpu-smoke.cpp | DONE (model smoke only) |
| B187-LOCAL-YOLO | T002,T003 | two identical-candidate local C++/MiniNDN terminal YOLO runs | Spec187YoloMiniNdn; Apptainer 1.5.3 candidate | PARTIAL |
| B187-TIGER | T004 | one bounded same-candidate TigerCluster run | run-sif-app.sh and same selector | WAITING_EXTERNAL_INPUT |
| B187-DEFERRED | T005 | QWEN listed as TODO without entering candidate | docs checks | DONE |
| B187-BASE-SDK | T001 prerequisite | existing base extended with verified external runtime and build SDK | test_dependency_sdk.py; actual container C++/Rust/Python probes | DONE (base only) |
| B187-NDNSF-CONSUMER | T001,T003 | repository targets consume the verified base without rebuilding external dependencies | incremental container build and native YOLO runner | PARTIAL (static and packaging tests passed; actual build pending) |
| B187-CONVERGENCE | T006 | fresh audit PASS before formal local/cluster evidence | CodeGraph plus exact source and symbol checks | PARTIAL |

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
- unobserved: ONNX/Rust container build closure, current-source host-gate manifest, candidate SIF/APP, candidate-bound C++ requester config/input, two real MiniNDN runs, and cluster run. Base smoke and APP SDK loader checks are recorded separately in [B187-APP-SDK](evidence/b187-app-sdk.md); they do not close the request chain.

## Dependencies & Execution Order

T001 → T002 → T006 → T003 → T004. T005 is independent documentation work and must not add a dependency to the YOLO path. Each code task receives its own static review before the batch composition review; T003 and T004 cannot start without convergence PASS and the preceding acceptance result.
