# Implementation Plan: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Branch**: `Experimental` | **Feature**: `189-qwen-two-provider-minindn` | **Date**: 2026-09-18
**Spec**: [spec.md](spec.md) | **Status**: IN_PROGRESS

## Summary

目标仍是在本机 12 GB 上完成 Qwen3-0.6B 两 CPU Provider 的真实 MiniNDN 请求。
本次收敛准备/规划边界、Repo producer/consumer 接线、事件判据与资源门顺序；
保留已验代码，不重做 Spec185/188，不扩张 SIF/Tiger。

Repo adapter 已有层 payload/冷热 receipt 测试；requester protected range-store
配置草稿尚未验收，assembler 层材料消费未闭合，仍获取完整 initializer。
r25 FAIL 已观察 assembly entry，未证明两 Provider 成功执行。
见 [audit correction](evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction)。

## Technical Context

- C++ owns preparation/publication/authorization/assembly/execution and behavior assertions.
- Python owns MiniNDN orchestration/host sampling/offline conversion only.
- 全局依赖按 [declared closure](../../docs/native-dependency-closure.md)；
  本机 Boost 为 /usr/include + /usr/lib/x86_64-linux-gnu 1.71，其余按声明全局根（含 ORT）。
  缺失/不兼容才安装，禁止 checkout/.codex-tmp 前缀覆盖。
- 复用已验 Waf tree，受影响 target 增量构建默认 -j4，swap 压力按仓库规则降并发。
  不为每个新 selector 复制整套 DI 编译闭包。
- 两 Provider，固定短输入与少量 token；独立正确性判据必需，广泛质量评测不在范围内。

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

T008 的 host guard/受控 stop 前置所有真实模型准备/发布/MiniNDN；其 safety entry
已可独立复用，剩余 counters 由 T003/T006 的真实 owner 交付，不新增行政批次。
新 native counters 随 T003/T006 的 owner 接入，不能倒过来阻断其小 fixture。
T009 前核对完整采样，运行中记录峰值。RESOURCE_BOUNDARY 是诊断，不是协议失败或完成。

### AD-07: candidate separate from run identity

不可变 candidate 包含 source content、global ABI、binaries、model/manifest、
profile/topology/oracle；commit/build path 是 provenance。
run-id/request id/运行期 key/path 独立记录并绑定 candidate。
只重验变更影响的后继证据，文档改动或新 run-id 不使有效 build/model 证据失效。

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

## Logical Batch Quality Plan

每批 native selector 运行前检查 `readelf`/`ldd` 实际加载的 DI/Core 库及 SHA-256。
本机 global-first RUNPATH 会优先使用 `/usr/local`；若本批生成的共享库与全局库不同，
先安装对应目标并核对一致，再测试。头文件结构变化但 SONAME 未变也必须执行此门。
不通过临时 LD_LIBRARY_PATH 绕过，不因单一 DI 安装变化重编未变 Core/Repo。
Waf 原生 install 与附带 Python editable hook 分开记录；后者失败不能写 binding PASS。

唯一成员注册表见 [batch-execution.md](batch-execution.md)；
顺序 B189-0 → B189-4 → B189-1 → B189-2 → B189-3 → B189-5。
7 个活动任务及旧 ID 合并映射见 [tasks.md](tasks.md)。
B189-1a protected storage、B189-1b atomic preparation 各有独立验证出口，
在同一能力任务/证据内顺序执行，不等全部 T003 完成才首次构建。

每小任务完整编码/fixture/调用方/build registration 后冻结五 lane review，
修复复审，再同批组合审查，最后一次增量构建和规定 C++ 测试。
等待期间不实现依赖该门的下一任务；批次达稳定出口即测试，不无限扩张。
T008/T006 的 small-fixture owner/cancel 适用 ASan/UBSan；
依赖不支持时记录限制并运行具体 owner/counter 反例，不能省略生命周期测试。
12 GB full-model run 不强制 sanitizer。

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

T001 收敛接线 → T008 安全出口 → T003 prepare/Repo/request → T005 选择门 →
T006/T007 范围组装/handoff 组合验证 → T009 同 handle 两请求及独立重复。
每次失败保留原始边界，遵守
[experiment retry loop](../../skills/speckit-code-design/references/experiment-static-review-loop.md)；
日志级别/timeout 改变不算功能修复，诊断运行必须注明目的。

## Closure rule

所有真实验收与文档交付完成才结束 Spec189；分类失败仍 PARTIAL。
实现中的 API/行为变化按 Design/MANAGEMENT.md 同步当前/目标契约及 PDF；
本轮审计只修正文档；已有未验证 range-store/lease API 草稿保持 PARTIAL，
其源码/API/中文契约与 PDF 同步属于 B189-1a 交付，不冒充已验当前设计。
