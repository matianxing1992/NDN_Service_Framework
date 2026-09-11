# Implementation Plan: Native DI Closure

**Branch**: Experimental | **Date**: 2026-09-11 | **Status**: IN_PROGRESS / B5 component and full-unit exits recorded; integration qualification pending
**Migration baseline**: `6f603491`；产品审计源码 `72b9e388cc3920b0bdcd4c36d302d63c71e7f15a`。
**Authority**: [spec](spec.md)、[tasks](tasks.md)、[transfer](contracts/transfer-matrix.md)、
[promotion candidate](contracts/promotion-candidate.md)、[qualification matrix](contracts/qualification-matrix.md)。

## Summary

本 Spec184 承接182未完成工作，不复制其庞大时间线。B1–B4 已形成可复核的局部出口；
下一 dispatch 为 B5/T006 的 qualification matrix 对账与缺口收敛。
具体缺陷的源码位置、触发条件与反例沿用冻结的
[request-chain audit](../182-native-di-python-bindings/evidence/request-chain-static-audit-20260911.md)。
旧 R12 批次映射为184 B1–B5；未完成的组件验收同样进入 B5，并非只搬四个 finding。

## Technical Context

使用现有 C++ DI 库、Core Face IO、独立 artifact authority、Provider、ORT/tokenizer、journal。
生产入口和 oracle 见 spec Acceptance Evidence Contract。没有新增依赖、wire 或目标 API；
不把 model/tokenizer/KV 状态移进通用 Core。开启每批前核对 build tree/cache/link identity，
复用已验证受影响 target，默认 -j4、观察 vmstat，不启动竞争构建。

## Constitution Check

中文叙述、英文标题/ID/状态；独立 authority、不信任 caller plan、C++ owner 和证据分级继承。
已完成实现不重复编写，未完成资格不因拆分关闭；当前/目标 Design 独立维护。
Promotion candidate、change-plane invalidation 和 design-code convergence 规则见
[promotion candidate contract](contracts/promotion-candidate.md)，任何 expensive validation
都必须先满足这些规则。

## Execution Order

| Batch | Tasks | Depends | Dynamic profile / invariants | Stable exit |
| --- | --- | --- | --- | --- |
| B1 Thread ownership | T001/T002 | 冻结审计/迁移对账 | `tsan`; IO owner、turn/ticket 发布与 abort 线性化、无迟到回调副作用 | IO dispatch 与 turn 发布的并发反例通过，无 pending ticket 泄漏 |
| B2 Durable outcome | T003 | B1 | `asan-ubsan`; handle/journal 生命周期、publish 后终态一致、residue=0 | publish 前后取消、deadline、close 与持久记录/handle 一致 |
| B3 Secure export | T004 | B2；技术上独立，默认顺序执行 | `asan-ubsan`; 临时文件/loader 生命周期、失败保留旧 checkpoint | 0600 原子导出、失败保留、loader round-trip |
| B4 Caller convergence | T005 | B1–B3 | caller/launcher rows `none` with reason; inherited B1/B2 profiles cover shared async owners | caller/mode matrix 闭合，当前 route/provider/Qwen selectors 有 focused exit；D2b/real-model/no-Python 留 B5 |
| B5 Qualification and handoff | T006/T007/T008 | B4；矩阵盘点可提前只读进行 | per inherited row; `none` for documentation-only reconciliation | qualification matrix 完整、fresh convergence audit `PASS` 后才可开始 T007；本地资格通过，外部边界明确 |

### Dynamic Validation Card

动态分析在每个逻辑批次的组合审查之后执行一次；不把 sanitizer/fuzz 构建拆到每个任务。
批次 evidence 必须冻结以下字段，并将每个 selector 的结果映射回成员任务：

| Field | B1 | B2 | B3 | B4 | B5 |
| --- | --- | --- | --- | --- | --- |
| Risk / profile | `concurrency`; `tsan` | `lifetime/linearization`; `asan-ubsan` | `lifetime/serialization`; `asan-ubsan` | async caller `tsan`, static rows `none` | inherited row profile；文档对账 `none` |
| Parameter boundary | request/attempt、取消、迟到回调 | publish 前后取消、deadline、FINALIZE 延迟 | umask、已有文件、symlink、写入失败 | caller/mode、默认路由、compatibility rejection | 每个 PO/I/FR/CD 的正负例和 candidate identity |
| C++ selector / invariant | `Spec184AuthorityIoOwnership`, `Spec184TurnPublicationRace`; IO owner、ticket residue | `Spec184DurableOutcome`; durable journal/handle 一致、residue=0 | `Spec184CheckpointExport`; 0600、旧文件保留、loader round-trip | caller-specific C++ selectors; native default and cleanup | matrix-bound selectors; source/artifact identity、terminal cleanup |
| Repeat / output | 每 selector 至少 2 次；独立 TSan tree | 至少 2 次；独立 ASan/UBSan tree | 至少 2 次；独立 ASan/UBSan tree | async rows 至少 2 次；按 row 输出 | 按继承 row 的预算和独立输出 |

动态工具只检查内存、线程和未定义行为；参数是否满足业务契约由上述 C++ fixture/oracle
断言。每张卡还要记录 compiler/linker、依赖和 binary digest、原始 stdout/stderr、退出码
及首个失败边界。外部库或 ABI 不一致导致的 sanitizer 报告先记 `DYNAMIC_FAIL`/`PARTIAL`；
不能用 `ASAN_OPTIONS` 等抑制直接升级为 `DYNAMIC_PASS`，必须在可验证的一致 ABI 构建中
无抑制重跑。

### Dynamic Parameter Matrix Budget

每个批次只维护一张有界参数矩阵；成员任务共享同一状态机、C++ selector 和动态构建。矩阵
采用行为等价类而非字段笛卡尔积，至少覆盖正常值、关键边界、故意非法值以及会改变清理或
并发顺序的取消/替换值。每行必须绑定预期业务结果和 C++ 断言；未覆盖边界写入 B5 的
qualification row，不为补齐表格而新增执行任务。

| Batch | Minimum cases | Budget / repeat | C++ assertion owner |
| --- | --- | --- | --- |
| B1 | nominal request, cancel-before-dispatch, cancel-after-publication, deadline/replacement | each selector ≥2 TSan repeats; deterministic barriers | `Spec184AuthorityIoOwnership`, `Spec184TurnPublicationRace` |
| B2 | publish-before-cancel, publish-after-cancel, delayed FINALIZE, close | normal + 3 unsuppressed ASan/UBSan repeats | `Spec184DurableOutcome` |
| B3 | umask, existing destination, symlink, pre-rename failure, round-trip | normal + 3 unsuppressed ASan/UBSan repeats | `Spec184CheckpointExport` |
| B4 | native config, compatibility selection, missing/invalid config, shutdown | per async selector ≥2 repeats; static rows `none` with reason | caller-specific C++ selector or explicit `gap` |
| B5 | one positive and one negative case for each inherited row class | matrix-owned budget recorded with candidate identity | qualification matrix selector; no Python-only oracle |

## Code Design and Review

B1：authority transport 仅在 worker 等待，将 Core 提交封送到 postToIo；Operation 字段明确
线程 owner/mutex，锁内快照、锁外 bind、锁内检查 Pending/attempt 后发布，失败清理未发布 ticket。
B2：确定 durable publish 与成功结果的同一线性化决定；FINALIZE best-effort 不撤回 committed parent。
B3：受限临时文件、检查写入/关闭、既有持久性要求与原子替换；symlink 不跟随，
同目录临时文件完成 `0600`、write、`fsync`、close、rename 和目录 `fsync` 后才可报告成功。
B4：先生成真实 caller/mode 清单再按共享后端分组，不能机械按历史16调用点逐点构建。
B5：先盘点原 FR/CD/INV/PO/I、组件与 harness 未关闭项，再补缺失实现/fixture，静态合成门后才正式验收。

逐任务明确加载 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 进行只读审查，记录路径/hash、
基线、完整 diff、调用方/测试、finding 和复审。批末按
`skills/speckit-code-design/references/batch-quality-gates.md` 组合审查再共享编译测试。
不得在 T001 后因一个小修改立即重跑全套；动态 profile 在批末独立 sanitizer/fuzz 输出树中运行，
也不得把 T007 当成所有前置定向 C++ 验证的唯一时点。
原生断言、fixture/driver、oracle 为 C++；Face/IO/scheduler/callback owner 活到任务 drain/join。

### B1 Allocation Contract

| Task | Production symbols / source | C++ source and selector | Target / closure | Dynamic profile / invariants |
| --- | --- | --- | --- | --- |
| T001 | `NativeAuthenticatedGrantClient::issueThroughCore` / `coreIssue` → `ServiceUser::RequestServiceTargeted` and `postToIo` | `tests/integration-tests/di-native-requester-grant.t.cpp` / `Spec184AuthorityIoOwnership` (`FOCUSED_VALIDATED`) | `integration-tests`; registered source and exact selector evidence in B1 record | `tsan`; Core IO owner, pending-call balance, late callback fencing |
| T002 | `NativeInferenceClient` turn/attempt binding, publication and ticket cleanup | `tests/unit-tests/di-native-client.t.cpp` / `Spec184TurnPublicationRace` (`FOCUSED_VALIDATED`) | `unit-tests`; unit glob registration and exact selector evidence in B1 record | `tsan`; ticket publication/abort linearization and stale-token rejection |

T001 and T002 share the B1 request-correctness outcome but have different targets and fixtures;
each task gets its own static gate before the B1 combination review. If a change adds a different
state machine, owner, target or hard prerequisite, it leaves B1 and receives a new Batch ID.

## Coverage and Evidence Contract

每批一个简短 `evidence/bN-result.md`，状态唯一在 tasks.md。五 lane 必须填实际符号/命令或 gap：
生产入口/调用方；实现/wire；测试/harness/oracle；build/source closure；迁移/证据。
每条 scope 见 spec 的 entry 和 transfer matrix；开始实施时补具体 diff、target 和 selector，不能预填 PASS。
新链接边界需符号定义 TU/target 与 nm/readelf 对照。记录 review trace、Batch growth decision、
Closure decision、Dynamic validation 和 static/compile-link/runtime-test/unobserved miss；B1–B3
动态 profile 已有无抑制通过，B4 记录 `none` with reason，B5 尚未运行最终资格 profile。
审查技巧沿用182审计后 R12 的线程读写表、线性化点、失败清理、wire 权威溯源和 production fixture 检查。

## Convergence Gate Before Qualification

T006 must produce a complete [qualification matrix](contracts/qualification-matrix.md), freeze
the candidate described in [promotion-candidate.md](contracts/promotion-candidate.md), and record
all unresolved rows as `OPEN`/`PARTIAL`. It then runs a fresh code-aware design-to-code convergence
audit recorded at `evidence/convergence-b5.md`. The audit must inspect the accepted spec/plan,
contracts, real C++ entry points and callers, target/source closure, effective configuration,
security boundaries, cleanup and evidence paths, and must report `PASS` for this candidate.

`evidence/convergence-b5.md` is a hard dependency of T007. Any behavior-affecting source, header,
dependency, configuration, harness or contract change after that record invalidates it and requires
the earliest gate in the promotion-candidate invalidation matrix. A focused regression may run while
the verdict is `BLOCK`; it remains development evidence and cannot authorize T007.

## Document Size Control

tasks.md 只保留当前 registry、当前 checkpoint 与任务，不追加逐命令时间线。
证据按批次单文件维护；原始日志放 .codex-tmp，不入 Git。新增问题先归现有批次，范围变更须说明
为何不能归入；已完成历史留182或批次证据，不重复抄进 spec/plan/tasks。
