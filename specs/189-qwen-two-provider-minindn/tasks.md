# Tasks: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Installer maintenance checkpoint**: 顶层安装入口已修正；12 项隔离测试、ShellCheck、Bash 语法与本机只读依赖检查通过。未运行完整安装或原生构建，不改变本 Spec 的 PARTIAL 状态。见 [installer audit](../../docs/install-stack-audit.md)。

**Status**: IN_PROGRESS
**Input**: [spec.md](spec.md), [plan.md](plan.md), [batch-execution.md](batch-execution.md)
**Rule**: Production C++ → C++ assertions → Python orchestration。每任务编码、fixture/调用方/构建注册完成后冻结静态审查；同批组合通过再统一增量构建测试。静态通过不是完成。

## Current Checkpoint

**B189-1b r20 local verification checkpoint**: 2026-09-19 — the material
consumer fixture repair passed official read-only review r19
(`STATIC_PASS`, snapshot SHA-256
`92d475dfc3c1bf8a77150c52ac9696dccf4c7ea7d101635f996bceb28e416c6a`). The
affected `integration-tests` target rebuilt with root Waf `-j4` in 22.803s;
the real worker bundle selector and selected-payload budget selector passed.
The related GrantIssuer and protected publisher selectors passed. The Repo
publication selector first exposed a stale assertion that required fewer Repo
objects than payloads; after the r20 review (`STATIC_PASS`, snapshot SHA-256
`448a01c7e7848b46bd23caba73fcac3e7cd10a6ebd7f5466eaa1bf95cbb41aa1`),
`unit-tests` rebuilt with root Waf `-j4` in 26.617s and the Repo, GrantIssuer,
and publisher selectors passed. Repo keeps one independently addressable
range-store object per material payload; protected NDN publication owns bundle
coalescing. The root NDNSF Waf builds only NDNSF-owned targets and consumes
the installed NAC-ABE SDK; NAC-ABE remains owned by its own build system.
This closes only local material-publication/consumer evidence. T003, real
Qwen preparation, ACK/Selection, Provider execution, MiniNDN/Tiger and
qualification remain `PARTIAL`/open. See [material publication evidence](evidence/b189-material-publication-20260919.md).

**B189-1b material-publication checkpoint**: 2026-09-19 — the r5 frozen
material-backed publication diff received official read-only `STATIC_PASS`.
The affected DI closure and the newly registered `spec189-canonical-publisher`
target built with root Waf using `-j4`; the focused material-backed publication
assertion passed 1/1, related publisher regressions passed 5/5, and the
protected request, placement, material, provider-stage and CLI C++ selectors
passed. The existing full publisher suite still has four fixture/environment
failures, which are recorded separately and are not counted as a product PASS.
The previous real r38 run remains stopped at
`PREPARATION_FAILED / DI_NATIVE_PUBLICATION_MATERIAL_LIMIT`; no MiniNDN or
Qwen qualification is claimed. See [material publication evidence](evidence/b189-material-publication-20260919.md).

**B189-1b r39 boundary**: 2026-09-19 — a fresh run using the repaired global
candidate crossed the former publication-byte rejection but stopped at
`PREPARATION_TIMEOUT / DI_NATIVE_PREPARATION_TIMEOUT` during real Qwen
preparation. Cleanup passed; no ACK, Selection, Provider assembly, execution,
terminal response or qualification was observed. T003 remains `PARTIAL`; the
next action is to diagnose and reduce preparation cost before another retry.

**Audit reconciliation checkpoint**: 2026-09-19 — 已将 [DI/Repo static audit](evidence/di-repo-design-static-audit-20260919.md) 与当前源码重新对账：F01 的 material-only consumer 已有局部生产接线和 C++ selector，但真实 protected ingress 仍未验收；F02 已有 focused C++ selector 但实际 ORT/RSS 与完整候选资格仍开放，F08 仍开放，F05/F09 已有 focused C++ selector 但完整候选边界仍开放；F03/F04/F06/F07 分别标为条件性或 legacy follow-up。新增 FR-027..FR-029、T003-R1..R3 与 T009-R1，未将任何任务勾选完成。[audit reconciliation](spec.md#audit-reconciliation--2026-09-19)

**Protected range-store checkpoint**: 2026-09-19 04:52 -05:00 — B189-1a 的冻结范围通过官方只读 `STATIC_PASS`；受影响的 NDNSF targets 用仓库 Waf `-j4` 完成 compile/link，Repo range-store 6/6 C++ cases 和生产 Runtime protected publication 1/1 C++ case 通过，requester `--help` 入口通过。证据记录了 Core ciphertext publication、Repo generation/lease fence、worker cancellation/rollback、bounded reads、source release 和第二次 prepare 无对象增长。[protected range-store evidence](evidence/b189-protected-range-store-20260919.md) 只关闭本地 protected publication 接缝；T003、真实 Qwen、ACK/Selection、Provider、MiniNDN/Tiger 仍为 `PARTIAL`/open。根 NDNSF Waf 只负责 NDNSF 自有目标；NAC-ABE 由其自身 Waf 及已安装 SDK 负责，未递归构建。

**B189-2 production-ingress checkpoint**: 2026-09-19 — 在现有 global-r3 Waf tree 对 `integration-tests` 的受影响源以 `-j4` 增量编译成功（39.072s，峰值 RSS 2,114,188 kB，0 swap），并运行真实 C++ `Spec170NdnsfDiCoreFlow` 生产入口 selectors：双 Provider D2b 请求到最终响应、ACK 后 post-Selection runner preparation、篡改 capability 拒绝均通过；post-Selection preparation factory 在手工 ACK/Selection 发布前两个 Provider 都为 0，发布后达到 provider0=2/provider1=1。该组用例证明 production handler 位于真实 ACK/Selection 后路径，但仍未直接记录未选 Provider 的 source fetch/assembly 计数，也未使用真实 Qwen canonical manifest；因此只登记为 T005 的 `FOCUSED_CXX_PASS`，不关闭 T005/B189-2。详见 [production placement evidence](evidence/b189-placement-production-20260919.md)。

**B189-3/T007 rerun checkpoint**: 2026-09-19 — B189-3/T007 的冻结范围经官方只读 `STATIC_PASS` 后，使用正确的 `../waf` 入口在 `build-spec189-b189-3-global-r3` 以 `-j4` 完成六个受影响目标编译链接（16.228s）：`ndnsf-distributed-inference`、`spec185-provider-assembly`、两个 Spec189 oracle CLI/fixture target 和 material oracle。C++ material oracle 15/15、provider-stage oracle 18/18、CLI oracle 5/5 通过；真实 `Provider::serve` selector 通过并记录同一 `preparationId` 的 assembly/ready 配对。首次错误的 `waf` 路径调用已登记在 failure log，未计为产品失败。该出口仍保持 T007 `PARTIAL`：没有独立模型数值、特定 upstream endpoint/compute-start 因果、真实 Qwen 多 token、MiniNDN 或复用资格。[causal oracle evidence](evidence/b189-causal-oracle-20260919.md)

**F02 focused checkpoint**: 2026-09-19 — T003-R1 的 production `MemorySnapshot`/terminal
guard 通过 r2/r3 官方只读复审；仓库根 Waf tree 重新配置并以 `spec189-preparation-memory`
完成 compile/link，修复 build-tree 与 `/usr/local` 同 SONAME 混链后 selector 通过 1/1。
本证据只覆盖分类计数与 post-publication/pre-cache cancellation rollback；实际 ORT allocator/RSS、
真实 Qwen 和完整资格仍开放。[F02 evidence](evidence/b189-f02-memory-20260919.md)

**F09 focused checkpoint**: 2026-09-19 — T003-R3 的 FilesystemRepoStoreBackend
`fsync`/`close`/directory-`fsync` 反例已完成 r4 只读 `STATIC_PASS`、`-j4` 单目标构建和
3/3 C++ selector 运行；F09 仅为 `FOCUSED_CXX_PASS`，不关闭 T003，也不替代 F02/F05
或完整 publication/qualification。见 [F09 evidence](evidence/b189-f09-fd-owner-20260919.md)。

**F05 focused checkpoint**: 2026-09-19 — T003-R2 的 RepoCore mixed quota
selector 完成 r2 只读 `STATIC_PASS`、`-j4` 单目标构建和 2/2 C++ cases；range
reservation 现在同时约束普通 vector 与 exact Data packet admission，abort 后可恢复。
这只关闭 F05 的 focused selector，不关闭 replacement/失败回滚、T003 或 protected
publication/qualification。见 [F05 evidence](evidence/b189-f05-quota-20260919.md)。

**Causal oracle checkpoint**: 2026-09-19 01:37 -05:00 — T007 多次 runner preparation 独立日志 ID 与逐 ID 判据、正常 CLI 的材料/范围/末段 terminal 门已通过 r2 只读任务与组合审查；增量构建 2m54.872s，C++ 材料15例/阶段18例/CLI 5例通过，真实 Provider::serve 接线 selector 通过并记录配对 preparationId。七个输入匹配审查快照；独立模型输出、endpoint 因果、真实 MiniNDN/复用仍未完成，保持 PARTIAL。[causal oracle evidence](evidence/b189-causal-oracle-20260919.md)

**Material oracle checkpoint**: 2026-09-19 01:25 -05:00 — T007 原子材料事件判据已通过只读任务/组合静态门，两个 oracle target 增量构建 23.813s，C++ parser 15 cases PASS；已核对审查/构建源码身份。共享 checker/test 独立归档，CLI 混合改动保留待整合；因果顺序、独立输出、同 handle 复用及真实 MiniNDN 仍未完成。[material oracle evidence](evidence/b189-material-oracle-20260919.md)

**DI/Repo repair design analysis**: 2026-09-19 — 已复核并行新增的 material-only consumer，上一轮 F01 的“未接入”不再代表最新源码；局部 consumer PASS 与真实生产链资格仍区分。完成 [repair design analysis](evidence/di-repo-repair-design-analysis-20260919.md)，建议统一材料读取、存储提交/目录恢复、预算与 turn 所有权，并明确小修复和后续结构收敛的边界。仅分析，未改产品源码、未构建或运行实验、未修改任务完成状态；建议为 `PROPOSED`，不覆盖冻结目标。

**Requester Repo checkpoint**: 2026-09-19 01:23 -05:00 — T003 source owner 首次 external initializer 重复读取已修复，r6 复审及增量构建通过；Repo C++ 5/5，私有 spool 下 Runtime production-entry 1/1。DI 全局安装已完成，DI/Core build/global SHA256 一致，requester 的全局动态链接路径已核对；两个调用方的混合改动尚未归档。真实跨节点读取/Qwen/MiniNDN 保持 PARTIAL。受保护 publication 继续走 Core ServiceUser；详见 [requester Repo evidence](evidence/b189-requester-repo-20260919.md)。

**DI/Repo design audit checkpoint**: 2026-09-19 — 原审计是 13b79ad1 工作树时点的只读扫描，覆盖 372 个源码文件清单、173 个 Python AST 和准备/发布/组装、Repo 目录/容量/持久化、Conversation owner。后续 material-only consumer 已接入局部生产组装路径并通过 C++ selector，因此 F01 的“未接入”只保留为历史边界；准备峰值、混合写入预算、turn owner 和 fd 错误路径仍开放。F03/F04/F06/F07 不属于当前 native protected qualification 的默认调用方，保留条件性/legacy 状态。报告不是逐行全量审查或产品验收，保持 `PARTIAL / NOT_STATIC_PASS`；详见 [DI/Repo design static audit](evidence/di-repo-design-static-audit-20260919.md) 和 [repair design analysis](evidence/di-repo-repair-design-analysis-20260919.md)。

**Updated**: 2026-09-18 19:45 -0500 — the file-backed ONNX assembly/resource subunit passed the r13 read-only static gate and focused validation (`22 passed, 1 skipped`); the real Qwen canonical identity scan passed with 3,689,700 kB peak RSS and zero swaps. The audit retired T008 as a standalone capability task: guard/lifecycle checks are cross-cutting gates owned by T003/T006 and closed by T009. The new single-target global install helper passed static review, its preflight and flags regression suite passed (`100 passed`), and `ndnsf-distributed-inference` built/installed in 11.899s with matching build/global SHA-256 and global ONNX Runtime linkage; no MiniNDN qualification is claimed. Protected Provider ingress, native Repo publication, ACK/Selection, two-provider execution and full cleanup remain open. See [B189 convergence evidence](evidence/b189-convergence.md), [target install evidence](evidence/b189-global-target-helper-20260918.md), [resource evidence](evidence/b189-resource.md), and [failure log](../../docs/failure-log.md).
**Latest checkpoint**: 2026-09-19 — the strict native ONNX source-reuse fix received read-only `STATIC_PASS`; the affected DI/worker/unit targets built in `1m52.201s` with `-j2`, and the five focused ONNX selectors passed after building their worker tools (`11/11`, `5/5`, `9/9`, `11/11`, `30/30`). The combined selector was stopped at a host resource boundary. Real Qwen runs r30 through r33 all stopped at `RESOURCE_BOUNDARY:swapIo` before an interpretable two-provider workload result; MiniNDN/workload remain `NOT_EVALUATED`. r32 used the correct global profile after a separate stale-profile preflight rejection; r33 reached the running/drained sampling phases but still did not start MiniNDN. See [ONNX identity/resource evidence](evidence/b189-onnx-identity-resource-20260919.md).
**Runner-preparation checkpoint**: 2026-09-19 — T006 generation/position metadata binding passed read-only `STATIC_PASS`; global-r3 `unit-tests` rebuilt with `-j1` in `28.600s`, and `NativePreparationContext*` passed 3/3. The regression covers stale adapter metadata removal and authenticated successor/position write-back. This is a metadata unit boundary only; real Provider ingress and ORT execution remain open. See [runner preparation evidence](evidence/b189-runner-preparation-20260919.md).
**Materialization-worker checkpoint**: 2026-09-19 — T006 canonical source/initializer ownership and bounded worker framing passed final read-only `STATIC_PASS` after two review fixes. The affected DI/worker/unit build completed with `-j1`; focused C++ selectors passed `5/5`, `3/3`, `4/4`, and `30/30` (the last with an explicit worker binary directory). This remains a component boundary; protected Repo ingress, real two-Provider execution, output oracle, and drain are open. See [materialization worker evidence](evidence/b189-materialization-worker-20260919.md).
**ONNX memory checkpoint**: 2026-09-19 — the direct-vector source reader and selective ONNX identity/shape materialization passed read-only `STATIC_PASS`. The global-r3 DI target installed successfully with `-j1` in `7m17.389s`; the unit-test target rebuilt in `5m56.704s`; `NativePreparationContext*` passed 3/3, the combined ONNX/assembly selector passed 18 cases, `Spec189*` passed 4 cases, and the selective shape regression passed. Qwen r36 still stopped after both Providers became ready at `RESOURCE_BOUNDARY:MemAvailable` (minimum `1557188608` vs floor `1610612736`, zero swap I/O, peak RSS `4591411200`); no workload or qualification result exists. See [ONNX memory/r36 evidence](evidence/b189-onnx-memory-r36-20260919.md) and [failure log](../../docs/failure-log.md).
**Protected Runtime publication checkpoint**: 2026-09-19 — the new C++ selector passed read-only `STATIC_PASS`; after fixing the fixture spool and bounded Repo read oracle, `spec189-prepared-request` rebuilt from global-r3 with `-j1` in `29.085s`, and the two `Spec189*` cases passed from the repository root. The protected case used Runtime's default Core publisher with `RepoEncryptedLargeDataStore`, verified encrypted source/root/material manifests, per-payload receipt bindings, bounded material reads, source release and no second-prepare object growth. This closes only a local protected-publication boundary; ACK/Selection, Provider assembly, real Qwen execution, output oracle, resource drain and MiniNDN remain open. See [protected Runtime evidence](evidence/b189-protected-runtime-20260919.md) and [failure log](../../docs/failure-log.md).
**Material consumer checkpoint**: 2026-09-19 — the T006 material-only consumer passed final read-only `STATIC_PASS` in r6. The global-r3 `integration-tests` target rebuilt with `-j1` in `3m37.037s`; the C++ selector passed its material-only positive path and aggregate-budget negative path, and the complete `Spec175NativeAssembly` suite passed 8/8 from the repository root. The consumer now reads only the authenticated manifest and selected payloads after Selection, with no source/initializer fallback and a pre-fetch aggregate budget. Production Core ACK/Selection ingress, real Qwen two-Provider execution, output oracle, drain and MiniNDN remain open. See [material consumer evidence](evidence/b189-material-consumer-20260919.md) and [failure log](../../docs/failure-log.md).
**Baseline**: `3e53fec5` plus pre-existing implementation and unvalidated protected-store draft; not a clean qualified candidate.

B189-1a publisher weak-pin、Repo identity fence 和 Core worker/cancel/key release
均已获官方只读 `STATIC_PASS`；组合构建和 package/cache owner selectors 已通过。
剩余的是 protected Repo source owner 在真实 Qwen prepare 中的接线和 B189-1b 原子
材料 consumer，详见 [prepare evidence](evidence/b189-prepare.md)。

T003 源借用修复已静态通过；增量构建 56.712s。初次 native heap corruption 已定位为
installed DI 的旧 ABI（publication 304 vs 360 bytes），全局安装同步后两个 C++ selectors
连续三轮通过。真实受保护 Repo 接线与原子材料仍未完成。见 [prepare evidence](evidence/b189-prepare.md)。

尚无 `QWEN_TWO_PROVIDER_PASS`。r25 run-record 仍 FAIL；Provider 日志已出现
`EXECUTION_ENTERED` / `ASSEMBLY_STARTED`，不能继续称“执行入口完全未观察到”。
尚未证明 runner ready、两段执行、有效终态及资源回收闭合。stream gap 是症状，不能单独认定根因。

本轮 B189-1a 组合构建 342 tasks / 7m8.097s，package-owner selector 3 次、Repo
protected selectors 6 次、bounded publisher 2 次及 Runtime prepare 1 次均 PASS；
保留全局依赖与定向构建、Repo 冷热发布/事务/层 payload fixture、同一 PreparedModel
两请求的 publication-counter selector、placement/cache selector、已注册 C++ 日志 oracle。
它们是组件证据，不能拼成真实 Qwen 全链 PASS。
requester 已有 encrypted range-store 注入草稿，尚未静态/构建/运行验收；
不能注入 plain Repo publisher 替代 protected publication。旧 assembler 仍获取完整 initializer；
分层 producer/consumer 尚未闭合。日志 oracle 的严格事件总序也需修正。

本轮审计见 [audit correction](evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction)。
旧详细 checkpoint 保留在上述 Git 基线和原批次证据；本表取代十任务线性调度。
T001 映射已关闭；host direct/launcher guard 已复审，39 host checks 通过；
两个 C++ 目标 -j4 构建成功，4 个 lifecycle 用例三轮通过。真实路径 native counters
及完整资源回收仍待验证；资源门由 T009 收口，未运行完整模型。磁盘现约 34 GiB 可用。
文档修订已获冻结 v2 的 DOCUMENTATION_STATIC_PASS；11/11 技能入口、6 task ID、
29 FR、链接/锚点及 diff 检查通过，详情见上述审计记录。产品验收保持 PARTIAL。

## Execution Progress

6 项是能力任务，不按数量计算产品百分比。T003 三个独立执行出口见
[bounded execution units](batch-execution.md#bounded-execution-units)，B189-1a 已关闭，
当前下一步为 B189-1b。
每个出口验证后立即记录，不等整项 T003 写完才第一次构建。
host guard 和小型 lifecycle safety entry 已达到其前置出口；剩余 native counter
接入属于 T003/T006 的实际 owner，最终在 T009 以完整采样和 drain 证据收口，不再
创建重复的资源行政批次。
本轮文档 v2 已获 DOCUMENTATION_STATIC_PASS；结构/29 FR/链接与技能同步检查通过，
见 [follow-up verification](evidence/spec189-static-audit-20260918.md#follow-up-verification)。
受保护接缝已通过 B189-1a 静态门、受影响目标构建及 C++ focused selectors；Repo
adapter producer/consumer 和 Runtime protected publication 的本地出口已记录，
但 protected Provider consumer/真实 Qwen 链路仍未审查，本轮未构建或运行模型。

| Unit / Details | Status | Depends | Remaining exit / Evidence |
| --- | --- | --- | --- |
| [T001 Freeze integration boundary](#t001) | DONE | — | 2026-09-18 13:29 -0500：真实接线/候选/后继缺口及五 lane 映射已只读审查；仅关闭实施边界。[convergence](evidence/b189-convergence.md) |
| [T003 Prepare and reuse Repo materials](#t003) | PARTIAL | T001 | material-backed publication budget/receipt path and focused C++ selector now pass; protected range-store/material-only consumer remain local boundaries. F02 actual ORT/RSS, F05 replacement/rollback beyond focused selector, F09 complete publication boundary, real Qwen source release and protected ingress remain open. [material publication](evidence/b189-material-publication-20260919.md); [prepare](evidence/b189-prepare.md); [protected range-store](evidence/b189-protected-range-store-20260919.md) |
| [T005 Authenticate placement](#t005) | PARTIAL | T003 | ACK 后规划、signed Selection、生产 ingress handler/no-fetch 计数。[placement](evidence/b189-placement.md); [production ingress](evidence/b189-placement-production-20260919.md) |
| [T006 Materialize selected ranges](#t006) | PARTIAL | T005 | material-only C++ consumer 与 aggregate budget 已通过；生产 Core/Provider ingress、owner/cancel counters 和真实两 Provider assembly 仍待完成。[material consumer](evidence/b189-material-consumer-20260919.md) |
| [T007 Validate handoff and output](#t007) | PARTIAL | T006 static gate | 阶段/材料/CLI 门已验；NDN endpoint 因果、hidden-state handoff、独立输出与真实多 token 仍待验。[causal oracle](evidence/b189-causal-oracle-20260919.md) |
| [T009 Qualify reuse and repeat](#t009) | PARTIAL | T003 + T005 + T006 + T007 | 同 handle 两请求、新 run-id 重复成功、F08 generation-guard、资源 guard、native counters 和 drain。[convergence](evidence/b189-convergence.md) |

## Task checklist

- [x] T001 [US1] Freeze the remaining production integration and candidate boundary.
- [ ] T003 [US1] Connect topology-independent Qwen preparation, Repo publication and reference-only reuse.
- [ ] T005 [US2] Verify real ACK-driven planning and authenticated Selection at production ingress.
- [ ] T006 [US4] Fetch selected Repo materials, assemble bounded native CPU runners, and expose native resource counters.
- [ ] T007 [US3] Validate real handoff, causal events and independent terminal output.
- [ ] T009 [US5] Qualify the resource envelope, complete MiniNDN path, same-handle reuse and independent repeat.

## Audit follow-up registry

这些是现有能力任务下的稳定子出口，不是新的行政任务，也不改变 Spec189 的六项
能力任务计数。每项都必须有 C++ production target/selector 和独立失败边界；未完成
时保持所属 T 项 `PARTIAL`。

| Subtask | Finding | Owner / dependency | Status | Exit evidence |
| --- | --- | --- | --- | --- |
| T003-R1 | F02 preparation peak | Runtime/ONNX preparation owner; before T009 full model | FOCUSED_CXX_PASS / QUALIFICATION_OPEN | C++ selector records source/material/encryption/ORT budget categories and post-publication cancel/retry cleanup; actual ORT allocator/RSS and real Qwen remain open; [F02 evidence](evidence/b189-f02-memory-20260919.md) |
| T003-R2 | F05 mixed quota reservation | RepoCore range/vector/Data packet admission; before protected candidate | FOCUSED_CXX_PASS | C++ mixed range/vector/Data selector passed; replacement/failure rollback and protected candidate remain open; [F05 evidence](evidence/b189-f05-quota-20260919.md) |
| T003-R3 | F09 fd error ownership | FilesystemRepoStoreBackend error path; before protected candidate | FOCUSED_CXX_PASS | injected fsync/close failure, one-owner/no-duplicate-close and preserved manifest boundary; [F09 evidence](evidence/b189-f09-fd-owner-20260919.md) |
| T009-R1 | F08 turn owner race | Conversation/PreparedModel handle installation; after T007 and before same-handle PASS | PLANNED | C++ barrier interleaving where older terminal/exception cannot overwrite or close newer turn |
| F03/F04 follow-up | catalog snapshot/history gap | Only if catalog snapshot/delta becomes a candidate caller | DEFERRED | snapshot-required + incarnation/oldest-sequence C++/Python sync evidence |
| F06/F07 follow-up | compatibility replacement/durability | Legacy helper/backend maintenance, not current native protected path | DEFERRED | separate compatibility task and failure model; no Spec189 PASS credit |

## Retired task IDs

合并不代表完成，旧 ID 不再单独领取或勾选为 PASS。

| Former ID | Disposition | Preserved obligation |
| --- | --- | --- |
| T002 | MERGED_INTO T003 | Qwen graph/config/digest 验证、原子层/shared 材料生成、staging cleanup |
| T004 | MERGED_INTO T003 | reference-only、两请求无新增发布、stale/released/oversized negatives |
| T010 | MERGED_INTO T009 | 独立重复、候选一致性、最终 verdict 与文档交付 |
| T008 | MERGED_INTO T009 | host guard、受控 stop、native resident/runner counters、完整采样与 drain；小型 guard/lifecycle 证据保留在 resource record |

<a id="t001"></a>
## T001 — Freeze integration boundary

**Read**: 最新 failure-log/raw boundary、架构阅读集、当前 Spec、global dependency receipt。
**Write**: `evidence/b189-convergence.md`；不另建 gate 框架。

1. 用 CodeGraph 固定 Runtime::prepare→RepoSourceProvider→DI_NativeRequester→
   NativeCanonicalOnnxAssembler→NativeProviderHandler/NativeEpochCoordinator 的实际接线与缺口。
2. 标明生产 CLI、独立 C++ assertion target、实际 Waf target/源码闭包；
   核对已有全局 ABI/receipt，只有缺失/改变/不兼容才安装或重建。
3. 固定 model revision、动态 KV schema、服务角色/key registry、拓扑、资源阈值、
   摘要派生入口和 handoff endpoint/attempt/sequence。最终 binary hash 在批末更新，
   不要求尚未实现的验收先通过。

**Acceptance**: 五 lane 的已验/待改/待测映射可执行，无循环依赖；
不因文档修改/run-id 改变重新全量构建，不授予产品 PASS。

## Retired T008 — cross-cutting gate

T008 不再作为独立能力任务。它保留的义务由实际 owner 承担：T003/T006 在各自
selector 中交付 native resident/materialization/lease/runner counters 和取消回收
反例，T009 在每次 full-model run 前调用现有 host guard，并在成功或分类停止后
统一检查采样、child 状态和 drain。`evidence/b189-resource.md` 保留已有 39 个
host checks、lifecycle fixture 和真实 identity 记录；这些证据不单独授予产品 PASS。

资源门规则：阈值来自 immutable profile，超过阈值必须分类为 `RESOURCE_BOUNDARY`；
只清理本 run staging，保留原始日志和有效 Repo 对象证据；`finally`/`kill` 本身
不等于 lifecycle PASS。由于这部分不新增生产能力，不再为它单独建批次或重复编译。

<a id="t003"></a>
## T003 — Prepare and reuse Repo materials

**Write**: `Runtime.cpp`、`NativeCanonicalPreparationCatalog.*`、
`NativeCanonicalArtifactPublisher.*`、定义 NativeCanonicalSource 的
`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp`、
`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoSourceProvider.hpp`、
`examples/DI_NativeRequester.cpp`；已有 Repo/PreparedModel C++ selectors；
`evidence/b189-prepare.md`。按实际定义路径修改，不复制 Qwen API。

共享 protected 接缝另涉及 `ndn-service-framework/ServiceUser.{hpp,cpp}`、
`EncryptedLargeDataRangeStore.hpp` 和 Repo `RepoEncryptedLargeDataStore.hpp`。
先按 B189-1a 验证 protected Repo 接缝，再按 B189-1b 实现原子材料；Core 不依赖
DI/Repo 类型。T003 不负责 ACK/Selection ingress，该边界属于 T005。
`NativeCanonicalArtifactPublisher::CacheState::prepared` 当前只保留 receipt 和 weak
serving pins；`PreparedModelPackage`/活动 request 才是强 owner。复用
ModelPreparationCache 预算/淘汰作为唯一保留策略，publisher 不成为第二个无界强 owner，
活动 package/request 仍保证可读。B189-1a 只验证受保护接缝和真实 package/cache owner
反例；B189-1b 才冻结原子材料 schema，不把未来 schema 当作当前 API。
C++ 反例必须走真实 publisher→package→淘汰路径，不能只手动 reset Core token。
B189-1a 当前 worker/cancel/key 与 Repo identity 静态门、组合构建和受保护
package/cache owner runtime selector 已通过；这只关闭本地 protected publication
接缝，不能把它写成 T003 完成。
具体出口与反例见 [bounded commit](contracts/model-preparation.md#bounded-commit-and-identity-ownership)。

**Audit follow-up exit B189-1c**：T003-R1 记录 source/initializer/material/encryption/ORT
各类 peak owner 与取消/重试清理；T003-R2 让 range reservation、普通 vector/Data packet
写入和 replacement 共用逻辑 quota；T003-R3 对 fsync/close failure 验证单一 fd owner。
三个出口必须使用 C++ production selector，分别记录首个失败边界，不能由 Python
脚本或磁盘剩余空间检查替代。

1. pinned canonical graph/initializer 生成拓扑无关原子层与 shared tensor 引用。
   层→节点/权重范围由 graph 推导并校验；embedding/final/tied weights 按内容去重。
   不能把预导出的 [0,14)/[14,28) 最终模型当 prepare 格式。
2. 复用 Repo manifest/payload owner/文件后端，最小版本化扩展 layer→graph/tensor
   object 或受验证 byte-range 的 digest/size/依赖关系。读单元有界，
   发布不再累积整份 vector；旧 schema 明确拒绝或走受测兼容路径，不能静默降级。
3. 将实际 requester 接入 Runtime Repo publisher/source 生命周期；对象持久化、
   manifest commit 且正常 Repo 读取可达后才 READY。source owner 可释放，
   服务与活动 lease 存活；不可达/对象丢失明确失败，request 不隐式重发模型。
4. 同一 immutable identity 的重复 prepare 命中已有 manifest/reference，且不会
   固定 Provider placement；同 handle 的两次请求和 publication/ingest 零增量由
   T009 的真实 MiniNDN 资格统一证明，避免在 prepare selector 中重复模拟最终链路。
5. 复用已通过 selector，仅补 real-Qwen receipt、源释放、stale manifest、
   digest/range/schema、staging rollback 和必要 envelope negatives。
   离线 snapshot→canonical 导出可复用，但不替代 native prepare/Repo。

**Acceptance**: C++ production entry 验证真实 Repo 材料、commit/可达性、源释放和
immutable prepare lookup。完整 Qwen run 由 T009 先通过资源门；此处不要求两
Provider 执行成功，也不重复证明最终同 handle 两请求。F02/F05/F09 的 B189-1c
出口未通过前，不能把完整候选标为 ready。

<a id="t005"></a>
## T005 — Authenticate placement

**Write**: NativeRequestPreparation/Envelope、必要 Core/NativeProviderHandler 接线、
`tests/integration-tests/spec189-placement-oracle.t.cpp`；
`evidence/b189-placement.md`。

1. 真实 ACK offers 后由 planner 决定两个覆盖范围；profile 可约束 [0,14)/[14,28)，
   不能改变 prepare manifest 或注入假 ACK/Selection。
2. 使用已有 typed projection/签名/canonical grant identity builder，
   绑定 manifest、Provider、role/range、attempt/epoch、plan digest。
3. 生产 Selection ingress 验证无 offer、stale epoch/digest、未选 Provider、
   overlap/out-of-range；实际 fetch/runner factory 计数为零。
4. 修正现有 selector 的 CPU backend/ABI 和 synthetic grant fixture；
   保留其组件价值，禁止用 cache-layer hit 证明网络授权已验。

**Acceptance**: 真实 signed path 与生产 ingress C++ assertions 通过；
无效选择无重型副作用，不依赖 T006 模型执行形成循环。

<a id="t006"></a>
## T006 — Materialize selected ranges

**Write**: NativeCanonicalOnnxAssembler、NativeOnnxAssemblyWorker、provider/Repo adapter、
已有 assembly/ownership selectors；`evidence/b189-execution.md`。

1. 实际 consumer 使用 T003 同一 manifest/receipt，授权后读取选定层与显式 shared
   tensors；替代整 initializer 下载再切片，未接线不能隐式回退旧路径。
2. 有界读取/文件物化与 RAII lease；digest/size/range/role/manifest 验证后才进 ORT。
   记录实际 fetched bytes、resident buffers、mapped files、runner owner，
   不能只用 cache.stats 或 RSS 代替源对象释放证明。
3. 错包/取消/组装失败测试无半成品 runner、文件/lease/worker 泄漏；
   保留已验 endpoint-preservation 回归，不重新实现。
4. 不可变内容可缓存复用，每请求重验授权；KV/会话与 runner 生命周期分离，
   不强制 warm request 重建 runner，不长期持有整源。

**Acceptance**: 实际 provider factory 用 Repo 选中材料构造 CPU runner，
没有每 Provider 整模型临时副本；共享 bytes 与失败/回收出口可核对。

<a id="t007"></a>
## T007 — Validate handoff and output

**Write**: 必要 NativeProviderHandler/NativeEpochCoordinator 修复、
`examples/Spec189TwoProviderOracle.cpp` 与已有 C++ handoff fixtures；
`evidence/b189-execution.md`。

1. 真实 Core/NDN hidden-state handoff 核对 endpoint digest、attempt/model/role/
   sequence、shape/dtype；不直接跨节点传内存对象。
2. 按 [placement contract](contracts/placement.md) 修正事件 checker：
   model assembly 与 upstream fetch 可交错，首段没有 upstream dependency；
   authorization/runner/input 均就绪才 execute，末段响应后所有 owner drain。
3. 覆盖成功、cache-hit、缺事件、错误因果/identity 的最小 C++ fixture。
   生产 CLI 和日志 checker 均不单独证明模型正确。
4. 冻结短输入及独立 reference，以 C++ 检查 shape/finite、
   冻结 logit tolerance 或 top-token 期望；digest 只作身份。
   保留 cancel/provider stop/stale handoff 反例，不扩张质量或 KV 性能工程。

**Acceptance**: 原生 handoff 与独立输出判据可执行，checker 不误拒合法次序且拒绝异常；
最终真实 Qwen 资格仍由 T009 负责。

<a id="t009"></a>
## T009 [US4, US5] — Qualify resource envelope, reuse and repeat

**Write**: 维护的 MiniNDN runner、单进程 C++ requester/driver 复用入口及证据 checker；
raw logs 放唯一 `.codex-tmp` run 目录；`evidence/b189-convergence.md`；
失败同步 failure-log。

1. 在 T003/T005/T006/T007 runtime 出口通过后，冻结实际源码内容、global ABI、模型/
   manifest、profile/topology、binaries/oracle；自动派生摘要，检查 policy/key/module/disk。
2. 两 CPU Provider，native prepare→Repo→ACK→planner/Selection→按需组装→
   NDN handoff→有效输出→drain。必须由一个长期存活的 C++ requester/driver
   在同一 Runtime/PreparedModel handle 上先 prepare 一次，再提交两个独立
   request；不能用脚本启动两个独立 requester 进程来替代。publication 增量为零，
   两个结果均通过独立 C++ oracle。
3. 原始证据落盘后保持同 candidate，用新 run-id 重复上述场景；
   request id/key/临时路径属于 run identity，不使 candidate 摘要变化。
4. 每次运行先通过 host guard，再比较 fetched bytes/cache/runner、RSS/Repo resident/
   物化峰值和 post-drain baseline。
   warm cache 可复用 runner；不能为凑计数而强制重建。
5. 失败保留第一已证实边界/未知部分，修复复审受影响范围再复测。T007 必须先
   提供独立固定输入的 C++ numerical/output oracle；token schema、digest 或
   CLI 日志不能替代它。
   实现引起的 API/行为变化按 Design/MANAGEMENT.md 同步契约/PDF/文档交付。
6. T009-R1 用 C++ 屏障控制旧 turn terminal/exception、新 turn start 和 handle
   installation；generation 不匹配时旧 turn 不得覆盖 active owner 或被 close 取消。
   该门通过前不能把“同 handle 两请求”仅凭两个结果文件认定为复用 PASS。

**Acceptance**: 两独立运行均成功，且各自同 handle 两请求/输出/资源/drain 齐全，
才 `QWEN_TWO_PROVIDER_PASS` / [x]。classified failure 不算完成。无 SIF/Tiger/27B 工作。

## Logical Batches and Dependencies

执行顺序：`B189-0 → B189-1 → B189-2 → B189-3 → B189-5`。
保留历史 ID 稳定链接；序号不再代表时间。
成员、五 lane、动态检查、唯一结果记录见 [batch-execution.md](batch-execution.md)。
达到批次出口即验证，不为了少编译加入新职责。
