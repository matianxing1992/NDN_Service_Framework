# Implementation Plan: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Branch**: `Experimental` | **Feature**: `189-qwen-two-provider-minindn` | **Date**: 2026-09-18
**Spec**: [spec.md](spec.md) | **Status**: IN_PROGRESS

## Summary

目标仍是在本机 12 GB 上完成 Qwen3-0.6B 两 CPU Provider 的真实 MiniNDN 请求。
本次只收敛本机真实 Qwen 两 Provider 请求的剩余闭环：Repo producer/consumer
接线、ACK/Selection 后的材料消费、事件判据和资源边界。保留已验代码，不重做
Spec185/188，不扩张 SIF/Tiger。

Repo adapter 已有层 payload/冷热 receipt 测试，Runtime protected publication 与
assembler material-only consumer 已有局部 C++ 验证；requester source-owner 接线
已构建，全局 DI/Core 安装身份已核对。真实 Qwen 下的跨节点材料读取、两 Provider
执行、输出与 drain 仍未闭合，不能把组件结果提升为完整链路资格。
r25 FAIL 已观察 assembly entry，未证明两 Provider 成功执行；后续资源边界与本轮
组件结果以 tasks.md 及其证据为准。
见 [audit correction](evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction)。

审计对账已将 F01 标为局部修复、资格仍开放；F02、F08 仍是当前候选的开放生产门，
F05/F09 已有 focused C++ selector 但仍需完整候选边界，分别由 T003/T009 收口。F03/F04 只在采用 catalog snapshot/delta 时适用，
F06/F07 属于未用于本机 native protected 资格路径的兼容/本地 Python 后端；它们保留
为后续维护项，不能写成已修复，也不应把本次 Qwen 任务扩成通用 Repo 重构。

## Technical Context

- C++ owns preparation/publication/authorization/assembly/execution and behavior assertions.
- Python owns MiniNDN orchestration/host sampling/offline conversion only.
- 全局依赖按 [declared closure](../../docs/native-dependency-closure.md)；
  本机 Boost 为 /usr/include + /usr/lib/x86_64-linux-gnu 1.71，其余按声明全局根（含 ORT）。
  缺失/不兼容才安装，禁止 checkout/.codex-tmp 前缀覆盖。
- 复用已验 Waf tree，受影响 target 增量构建默认 -j4，swap 压力按仓库规则降并发。
  不为每个新 selector 复制整套 DI 编译闭包。
- 两 Provider，固定短输入与少量 token；独立正确性判据必需，广泛质量评测不在范围内。

## Audit correction — local candidate first

本 Spec 的成功条件是一次本机可重复的真实请求；候选专用的 Qwen layer map、
profile、resource budget、MiniNDN topology 和 `spec189-two-provider-oracle`
都是本地验收输入，不是新的 NDNSF 全局 API、Core/Repo 默认值或长期协议。除非
实际修改公开签名、跨模块数据契约或安全语义，否则不更新全局设计来承载这些
临时字段；需要跨 Spec 的 API 变化必须另开设计变更，而不是在本 Spec 中顺手
泛化。

原生 C++ 负责 source identity、material manifest、prepare、授权、assembly 和
行为断言。实验脚本中的 ONNX helper 只能做候选预检、资源采样和进程编排；它的
digest 不是 native identity 的权威来源，native prepare 必须再次独立检查同一
source。r27 的 Python identity 资源边界已修复，下一步先以新的受控 run 观察
native prepare 的第一边界；若仍是 source 内存峰值，新增的是一个有界修复单元，
不借机重写全局 Repo 或 DI 缓存架构。

资源 guard 是每个 full-model run 的前置/伴随门，不再作为独立能力任务。小型
guard/lifecycle 证据保留，native resident/runner counters 由其实际 owner
（T003/T006）交付，最终在 T009 收口。

## Constitution Check

设计修订遵循 C++ ownership、真实调用链、全局依赖与静态门；产品仍 PARTIAL。
文档一致性 PASS 不能替代 native/MiniNDN runtime。

## Architecture Decisions

### AD-01: topology-independent preparation

native prepare 验证 canonical graph/config/initializer，生成原子层和 shared tensor
内容索引，发布可经正常 Repo 路径读取的材料，commit 后返回现有 PreparedModel。
prepare 不固定最终 Provider 分区，不构造两段 runner；其他分区计划可复用同一材料。
本 Spec 只验两个 Provider，不增加其他拓扑验收。

复用 Repo manifest/payload owner/文件后端/事务，最小版本化扩展表达 layer→
对象或受验证 byte-range、digest/size/shared dependencies。
不新增 PreparedQwenModel 公开 API、Qwen 专用 Repo 或第二 serializer。

### AD-02: durable reference and availability

requester 经 Runtime 注入通用 ciphertext range store；Core 保持加密、签名与
NDN serving，Repo 负责持久范围存储。不能将 plain Repo publisher 直接替代
生产 encrypted fetch 的对端，见 [binding](contracts/model-preparation.md#protected-repo-integration-binding)。
prepare 返回前证明持久提交与可读性；模型源和临时 buffer 可释放，handle 持有 reference/lease。
Repo service 在请求期间有明确 owner，缓存符合预算；不可达/丢失明确报错，
request 不隐式重新发布或携带模型 payload。
publisher receipt cache 不得无界持有 serving lease；保留决策归现有
ModelPreparationCache 预算与淘汰，活动 package/request 持有必要 owner。

### AD-03: ACK determines partition; Selection authorizes materialization

真实 ACK offers 后由 planner 选择范围/Provider，signed Selection/grant 绑定
manifest、role/range、attempt/epoch、plan digest。初始 profile 可约束 [0,14)/[14,28)，
不改变 prepare 材料。无效/未选 placement 在重型 fetch/runner 前拒绝。
fixture 使用已有 canonical typed identity 与签名入口，不手拼替代授权。

### AD-04: selected materialization and bounded cache

Provider 仅读选中原子层及显式 shared tensors，有界读/文件物化后组装 ONNX。
禁止整 initializer 下载后切片与预加载整模型 runner。
不可变内容可共享，授权每请求验证；KV/会话/runner owner 与 Repo 持久材料分离。
cold/warm runner 创建数可以不同，但资源回收与计费必须可解释。

### AD-05: causal events and independent output

使用已有 Core/NDN hidden-state dependency；模型材料 fetch 与 upstream tensor
fetch 不同，assembly 和等待输入可交错，首段没有 upstream。
按 [causal contract](contracts/placement.md) 验证，不假设跨 Provider 日志全序。
已注册 Waf target `spec189-two-provider-oracle`（源码
`examples/Spec189TwoProviderOracle.cpp`，输出 `<build>/examples/spec189-two-provider-oracle`）是 C++ 日志/身份 checker，
需修正事件假设并配合独立输出 reference。计算 digest 本身不能证明推理正确。

### AD-06: safety before expensive work

Host guard/受控 stop 前置所有真实模型准备/发布/MiniNDN；其 safety entry 已可
独立复用，但不再创建行政任务，剩余 counters 由 T003/T006 的真实 owner 交付。
新 native counters 随 T003/T006 的 owner 接入，不能倒过来阻断其小 fixture。
T009 前核对完整采样，运行中记录峰值。RESOURCE_BOUNDARY 是诊断，不是协议失败或完成。

### AD-07: candidate separate from run identity

不可变 candidate 包含 source content、global ABI、binaries、model/manifest、
profile/topology/oracle；commit/build path 是 provenance。
run-id/request id/运行期 key/path 独立记录并绑定 candidate。
只重验变更影响的后继证据，文档改动或新 run-id 不使有效 build/model 证据失效。

### AD-08: audit findings have explicit applicability and owners

审计发现按当前 candidate path 分流：material-only consumer 的接线属于 T003/T006
并必须经真实 protected ingress 验收；准备峰值和混合 quota 属于 T003 的生产反例；
Conversation generation owner 属于 T009 的同 handle 两请求反例；filesystem fd 错误
路径属于 T003 的发布错误门。catalog snapshot/history、segmented compatibility 和
Python power-loss durability 不属于当前 native qualification 的默认路径，只有实际
调用方进入候选后才提升依赖；不得用本 Spec 的局部 selector 代替这些通用契约。

所有这些门都必须记录 `static`、`compile/link`、`runtime/test`、`unobserved` 四类
状态；未完成的 audit follow-up 不得通过降低资源阈值、增加 timeout 或复制 digest
来关闭。

## Production paths and design-to-code binding

| Binding | Actual source / caller | Remaining proof |
| --- | --- | --- |
| Q189-PREP | Runtime::prepare, NativeCanonicalPreparationCatalog/Publisher, DI_NativeRequester | 原子 Qwen 材料发布与真实 Repo 可达性 |
| Q189-REPO | NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoSourceProvider.hpp; RepoCore | receipt/事务/文件对象接入实际 producer/consumer |
| Q189-WIRE | PreparedModel, NativeRequestPreparation/Envelope, Core ACK/Selection, NativeProviderHandler | 同 handle 复用、ACK 后规划、生产 ingress no-fetch |
| Q189-ASSEMBLY | NativeCanonicalOnnxAssembler, NativeOnnxAssemblyWorker, provider factory | 选定材料替代整 initializer fetch |
| Q189-HANDOFF | NativeEpochCoordinator, NativeProviderHandler, Core dependency path | endpoint/attempt/tensor 绑定与 terminal/drain |
| Q189-ORACLE | examples/Spec189TwoProviderOracle.cpp + existing native selectors | 因果与独立输出校验；CLI 不等于 oracle |
| Q189-EVIDENCE | Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py and maintained launcher | guard、同 handle 两请求、新 run-id 重复成功 |
| Q189-AUDIT-OWNER | RepoCore/FilesystemRepoStoreBackend、Runtime/Conversation、T003/T009 C++ selectors | F02/F05/F08/F09 的真实 owner、失败/并发反例和 resource accounting；F03/F04/F06/F07 仅在适用调用方接入后提升 |

## Logical Batch Quality Plan

每批 native selector 运行前检查 `readelf`/`ldd` 实际加载的 DI/Core 库及 SHA-256。
本机 global-first RUNPATH 会优先使用 `/usr/local`；若本批生成的共享库与全局库不同，
先安装对应目标并核对一致，再测试。头文件结构变化但 SONAME 未变也必须执行此门。
不通过临时 LD_LIBRARY_PATH 绕过，不因单一 DI 安装变化重编未变 Core/Repo。
Waf 原生 install 与附带 Python editable hook 分开记录；后者失败不能写 binding PASS。

唯一成员注册表见 [batch-execution.md](batch-execution.md)；
顺序 B189-0 → B189-1 → B189-2 → B189-3 → B189-5。
6 个活动任务及旧 ID 合并映射见 [tasks.md](tasks.md)。资源门随 B189-1/B189-3
交付并在 B189-5 最终收口。
B189-1a protected storage、B189-1b atomic preparation 各有独立验证出口，
在同一能力任务/证据内顺序执行，不等全部 T003 完成才首次构建。

B189-1c 是 T003 的审计 follow-up 出口：准备峰值分类计数、混合 reservation quota
和 filesystem fd error ownership。它必须在真正的 protected candidate 之前完成；
不能用旧 B189-1a/B189-1b 组件结果替代。T009 增加 generation-guarded same-handle
交错门；F03/F04/F06/F07 只有实际调用方进入本候选时才加入依赖图。

每小任务完整编码/fixture/调用方/build registration 后冻结五 lane review，
修复复审，再同批组合审查，最后一次增量构建和规定 C++ 测试。
等待期间不实现依赖该门的下一任务；批次达稳定出口即测试，不无限扩张。
T006 的 small-fixture owner/cancel 适用 ASan/UBSan；
依赖不支持时记录限制并运行具体 owner/counter 反例，不能省略生命周期测试。
12 GB full-model run 不强制 sanitizer。每次 full-model run 仍必须通过 guard，
但不为 guard 单独建立批次或重复构建。

## Evidence reuse and invalidation

| Change | Retain | Recheck |
| --- | --- | --- |
| Docs/task order | 有效组件/build/model 证据 | 文档链接、任务依赖与验收一致性 |
| Material schema | 全局依赖与无关 Core | producer/consumer/schema negatives/受影响 MiniNDN |
| Placement/grant | 兼容 Repo 内容 | ingress/no-fetch/两 Provider 绑定 |
| Assembly/handoff | 未变 prepare 和依赖证据 | 原生 selector、输出/因果/lifecycle/full path |
| Global ABI | 无关文档与模型源 | 受影响目标/绑定/loader 及 downstream |
| New run-id | 同一 candidate | 新运行证据，不重编或重导出 |

## Formal validation order

T001 收敛接线 → T003 prepare/Repo/request → T005 选择门 →
T006/T007 范围组装/handoff 组合验证 → T009 同 handle 两请求及独立重复。
每次失败保留原始边界，遵守
[experiment retry loop](../../skills/speckit-code-design/references/experiment-static-review-loop.md)；
日志级别/timeout 改变不算功能修复，诊断运行必须注明目的。

## Closure rule

所有真实验收与文档交付完成才结束 Spec189；分类失败仍 PARTIAL。
实现中的 API/行为变化按 Design/MANAGEMENT.md 同步当前/目标契约及 PDF；
本轮审计只修正文档；已有未验证 range-store/lease API 草稿保持 PARTIAL，
其源码/API/中文契约与 PDF 同步属于 B189-1a 交付，不冒充已验当前设计。
