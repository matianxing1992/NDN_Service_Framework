# Tasks: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Status**: IN_PROGRESS
**Input**: [spec.md](spec.md), [plan.md](plan.md), [batch-execution.md](batch-execution.md)
**Rule**: Production C++ → C++ assertions → Python orchestration。每任务编码、fixture/调用方/构建注册完成后冻结静态审查；同批组合通过再统一增量构建测试。静态通过不是完成。

## Current Checkpoint

**Requester Repo checkpoint**: 2026-09-19 01:17 -05:00 — T003 source owner 首次 external initializer 重复读取已修复，r6 复审及增量构建通过；Repo C++ 5/5，私有 spool 下 Runtime production-entry 1/1。requester 接线已构建但新 DI 全局库尚未安装，两个调用方的混合改动尚未归档；真实跨节点读取/Qwen/MiniNDN 保持 PARTIAL。受保护 publication 继续走 Core ServiceUser；详见 [requester Repo evidence](evidence/b189-requester-repo-20260919.md)。

**Updated**: 2026-09-18 17:49 -0500 — B189-1b producer and Repo-side selected consumer passed static review, the affected `unit-tests` target built with `-j4`, and four Repo C++ cases passed. Protected Provider ingress, real Qwen preparation and assembly remain open; three older Spec182 publisher lifecycle selectors remain unresolved. See [B189 prepare evidence](evidence/b189-prepare.md) and [failure log](../../docs/failure-log.md).
**Baseline**: `3e53fec5` plus pre-existing implementation and unvalidated protected-store draft; not a clean qualified candidate.

B189-1a publisher weak-pin、Repo identity fence 和 Core worker/cancel/key release
均已获官方只读 `STATIC_PASS`；真实 package owner 反例仍待补，尚未构建或运行。
尚未组合构建/测试；详见 [prepare evidence](evidence/b189-prepare.md)。

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
T001 映射已关闭；T008 direct/launcher guard 已复审，39 host checks 通过；
两个 C++ 目标 -j4 构建成功，4 个 lifecycle 用例三轮通过。真实路径 native counters
及完整资源回收仍待验证，T008 保持 PARTIAL；未运行模型。磁盘现约 34 GiB 可用。
文档修订已获冻结 v2 的 DOCUMENTATION_STATIC_PASS；11/11 技能入口、7 task ID、
25 FR、链接/锚点及 diff 检查通过，详情见上述审计记录。产品验收保持 PARTIAL。

## Execution Progress

7 项是能力任务，不按数量计算产品百分比。T003 两个独立执行出口见
[bounded execution units](batch-execution.md#bounded-execution-units)，当前下一步为 B189-1a。
每个出口验证后立即记录，不等整项 T003 写完才第一次构建。
T008 的 host guard 和小型 lifecycle safety entry 已达到其前置出口；剩余 native
counter 接入属于 T003/T006 的实际 owner，不能另开重复的 T008 实现批次，T008
只在 T009 前以完整采样和 drain 证据收口。
本轮文档 v2 已获 DOCUMENTATION_STATIC_PASS；结构/25 FR/链接与技能同步检查通过，
见 [follow-up verification](evidence/spec189-static-audit-20260918.md#follow-up-verification)。
受保护接缝草稿已通过 B189-1a 静态门；原子材料 producer/consumer 仍未审查，
本轮未构建或运行模型。

| Unit / Details | Status | Depends | Remaining exit / Evidence |
| --- | --- | --- | --- |
| [T001 Freeze integration boundary](#t001) | DONE | — | 2026-09-18 13:29 -0500：真实接线/候选/后继缺口及五 lane 映射已只读审查；仅关闭实施边界。[convergence](evidence/b189-convergence.md) |
| [T008 Guard before model runs](#t008) | PARTIAL | T001 | 2026-09-18 14:11 -0500：direct/launcher guard 39 checks PASS；4 native lifecycle cases 三轮 PASS；真实 counter 采样/全链 drain 未验。T003 小 fixture 可继续，full model 仍受门禁。[resource](evidence/b189-resource.md) |
| [T003 Prepare and reuse Repo materials](#t003) | PARTIAL | T001; T008 before full-model run | 2026-09-18 17:49 -0500：B189-1b producer 与 Repo-side selected consumer static/build/fixture PASS；protected Provider ingress、真实 Qwen source release 与后续 ACK/assembly 仍待完成。[prepare](evidence/b189-prepare.md) |
| [T005 Authenticate placement](#t005) | PARTIAL | T003 | ACK 后规划、signed Selection、生产 ingress no-fetch。[placement](evidence/b189-placement.md) |
| [T006 Materialize selected ranges](#t006) | PARTIAL | T005 | Repo consumer、有界组装、owner/cancel。[execution](evidence/b189-execution.md) |
| [T007 Validate handoff and output](#t007) | PARTIAL | T006 static gate | 因果 oracle、NDN hidden-state handoff、独立输出判据。[execution](evidence/b189-execution.md) |
| [T009 Qualify reuse and repeat](#t009) | PARTIAL | T008 + B189-1/2/3 runtime exits | 同 handle 两请求、新 run-id 重复成功、资源/drain。[convergence](evidence/b189-convergence.md) |

## Task checklist

- [x] T001 [US1] Freeze the remaining production integration and candidate boundary.
- [ ] T008 [US4] Verify resource admission, sampling and deterministic drain before full-model runs.
- [ ] T003 [US1] Connect topology-independent Qwen preparation, Repo publication and reference-only reuse.
- [ ] T005 [US2] Verify real ACK-driven planning and authenticated Selection at production ingress.
- [ ] T006 [US3] Fetch selected Repo materials and assemble bounded native CPU runners.
- [ ] T007 [US3] Validate real handoff, causal events and independent terminal output.
- [ ] T009 [US5] Qualify the complete MiniNDN path, same-handle reuse and independent repeat.

## Retired task IDs

合并不代表完成，旧 ID 不再单独领取或勾选为 PASS。

| Former ID | Disposition | Preserved obligation |
| --- | --- | --- |
| T002 | MERGED_INTO T003 | Qwen graph/config/digest 验证、原子层/shared 材料生成、staging cleanup |
| T004 | MERGED_INTO T003 | reference-only、两请求无新增发布、stale/released/oversized negatives |
| T010 | MERGED_INTO T009 | 独立重复、候选一致性、最终 verdict 与文档交付 |

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

<a id="t008"></a>
## T008 — Guard before model runs

**Write**: `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py` 和已有 launcher、
现有 C++ 生命周期 fixture；`evidence/b189-resource.md`。

1. 在真实模型准备/发布前检查 MemAvailable/disk；每秒采样 RSS、swap、disk、
   child 状态及 native Repo resident/materialization/lease/runner counters。
   阈值来自 profile，不静默放宽。
2. 超阈值/deadline/child failure 走统一 bounded stop：取消、限时 drain、
   必要时升级进程组终止；只清理本 run staging，保留有效 Repo 对象与证据。
3. 小型 C++ fixture 验证 Face/io_context/worker/callback owner 与 drain；
   受控阈值触发 host guard，禁止以真实 OOM 测保护。Python 仅测采样/进程控制。
4. 完整模型峰值/正常 drain 交 T009，避免安全门依赖尚不能安全运行的 full model。
5. 前置安全出口只要求 host guard、受控停止和已有小型 owner/drain fixture。
   新 native counters 随 T003/T006 的实际 owner 实现并测试，T009 前必须接入采样；
   不能因它们尚未实现而阻止 T003 小 fixture。T008 最终勾选仍需完整计数器证据。

**Acceptance**: guard 先于昂贵工作生效，受控停止正确分类且无 child/fixture 泄漏；
finally/kill 本身不等于 lifecycle PASS。

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
先按 B189-1a 验证接缝，再按 B189-1b 实现原子材料；Core 不依赖 DI/Repo 类型。
`NativeCanonicalArtifactPublisher::CacheState::prepared` 当前只保留 receipt 和 weak
serving pins；`PreparedModelPackage`/活动 request 才是强 owner。复用
ModelPreparationCache 预算/淘汰作为唯一保留策略，publisher 不成为第二个无界强 owner，
活动 package/request 仍保证可读。B189-1a 只验证受保护接缝和真实 package/cache owner
反例；B189-1b 才冻结原子材料 schema，不把未来 schema 当作当前 API。
C++ 反例必须走真实 publisher→package→淘汰路径，不能只手动 reset Core token。
B189-1a 当前 worker/cancel/key 与 Repo identity 静态门已通过；仍须以真实
package/cache owner 反例和组合 runtime 验证受保护接缝，不能把静态通过写成 T003 完成。
具体出口与反例见 [bounded commit](contracts/model-preparation.md#bounded-commit-and-identity-ownership)。

1. pinned canonical graph/initializer 生成拓扑无关原子层与 shared tensor 引用。
   层→节点/权重范围由 graph 推导并校验；embedding/final/tied weights 按内容去重。
   不能把预导出的 [0,14)/[14,28) 最终模型当 prepare 格式。
2. 复用 Repo manifest/payload owner/文件后端，最小版本化扩展 layer→graph/tensor
   object 或受验证 byte-range 的 digest/size/依赖关系。读单元有界，
   发布不再累积整份 vector；旧 schema 明确拒绝或走受测兼容路径，不能静默降级。
3. 将实际 requester 接入 Runtime Repo publisher/source 生命周期；对象持久化、
   manifest commit 且正常 Repo 读取可达后才 READY。source owner 可释放，
   服务与活动 lease 存活；不可达/对象丢失明确失败，request 不隐式重发模型。
4. 同 Runtime prepare 一次、同 handle 请求两次，publication/ingest 增量为零；
   重复 prepare 命中完整 immutable identity。
5. 复用已通过 selector，仅补 real-Qwen receipt、源释放、stale manifest、
   digest/range/schema、staging rollback 和必要 envelope negatives。
   离线 snapshot→canonical 导出可复用，但不替代 native prepare/Repo。

**Acceptance**: C++ production entry 验证真实 Repo 材料、commit/可达性及复用。
完整 Qwen 操作先过 T008，小 fixture 可先运行；此处不要求两 Provider 执行成功。

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
## T009 — Qualify reuse and repeat

**Write**: 维护的 MiniNDN runner/C++ requester 复用入口及证据 checker；
raw logs 放唯一 `.codex-tmp` run 目录；`evidence/b189-convergence.md`；
失败同步 failure-log。

1. T008 与 B189-1/2/3 runtime 出口通过后，冻结实际源码内容、global ABI、模型/
   manifest、profile/topology、binaries/oracle；自动派生摘要，检查 policy/key/module/disk。
2. 两 CPU Provider，native prepare→Repo→ACK→planner/Selection→按需组装→
   NDN handoff→有效输出→drain。同 requester 进程只 prepare 一次，用同 handle
   做两个独立 request，publication 增量为零且两个结果均通过 C++ oracle。
3. 原始证据落盘后保持同 candidate，用新 run-id 重复上述场景；
   request id/key/临时路径属于 run identity，不使 candidate 摘要变化。
4. 比较 fetched bytes/cache/runner、RSS/Repo resident/物化峰值和 post-drain baseline。
   warm cache 可复用 runner；不能为凑计数而强制重建。
5. 失败保留第一已证实边界/未知部分，修复复审受影响范围再复测。
   实现引起的 API/行为变化按 Design/MANAGEMENT.md 同步契约/PDF/文档交付。

**Acceptance**: 两独立运行均成功，且各自同 handle 两请求/输出/资源/drain 齐全，
才 `QWEN_TWO_PROVIDER_PASS` / [x]。classified failure 不算完成。无 SIF/Tiger/27B 工作。

## Logical Batches and Dependencies

执行顺序：`B189-0 → B189-4 → B189-1 → B189-2 → B189-3 → B189-5`。
保留历史 ID 稳定链接；序号不再代表时间。
成员、五 lane、动态检查、唯一结果记录见 [batch-execution.md](batch-execution.md)。
达到批次出口即验证，不为了少编译加入新职责。
